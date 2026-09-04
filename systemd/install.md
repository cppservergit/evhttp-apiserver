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

## 4. Install the Systemd Service
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

## 5. View Logs and Telemetry
Since the application strictly logs JSON to standard error, systemd automatically captures and routes this to `journalctl`.

```bash
# Follow the live logs
sudo journalctl -u apiserver -f -o cat
```

## 6. Trigger a Hot-Reload
If you update `apiserver.env`, you do not need to restart the server and drop active requests. You can trigger the internal `SIGHUP` reload using `systemctl`:

```bash
sudo systemctl reload apiserver.service
```
