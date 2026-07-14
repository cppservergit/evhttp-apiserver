# EvHttp API Server

**EvHttp API Server** is a high-performance, strictly-typed C23 web server and REST API gateway. Built on top of `libevent` and `json-c`, it utilizes a highly concurrent multithreaded reactor/worker-pool architecture to aggregate data from legacy ODBC SQL databases and external microservices. It is designed to be extremely fast, memory-safe, and capable of gracefully sustaining massive bursts of traffic.

## Repository
**GitHub:** `git@github.com:cppservergit/evhttp-apiserver.git`  
**Branch:** `main`

## Recommended Environments
This project embraces modern C23 standards and strict compiler safety flags. We recommend developing on:
* **Ubuntu 24.04 LTS** utilizing **GCC 14**
* **Ubuntu 26.04 LTS** utilizing **GCC 15**

## Development Installation

### 1. Install Dependencies
Install the required development headers and libraries. 

For Ubuntu 24.04 and 26.04:
```bash
sudo apt-get update
sudo apt-get install -y build-essential gcc make \
                        libevent-dev \
                        libjson-c-dev \
                        libcurl4-openssl-dev \
                        unixodbc-dev \
                        tdsodbc
```

### 2. Clone the Repository
```bash
git clone git@github.com:cppservergit/evhttp-apiserver.git
cd evhttp-apiserver
```

### 3. Configure the Environment
The server requires an `apiserver.env` configuration file in the `bin/` directory.

```bash
mkdir -p bin
cat <<EOF > bin/apiserver.env
ODBC_CONN_STR=Driver=FreeTDS;SERVER=demodb.mshome.net;PORT=1433;DATABASE=demodb;UID=sa;PWD=your_password;APP=apiserver;Encryption=off;ClientCharset=UTF-8
API_URL=https://cppserver.com
API_USER=mcordova
API_PASS=basica
ACCESS_LOG=true
NUM_THREADS=12
MAX_QUEUE_SIZE=10000
EOF
```

### 4. Build and Run
The project uses a standard Makefile. The compiled binary will be placed in the `bin/` directory.

```bash
# Compile the optimized release build
make release

# Run the server
./bin/apiserver
```

### 5. Advanced Targets (Sanitizers)
To profile the architecture under load, you can build with Google Sanitizers:
```bash
make asan   # Builds bin/test_asan (Address/Leak Sanitizer)
make tsan   # Builds bin/test_tsan (Thread Sanitizer)
```

## Production Deployment
For production environments, the server is designed to run securely as a persistent `systemd` daemon with automatic restart, logging, and hot-reload capabilities. 

Please see the [Systemd Installation Guide](systemd/install.md) for full instructions on deploying the binary and configuring the service.

## License
This project is licensed under the 3-Clause BSD License - see the [LICENSE](LICENSE) file for details.
