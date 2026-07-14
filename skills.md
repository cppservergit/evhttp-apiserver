# EvHttp API Server Specification

## 1. Executive Summary
This document serves as the master blueprint for generating the **EvHttp API Server**, a high-performance, strictly-typed C23 web server built upon `libevent` and `json-c`. It is designed to act as an edge-tier REST API gateway that aggregates data from legacy ODBC SQL databases and external REST microservices while adhering to strict memory safety, concurrency, and telemetry standards.

## 2. Core Architecture & Concurrency
* **Framework:** `libevent` (evhttp) utilizing a highly-concurrent multithreaded reactor/worker pool pattern.
* **Concurrency Model:** N **Reactor threads** maintain independent event loops (`event_base`) for non-blocking network I/O, utilizing `SO_REUSEPORT` for kernel-level load balancing. A central **Worker Pool** pulls parsed HTTP tasks from a shared inbound queue to execute blocking database/network operations. Workers safely return HTTP responses to the correct reactor thread using dedicated per-reactor completion queues and `eventfd` signaling.
* **Thread-Safety:** Relies on robust primitives. Thread-Local Storage (`pthread_key_create`) manages worker-specific memory lifetimes (e.g., JWT caches, libcurl handles) securely without mutex bottlenecks. POSIX read-write locks (`pthread_rwlock_t`) govern global state reloads.

## 3. Subsystem Specifications

### 3.1. Database Integration (ODBC)
* **Driver:** FreeTDS over ODBC to connect seamlessly with MS SQL Server schemas.
* **Abstractions:** Implementations must drastically reduce boilerplate. Use an encapsulated abstraction layer (`odbcutil.c` / `odbcutil.h`).
* **Helper APIs:**
  * `odbcutil_alloc_stmt(hdbc, __func__)`: Automatically extracts the caller's context via preprocessor directives for rich diagnostics on allocation failure.
  * `odbcutil_get_json(query, __func__)`: A 1-liner wrapper that independently allocates handles, executes the parameterless SQL query, streams the rowset directly into a `json_tokener`, and tears down the connection gracefully.

### 3.2. Outbound REST Client (libcurl)
* **Design:** Worker threads independently orchestrate downstream HTTP API requests (e.g., `/metrics` scraping or JWT generation).
* **Isolation:** Requires explicit thread-local initialization (`http_client_init_thread`) and termination (`http_client_cleanup_thread`) upon worker lifecycle transitions to prevent global curl state corruption.

### 3.3. Configuration Management & Hot-Reloading
* **Standard:** Cloud-native `.env` file parsing mapped to strict internal C buffers.
* **Resilience:** The system must validate all configurations at startup. If required variables (e.g., `ODBC_CONN_STR`, `API_URL`) are missing, the server must `LOG_FATAL` and terminate. Default credentials must *never* be hardcoded.
* **Hot-Reloading:** The server must intercept POSIX `SIGHUP` via `sigwait` in the master thread. Upon interception, it safely acquires an exclusive write-lock (`pthread_rwlock_wrlock`) and re-evaluates the `.env` configuration file without dropping active sockets or restarting the process.

### 3.4. Structured Logging & Telemetry
* **Format:** Strict, single-line JSON format streamed exclusively to `stderr`.
* **Levels:** `INFO`, `WARN`, `AUDIT`, `ERROR`, `FATAL`, `DEBUG`.
* **Implementation:** Wrapped behind variadic macros (e.g., `#define LOG_WARN(...)`) that automatically format the payload, escaping quotes and embedding thread-IDs.

### 3.5. JSON Payload Validation
* **Engine:** A custom `ValidationContext` struct-based schema engine mapping JSON keys to expected primitives (`TYPE_INT`, `TYPE_DOUBLE`, `TYPE_STRING`, `TYPE_DATE`).
* **Performance:** Uses high-performance branchless array offsets to enforce boundaries (e.g., verifying leap years without complex dependencies in `validate_date_string_fast`).

## 4. Memory Management & Safety
* **RAII Patterns:** Aggressive utilization of C23 attribute syntax (`[[gnu::cleanup]]`) to mimic C++ destructors. Macros like `RAII_JSON_OBJECT` and `RAII_EVENT_BASE` guarantee immediate reference decay on scope exit, unconditionally eliminating memory leaks.
* **Tooling Standard:** 
  * `-fanalyzer`: Strictest level GCC static analysis.
  * **ASAN (AddressSanitizer):** Catch out-of-bounds heap overflows, use-after-free, and memory leaks.
  * **TSAN (ThreadSanitizer):** Detect race conditions or unprotected shared memory across worker threads.

## 5. Build System & Compilation
* **Toolchain:** `gcc` with `-std=gnu2x` flag.
* **Flags:** Highly pedantic warnings required to compile: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`.
* **Targets:**
  * `make release`: Clean build isolating object files to a dedicated `obj/` folder and binaries to `bin/apiserver`.
  * `make asan` / `make tsan`: Specialized build environments injecting dynamic analysis layers to rigorously profile architecture limits during load tests. Outputs binaries to `bin/test_asan` and `bin/test_tsan`.
