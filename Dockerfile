# syntax=docker/dockerfile:1
ARG UBUNTU_RELEASE=26.04

# ===================================================================================================================================
# Multi-stage Dockerfile — GCC-15 builder + Distroless (Chisel + LDD extraction)
# Architecture: x86_64 only (binary compiled with -march=x86-64-v3, library paths hardcoded to x86_64-linux-gnu)
#
# Build:   docker build -t apiserver:latest .
# Run:     docker run --rm -p 8080:8080 --env-file bin/apiserver.env -u 1000:1000 -v /opt/uploads:/opt/uploads apiserver:latest
# ===================================================================================================================================

# -----------------------------------------------------------------------------
# Stage 1: BUILD — Uses precompiled apiserver-base to compile only the app
# -----------------------------------------------------------------------------
FROM apiserver-base:latest AS builder

WORKDIR /build
COPY Makefile ./
COPY include/ include/
COPY src/ src/

RUN make slim

# -----------------------------------------------------------------------------
# Stage 2: EXTRACTOR — Custom Debian Distroless via LDD
# -----------------------------------------------------------------------------
FROM builder AS extractor

WORKDIR /rootfs

# 1. Create base OS filesystem structure, permissions, and CA certs
RUN mkdir -p app tmp usr/lib etc/freetds app/uploads etc/ssl/certs \
    && chmod 1777 tmp \
    && cp /etc/ssl/certs/ca-certificates.crt etc/ssl/certs/ \
    && echo "hosts: files dns" > etc/nsswitch.conf

# 2. Create app user and set permissions
RUN echo "evhttp:x:10001:10001::/home/evhttp:/bin/false" > etc/passwd \
    && echo "evhttp:x:10001:" > etc/group \
    && chown -R 10001:10001 app

# 3. Copy the stripped binary
RUN cp /build/bin/apiserver app/apiserver

# 4. Create ODBC and FreeTDS config
RUN printf "[FreeTDS]\nDescription=FreeTDS\nDriver=/usr/lib/libtdsodbc.so\nUsageCount=1\n\n[PostgreSQL Unicode]\nDescription=PostgreSQL ODBC driver (Unicode version)\nDriver=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so\nUsageCount=1\n" > etc/odbcinst.ini && \
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

# 9.5 Remove unnecessary profiling and legacy NSS libraries
RUN rm -f usr/lib/x86_64-linux-gnu/libc_malloc_debug.so.0 \
          usr/lib/x86_64-linux-gnu/libthread_db.so.1 \
          usr/lib/x86_64-linux-gnu/libmemusage.so \
          usr/lib/x86_64-linux-gnu/libpcprofile.so \
          usr/lib/x86_64-linux-gnu/libBrokenLocale.so.1 \
          usr/lib/x86_64-linux-gnu/libnsl.so.1 \
          usr/lib/x86_64-linux-gnu/libnss_compat.so.2 \
          usr/lib/x86_64-linux-gnu/libnss_hesiod.so.2 || true

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
