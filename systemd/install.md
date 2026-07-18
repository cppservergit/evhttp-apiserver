# EvHttp API Server - Systemd Installation Guide

This guide details how to deploy and run the pre-compiled EvHttp API server binary as a persistent `systemd` daemon on Ubuntu 24.04 or Ubuntu 26.04.

## 1. Install Runtime Dependencies
Ensure that all required shared libraries are natively installed on your container.

### For Ubuntu 26.04
```bash
sudo apt-get update
sudo apt-get install -y libevent-2.1-7 \
                        libevent-pthreads-2.1-7 \
                        libjson-c5 \
                        libcurl4t64 \
                        unixodbc \
                        tdsodbc
```

### For Ubuntu 24.04 (t64 ABI transition)
```bash
sudo apt-get update
sudo apt-get install -y libevent-2.1-7t64 \
                        libevent-pthreads-2.1-7t64 \
                        libjson-c5 \
                        libcurl4t64 \
                        unixodbc \
                        tdsodbc
```

## 2. Deploy the Binary
For this example, we will assume you are deploying the pre-compiled application bundle to `/opt/evhttp`.

```bash
sudo mkdir -p /opt/evhttp/bin
sudo cp apiserver /opt/evhttp/bin/
sudo cp apiserver.env /opt/evhttp/bin/
sudo chmod +x /opt/evhttp/bin/apiserver

# Create an unprivileged system user for security
sudo useradd -r -s /bin/false apiserver

# Grant ownership of the deployment directory to the unprivileged user
sudo chown -R apiserver:apiserver /opt/evhttp
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
# database access
ODBC_CONN_STR=Driver=FreeTDS;SERVER=demodb.mshome.net;PORT=1433;DATABASE=demodb;UID=your_username;PWD=your_password;APP=apiserver;Encryption=off;ClientCharset=UTF-8

# remote backend API configuration
API_URL=https://cppserver.com
API_USER=your_api_user
API_PASS=your_api_pass
REMOTE_API_KEY=your_api_key
TELEMETRY_API_KEY=your_telemetry_key

# enable access logs - can be changed on the fly and supports service reload
ACCESS_LOG=true

# server configuration - any change requires service restart
NUM_THREADS=80
MAX_QUEUE_SIZE=10000
FAST_POOL_PERCENTAGE=25

# remote login provider
LOGIN_PROVIDER=http://demodb.mshome.net:8080
LOGIN_URI=/login

# jwt configuration

# generated with: openssl rand -hex 32
JWT_SECRET=your_jwt_secret
JWT_TIMEOUT_SECONDS=300

# trust haproxy IP for accepting X-Forwarded-For header
TRUST_PROXY_IP=127.0.0.1
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
