# EvHttp API Server - Systemd Installation Guide

This guide details how to deploy and run the pre-compiled EvHttp API server binary as a persistent `systemd` daemon on Ubuntu 24.04 or Ubuntu 26.04.

## 1. Install Runtime Dependencies
Ensure that all required shared libraries are natively installed on your host or VM.

### For Ubuntu 26.04
```bash
sudo apt-get update
sudo apt-get install -y libevent-2.1-7 \
                        libevent-pthreads-2.1-7 \
                        libjson-c5 \
                        libcurl4t64 \
                        unixodbc \
                        tdsodbc \
                        odbc-postgresql \
                        libsodium23 \
                        libqrencode4 \
                        liboath0
```

### For Ubuntu 24.04 (t64 ABI transition)
```bash
sudo apt-get update
sudo apt-get install -y libevent-2.1-7t64 \
                        libevent-pthreads-2.1-7t64 \
                        libjson-c5 \
                        libcurl4t64 \
                        unixodbc \
                        tdsodbc \
                        odbc-postgresql \
                        libsodium23 \
                        libqrencode4 \
                        liboath0t64
```

> [!NOTE]
> If you compile optimized runtime libraries (e.g. `libevent` or `libcurl`) from source into `/usr/local/lib`, verify that `/usr/local/lib` is included in `/etc/ld.so.conf.d/` and refresh the linker cache:
> ```bash
> sudo ldconfig
> ```

## 2. Deploy Binary and Directories
Deploy the pre-compiled application bundle, environment configuration, and upload directory:

```bash
# Create deployment directories and uploads directory
sudo mkdir -p /opt/evhttp/bin /opt/evhttp/systemd /opt/uploads

# Copy binary, environment configuration, and systemd service file from repo root
sudo cp bin/apiserver /opt/evhttp/bin/
sudo cp bin/apiserver.env /opt/evhttp/bin/
sudo cp systemd/apiserver.service /opt/evhttp/systemd/
sudo chmod +x /opt/evhttp/bin/apiserver

# Create an unprivileged system user if it does not already exist
id -u apiserver &>/dev/null || sudo useradd -r -s /bin/false apiserver

# Grant ownership of deployment and uploads directory to the unprivileged user
sudo chown -R apiserver:apiserver /opt/evhttp /opt/uploads
```

## 3. Configure the Environment
Ensure your configuration file exists next to the compiled binary.

```bash
cd /opt/evhttp/bin
# If the file does not exist, create it with your actual credentials
sudo nano apiserver.env
```

Ensure `apiserver.env` contains the required keys:
```env
# ODBC connection strings - you can define from DB_0 to DB_3
DB_0=Driver=FreeTDS;SERVER=DatabaseServerAddr;PORT=1433;DATABASE=demodb;UID=your_username;PWD=your_password;APP=apiserver;Encryption=off;ClientCharset=UTF-8
#DB_1=Driver={PostgreSQL Unicode};Server=DatabaseServerAddr;Port=5432;Database=testdb;Username=YourUsername;Password=YourPassword;ConnSettings=SET application_name='apiserver';TextAsLongVarchar=1;MaxLongVarcharSize=0;BoolsAsChar=0;

# enable access logs - can be changed on the fly and supports service reload
ACCESS_LOG=true

# server configuration - any change requires service restart
NUM_THREADS=80
MAX_QUEUE_SIZE=10000
FAST_POOL_PERCENTAGE=25
MAX_PAYLOAD_SIZE=5242880

# remote login provider
LOGIN_PROVIDER=http://demodb.mshome.net:8080
LOGIN_URI=/login

# jwt configuration

# secret and api key generated with: openssl rand -hex 32
JWT_SECRET=your_jwt_secret
JWT_TIMEOUT_SECONDS=300
TELEMETRY_API_KEY=your_telemetry_key

# trust haproxy IP for accepting X-Forwarded-For header
TRUST_PROXY_IP=127.0.0.1

# cors configuration
CORS_ALLOWED_ORIGINS=file://,null,https://cppserver.com

# uploads directory - must be writable by the apiserver user
UPLOADS_DIR=/opt/uploads

# remote backend API configuration
API_URL=https://cppserver.com
API_USER=your_api_user
API_PASS=your_api_pass
REMOTE_API_KEY=your_api_key
```

### ODBC Driver Configuration
Verify that the ODBC drivers referenced in `DB_0` and `DB_1` are registered in `/etc/odbcinst.ini`:

```ini
[FreeTDS]
Description=FreeTDS Driver
Driver=/usr/lib/x86_64-linux-gnu/odbc/libtdsodbc.so
Setup=/usr/lib/x86_64-linux-gnu/odbc/libtdsS.so
UsageCount=1

[PostgreSQL Unicode]
Description=PostgreSQL ODBC driver (Unicode version)
Driver=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
Setup=/usr/lib/x86_64-linux-gnu/odbc/libodbcpsqlS.so
UsageCount=1
```

*(If you compiled FreeTDS from source into `/usr/local/lib`, point the Driver path to `/usr/local/lib/libtdsodbc.so`)*

Also ensure `/etc/freetds/freetds.conf` includes:
```ini
[global]
    text size = 64512
    client charset = UTF-8
```

## 4. Advanced: Native Encrypted Secrets via `systemd-creds` (Optional)
If your customer or compliance policy requires sensitive credentials (`DB_0`, `JWT_SECRET`, `API_PASS`, etc.) to be **encrypted at rest** inside the container or host, you can leverage native `systemd` credentials without modifying the C application.

### How it Works
1. Secrets are encrypted at rest using `systemd-creds` bound to the machine's identity (`/etc/machine-id` or TPM2).
2. When the service starts, systemd decrypts the secrets into a transient, secure RAM filesystem (`ramfs`) accessible only by the service user at `$CREDENTIALS_DIRECTORY` (mode `0400`).
3. A lightweight 3-line launcher wrapper exports the decrypted files as standard environment variables right before executing the binary.
4. The API server reads them with `getenv()` and immediately scrubs them from memory and the environment table using `explicit_bzero()` and `unsetenv()`.

### Step 1: Encrypt Secrets at Rest
Generate encrypted credential files inside `/opt/evhttp/credentials.encrypted`:

```bash
sudo mkdir -p /opt/evhttp/credentials.encrypted

# Encrypt ODBC connection string
echo -n "Driver=FreeTDS;SERVER=DatabaseServerAddr;PORT=1433;DATABASE=demodb;UID=your_username;PWD=your_password;APP=apiserver;Encryption=off;ClientCharset=UTF-8" | \
    sudo systemd-creds encrypt --name=db_0 - /opt/evhttp/credentials.encrypted/db_0.cred

# Encrypt JWT secret
echo -n "your_jwt_secret" | \
    sudo systemd-creds encrypt --name=jwt_secret - /opt/evhttp/credentials.encrypted/jwt_secret.cred

# Encrypt API password
echo -n "your_api_pass" | \
    sudo systemd-creds encrypt --name=api_pass - /opt/evhttp/credentials.encrypted/api_pass.cred
```

### Step 2: Create the Launcher Wrapper
Create `/opt/evhttp/bin/apiserver-launcher.sh`:

```bash
sudo nano /opt/evhttp/bin/apiserver-launcher.sh
```

Add the following wrapper:
```sh
#!/bin/sh
# /opt/evhttp/bin/apiserver-launcher.sh

if [ -n "$CREDENTIALS_DIRECTORY" ]; then
    [ -f "$CREDENTIALS_DIRECTORY/db_0" ]       && export DB_0=$(cat "$CREDENTIALS_DIRECTORY/db_0")
    [ -f "$CREDENTIALS_DIRECTORY/jwt_secret" ] && export JWT_SECRET=$(cat "$CREDENTIALS_DIRECTORY/jwt_secret")
    [ -f "$CREDENTIALS_DIRECTORY/api_pass" ]   && export API_PASS=$(cat "$CREDENTIALS_DIRECTORY/api_pass")
fi

exec /opt/evhttp/bin/apiserver "$@"
```

Make the script executable:
```bash
sudo chmod +x /opt/evhttp/bin/apiserver-launcher.sh
```

### Step 3: Configure the Systemd Service Unit
In `/etc/systemd/system/apiserver.service`, add the `LoadCredentialEncrypted=` directives and update `ExecStart` to point to the launcher wrapper:

```ini
[Service]
...
WorkingDirectory=/opt/evhttp/bin
ExecStart=/opt/evhttp/bin/apiserver-launcher.sh

# Load encrypted credentials into $CREDENTIALS_DIRECTORY at runtime
LoadCredentialEncrypted=db_0:/opt/evhttp/credentials.encrypted/db_0.cred
LoadCredentialEncrypted=jwt_secret:/opt/evhttp/credentials.encrypted/jwt_secret.cred
LoadCredentialEncrypted=api_pass:/opt/evhttp/credentials.encrypted/api_pass.cred
...
```

> [!TIP]
> This pattern allows the binary to remain 100% 12-Factor and Cloud/Kubernetes-native (consuming standard environment variables), while satisfying enterprise at-rest encryption requirements under systemd.

## 5. Advanced: Enterprise Secret Management with HashiCorp Vault (Optional)
If your organization or customer uses **HashiCorp Vault** (or a compatible KMS/Secrets Manager), you can manage all sensitive credentials centrally while preserving the exact same `getenv()` application interface.

### The "Universal getenv" Architecture
A core design strength of this server is that **all configuration is consumed via standard environment variables**. The C application code remains identical across every target:
* **Local Development / QA:** Simple `.env` file or terminal export.
* **LXD / Bare-Metal (Native Systemd):** Encrypted at rest via `systemd-creds` (Section 4).
* **LXD / Bare-Metal (Enterprise IT):** Managed and rotated via HashiCorp Vault into `/run` tmpfs (this section).
* **Kubernetes / Cloud Native:** Injected via K8s `SecretKeyRef` or AWS/GCP/Azure Secret Managers.

Only the deployment and provisioning instructions change—never the application binary.

---

### How Vault Agent Works in LXD / Systemd
1. **`vault-agent`** runs inside the LXD container as an auxiliary systemd daemon.
2. It authenticates to the customer's Vault cluster using **AppRole** (the enterprise standard for non-human machine auth).
3. Vault Agent templates the secrets directly into a memory-only directory: `/run/apiserver/vault.env` (permissions `0400`, owned by `apiserver`).
4. Systemd reads `/run/apiserver/vault.env` via `EnvironmentFile=`.
5. The API server starts, reads the secrets via `getenv()`, and immediately wipes them from the process environment table with `explicit_bzero()`.
6. When secrets (e.g. database passwords) rotate in Vault, Vault Agent automatically executes `systemctl restart apiserver` to cleanly rebuild the ODBC connection pools.

---

### Step 1: Install Vault on Ubuntu (LXD Container)
Add the official HashiCorp repository and install the `vault` binary:

```bash
sudo apt-get update && sudo apt-get install -y gpg wget
wget -O- https://apt.releases.hashicorp.com/gpg | sudo gpg --dearmor -o /usr/share/keyrings/hashicorp-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/hashicorp-archive-keyring.gpg] https://apt.releases.hashicorp.com $(lsb_release -cs) main" | sudo tee /etc/apt/sources.list.d/hashicorp.list
sudo apt-get update && sudo apt-get install -y vault
```

---

### Step 2: Configure Vault Agent (`/etc/vault/agent.hcl`)
Create the Vault Agent configuration:

```bash
sudo mkdir -p /etc/vault
sudo nano /etc/vault/agent.hcl
```

```hcl
pid_file = "/run/vault-agent.pid"

vault {
  address = "https://vault.company.internal:8200"
}

auto_auth {
  method "approle" {
    config = {
      role_id_file_path   = "/etc/vault/role-id"
      secret_id_file_path = "/etc/vault/secret-id"
      remove_secret_id_file_after_reading = false
    }
  }
}

template {
  contents = <<EOF
{{ with secret "secret/data/apiserver/production" }}
DB_0="{{ .Data.data.db_0 }}"
JWT_SECRET="{{ .Data.data.jwt_secret }}"
API_PASS="{{ .Data.data.api_pass }}"
REMOTE_API_KEY="{{ .Data.data.remote_api_key }}"
TELEMETRY_API_KEY="{{ .Data.data.telemetry_api_key }}"
{{ end }}
EOF
  destination = "/run/apiserver/vault.env"
  perms       = "0400"

  # Restart apiserver upon secret rotation to rebind ODBC connection pools:
  command     = "systemctl restart apiserver"
}
```

Deploy the `role-id` and `secret-id` provided by the IT Security team:
```bash
echo "your-approle-role-id"   | sudo tee /etc/vault/role-id
echo "your-approle-secret-id" | sudo tee /etc/vault/secret-id
sudo chmod 0600 /etc/vault/role-id /etc/vault/secret-id
sudo chown -R apiserver:apiserver /etc/vault
```

---

### Step 3: Create the Vault Agent Systemd Unit
Create `/etc/systemd/system/vault-agent.service`:

```ini
[Unit]
Description=HashiCorp Vault Agent
After=network-online.target
Wants=network-online.target

[Service]
User=apiserver
Group=apiserver
RuntimeDirectory=apiserver
RuntimeDirectoryMode=0700
ExecStart=/usr/bin/vault agent -config=/etc/vault/agent.hcl
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

Enable and start the agent:
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now vault-agent.service
```

Verify that `/run/apiserver/vault.env` is populated with secrets in memory:
```bash
sudo cat /run/apiserver/vault.env
```

---

### Step 4: Configure `apiserver.service` to Consume Vault Secrets
Update `/etc/systemd/system/apiserver.service` to declare dependency on `vault-agent.service` and load `/run/apiserver/vault.env`:

```ini
[Unit]
Description=EvHttp API Server
After=network.target vault-agent.service
Requires=vault-agent.service

[Service]
Type=simple
User=apiserver
Group=apiserver
WorkingDirectory=/opt/evhttp/bin

# 1. Non-sensitive defaults from base file
EnvironmentFile=/opt/evhttp/bin/apiserver.env

# 2. Sensitive secrets rendered into RAM by Vault Agent (overrides values above)
EnvironmentFile=-/run/apiserver/vault.env

ExecStart=/opt/evhttp/bin/apiserver
Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal
SyslogIdentifier=apiserver

[Install]
WantedBy=multi-user.target
```

---

### Security Benefits of this Model
1. **Zero Plaintext on Disk:** `/run` is a memory-backed `tmpfs`. Secrets never touch the non-volatile storage/SSD and leave no trace in LXD container snapshots.
2. **Immediate In-Memory Scrubbing:** The server consumes the environment variables on boot and immediately scrubs them using `explicit_bzero()`, leaving `/proc/$PID/environ` empty.
3. **Automated Secret Rotation:** If IT security changes the database password in Vault, Vault Agent automatically updates `/run/apiserver/vault.env` and issues `systemctl restart apiserver` to reconnect the ODBC pool with zero manual intervention.

## 6. Install the Systemd Service
Link the provided systemd service file into the global systemd directory and start the service.

```bash
# Copy the unit file
sudo cp /opt/evhttp/systemd/apiserver.service /etc/systemd/system/

# Reload the systemd daemon to recognize the new file
sudo systemctl daemon-reload

# Enable the service to start automatically on system boot
sudo systemctl enable apiserver.service

# Start the service
sudo systemctl start apiserver.service

# Verify service status
sudo systemctl status apiserver.service
```

## 7. View Logs and Telemetry
Since the application strictly logs JSON to standard error, systemd automatically captures and routes this to `journalctl`.

```bash
# Follow the live logs
sudo journalctl -u apiserver -f -o cat
```

## 8. Trigger a Hot-Reload
If you update `apiserver.env`, you do not need to restart the server and drop active requests. You can trigger the internal `SIGHUP` reload using `systemctl`:

```bash
sudo systemctl reload apiserver.service
```

