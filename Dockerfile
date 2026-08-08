# syntax=docker/dockerfile:1
ARG UBUNTU_RELEASE=26.04

# ===================================================================================================================================
# Multi-stage Dockerfile — GCC-15 builder + Distroless (Chisel + LDD extraction)
#
# Build:   docker build -t apiserver:latest .
# Run:     docker run --rm -p 8080:8080 --env-file bin/apiserver.env -u 1000:1000 -v /opt/uploads:/opt/uploads apiserver:latest
# ===================================================================================================================================

# -----------------------------------------------------------------------------
# Stage 1: BUILD — Ubuntu 26.04 LTS with GCC-15 (stable default compiler)
# -----------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_RELEASE} AS builder

ENV DEBIAN_FRONTEND=noninteractive

# 1. Install Dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc \
        libc6-dev \
        make \
        ca-certificates \
        gawk \
        odbc-postgresql \
        wget \
        libssl-dev \
        zlib1g-dev \
        pkg-config \
        autoconf \
        automake \
        libtool \
        cmake \
        liboath-dev \
    && rm -rf /var/lib/apt/lists/*

# ------------------------------------------------------------------------------
# GLOBAL OPTIMIZATION FLAGS
# ------------------------------------------------------------------------------
ENV CFLAGS="-O2" \
    CXXFLAGS="-O2" \
    LDFLAGS="-O2"

# 2. Compile Minimal libcurl
WORKDIR /tmp/curl
RUN wget -q https://github.com/curl/curl/releases/download/curl-8_21_0/curl-8.21.0.tar.gz -O curl.tar.gz \
    && tar -xvf curl.tar.gz --strip-components=1 \
    && ./configure --prefix=/usr --with-ssl --with-zlib --enable-threaded-resolver \
       --disable-dict --disable-file --disable-ftp --disable-gopher --disable-imap \
       --disable-ldap --disable-ldaps --disable-mqtt --disable-pop3 --disable-rtsp \
       --disable-smb --disable-telnet --disable-tftp \
       --without-libssh2 --without-librtmp --without-libidn2 --without-nghttp2 \
       --without-brotli --without-zstd --without-libpsl \
       --disable-docs --disable-manual \
    && make -j$(nproc) && make install \
    && cp -f /usr/lib/libcurl.so* /usr/lib/x86_64-linux-gnu/

# 3. Compile Minimal libevent
WORKDIR /tmp/libevent
RUN wget -q https://github.com/libevent/libevent/releases/download/release-2.1.13-stable/libevent-2.1.13-stable.tar.gz -O libevent.tar.gz \
    && tar -xvf libevent.tar.gz --strip-components=1 \
    && ./configure --prefix=/usr --disable-static --disable-samples --disable-libevent-regress \
    && make -j$(nproc) && make install

# 4. Compile Minimal json-c
WORKDIR /tmp/json-c
RUN wget -q https://github.com/json-c/json-c/archive/refs/tags/json-c-0.18-20240915.tar.gz -O json-c.tar.gz \
    && tar -xvf json-c.tar.gz --strip-components=1 \
    && mkdir build && cd build \
    && cmake .. \
       -DCMAKE_INSTALL_PREFIX=/usr \
       -DBUILD_SHARED_LIBS=ON \
       -DBUILD_STATIC_LIBS=OFF \
       -DISABLE_WERROR=ON \
       -DENABLE_THREADING=ON \
       -DENABLE_RDRAND=ON \
       -DBUILD_TESTING=OFF \
       -DBUILD_APPS=OFF \
    && make -j$(nproc) && make install

# 3. Compile Minimal unixODBC
WORKDIR /tmp/unixodbc
RUN wget -q https://github.com/lurcher/unixODBC/releases/download/v2.3.14/unixODBC-2.3.14.tar.gz -O unixodbc.tar.gz \
    && tar -xvf unixodbc.tar.gz --strip-components=1 \
    && ./configure --prefix=/usr --sysconfdir=/etc --disable-gui --disable-readline \
       --enable-iconv --with-iconv-char-enc=UTF8 --with-iconv-ucode-enc=UTF16LE \
    && make -j$(nproc) && make install

# 4. Compile Minimal FreeTDS
WORKDIR /tmp/freetds
RUN wget -q https://www.freetds.org/files/stable/freetds-1.5.18.tar.gz -O freetds.tar.gz \
    && tar -xvf freetds.tar.gz --strip-components=1 \
    && ./configure --prefix=/usr --with-unixodbc=/usr --with-openssl=/usr \
       --enable-msdblib --disable-libiconv --disable-krb5 --disable-gssapi \
       --disable-apps --disable-server --disable-pool \
    && make -j$(nproc) && make install

# 5. Compile Minimal libqrencode
WORKDIR /tmp/qrencode
RUN wget -q https://github.com/fukuchi/libqrencode/archive/refs/tags/v4.1.1.tar.gz -O qrencode.tar.gz \
    && tar -xvf qrencode.tar.gz --strip-components=1 \
    && ./autogen.sh \
    && ./configure --prefix=/usr --without-png --without-tools --without-tests \
    && make -j$(nproc) && make install

# 6. Compile Minimal libsodium
WORKDIR /tmp/libsodium
RUN wget -q https://download.libsodium.org/libsodium/releases/libsodium-1.0.22-stable.tar.gz -O libsodium.tar.gz \
    && tar -xvf libsodium.tar.gz --strip-components=1 \
    && ./configure --prefix=/usr --disable-static \
    && make -j$(nproc) && make install


WORKDIR /build
COPY Makefile include/ src/ ./
COPY include/ include/
COPY src/ src/

RUN make slim

# -----------------------------------------------------------------------------
# Stage 2: EXTRACTOR — Chisel base OS + LDD for custom dependencies
# -----------------------------------------------------------------------------
FROM builder AS extractor

ARG UBUNTU_RELEASE
ARG TARGETARCH=amd64
ARG CHISEL_VERSION=v1.4.2

# 1. Install Chisel
RUN wget -q "https://github.com/canonical/chisel/releases/download/${CHISEL_VERSION}/chisel_${CHISEL_VERSION}_linux_${TARGETARCH}.tar.gz" -O chisel.tar.gz && \
    tar -xvf chisel.tar.gz -C /usr/bin/ && rm chisel.tar.gz

WORKDIR /rootfs

# 2. Use Chisel for the base OS layer
RUN chisel cut --release ubuntu-${UBUNTU_RELEASE} --root /rootfs \
        base-files_base \
        base-files_release-info \
        base-files_chisel \
        ca-certificates_data \
        libc6_libs \
        libgcc-s1_libs

# 3. Create app filesystem structure and set permissions
RUN mkdir -p app tmp usr/lib etc/freetds app/uploads \
    && chmod 1777 tmp \
    && echo "evhttp:x:10001:10001::/home/evhttp:/bin/false" > etc/passwd \
    && echo "evhttp:x:10001:" > etc/group \
    && chown -R 10001:10001 app

# 4. Copy the stripped binary
RUN cp /build/bin/apiserver app/apiserver

# 5. Create ODBC and FreeTDS config
RUN echo "hosts: files dns" > etc/nsswitch.conf && \
    printf "[FreeTDS]\nDescription=FreeTDS\nDriver=/usr/lib/libtdsodbc.so\nUsageCount=1\n\n[PostgreSQL Unicode]\nDescription=PostgreSQL ODBC driver (Unicode version)\nDriver=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so\nUsageCount=1\n" > etc/odbcinst.ini && \
    printf "[global]\n\ttext size = 64512\n\tclient charset = UTF-8\n" > etc/freetds/freetds.conf

# 7. Extract all runtime shared libraries (.so files) using ldd on the binary
#    Use `cp -n` to not overwrite libraries that Chisel already installed (like libc)
RUN ldd app/apiserver \
        /usr/lib/libtdsodbc.so \
        /usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so | \
    awk '/=> \// {print $3}' | sort -u | xargs -I '{}' cp -n -v --parents '{}' . && \
    ldd app/apiserver | grep -Eo "/lib64/ld-linux-x86-64.so.[0-9]+" | xargs -I '{}' cp -n -v --parents '{}' .

# 8. Copy the ODBC drivers themselves
RUN cp -n -v --parents /usr/lib/libtdsodbc.so . && \
    cp -n -v --parents /usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so .

# 9. Copy glibc dynamic modules (NSS for DNS, gconv for FreeTDS charsets)
RUN find /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu \
        -name "libnss_dns.so*" -o \
        -name "libnss_files.so*" -o \
        -name "libresolv.so*" 2>/dev/null | xargs -I '{}' cp -n -v --parents '{}' .

# Copy and prune gconv modules (FreeTDS needs these to convert to UTF-8)
RUN cp -r --parents /usr/lib/x86_64-linux-gnu/gconv . && \
    cd usr/lib/x86_64-linux-gnu/gconv && \
    find . -type f \
        -not -name 'gconv-modules*' \
        -not -name 'ISO8859-1.so' \
        -not -name 'UTF-16.so' \
        -not -name 'UTF-32.so' \
        -not -name 'UNICODE.so' \
        -delete

# 10. Strip all binaries and shared libraries to drastically reduce image size
RUN find . -type f -name "*.so*" -exec strip --strip-all {} + 2>/dev/null || true

# -----------------------------------------------------------------------------
# Stage 3: RUNTIME — True Distroless (scratch)
# -----------------------------------------------------------------------------
FROM scratch

# Import the minimal filesystem (binary, .so dependencies, CA certs, ODBC)
COPY --from=extractor /rootfs /

# Default library paths and FreeTDS configuration
ENV LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu/odbc \
    FREETDSCONF=/etc/freetds/freetds.conf \
    TDSVER=7.4 \
    TZ=UTC

WORKDIR /app
EXPOSE 8080
USER 10001

ENTRYPOINT ["./apiserver"]
