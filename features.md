# Technical Features Checklist

This project is an advanced HTTP API server written in C, heavily optimized for scalability, memory safety, and deterministic concurrency.

## 1. Modern C & Hardening
- [x] **Modern C Standards:** Designed using C11/C23 paradigms (compiled with `-std=gnu2x`).
- [x] **Aggressive Warning Flags:** Compiled with strict flags turning warnings into errors (`-Wall -Wextra -Werror -Wpedantic -Wshadow -Wvla -Wnull-dereference -Wimplicit-fallthrough`).
- [x] **Sanitizers:** Integration with AddressSanitizer (ASAN), ThreadSanitizer (TSAN), and Valgrind for formal verification of memory safety and data races.
- [x] **Binary Defenses:** Uses strong runtime security (`-fstack-protector-strong`, `-fstack-clash-protection`, `-fcf-protection=full`, `-D_FORTIFY_SOURCE=3`, `-fPIE`, `-Wl,-z,relro,-z,now`).

## 2. Advanced Concurrency & Thread-Safety
- [x] **Strict Architecture Decoupling:** Libevent structures (like `struct evhttp_request`) are completely contained within the reactor thread. Background workers process pure, decoupled structs (`http_task_t`), mathematically eliminating network-tier data races.
- [x] **Thread-Local Storage (TLS):** Extensive use of `_Thread_local` variables for caching state (e.g., client IPs, authentication data) across handlers, entirely avoiding lock contention.
- [x] **Atomic Hot-Reloading:** The configuration system leverages C11 atomics (`_Atomic` / `<stdatomic.h>`) for zero-downtime, lock-free `SIGHUP` config hot-reloading.
- [x] **Leader-Follower JWT Authentication:** Solves the Thundering Herd bottleneck using a condition variable and mutex. Instead of all threads stalling on a global read/write lock or firing off simultaneous upstream requests, exactly one thread fetches an expired JWT over the network while followers yield efficiently.
- [x] **No Global Fast-Path Locks:** Handlers run lock-free. Contention is eliminated to allow linear multi-core scaling.

## 3. High-Performance Multithreading
- [x] **Reactor Pattern:** Network I/O is managed by `libevent` event loops, offloading blocking workloads to independent threads.
- [x] **CPU Affinity:** Reactor and worker threads can be bound to specific hardware cores using `CPU_SET` to drastically improve L1/L2 cache locality and reduce context-switching.
- [x] **Bulkheading (Thread Segmentation):** Worker threads are segregated into a Fast Pool (for quick calculations/memory operations) and a Slow Pool (for ODBC queries and synchronous remote HTTP calls), preventing I/O stalls from starving lightweight requests.

## 4. Queues & IPC Mechanisms
- [x] **Zero-Allocation Intrusive Queues:** Task routing utilizes custom lock-based, O(1) singly-linked intrusive queues. The `next` pointers are embedded directly in the task structs, removing dynamic heap allocation during queueing.
- [x] **Eventfd Coalescence:** Thread-to-Reactor communication happens via lightweight Linux `eventfd`. Multiple wake-ups coalesce into a single 64-bit integer, slashing kernel syscall overhead compared to pipes or socketpairs.
- [x] **Backpressure Controls:** Ingest limits and queue clamping prevent uncontrolled memory growth under DDoS or massive traffic spikes, returning 503 instead of crashing.
- [x] **Asynchronous Logger:** A dedicated background thread processes logs using a lock-based ring-buffer queue and a pre-allocated object pool of log entries. This offloads blocking I/O calls from the fast-path workers, ensuring log emission doesn't stall the system.

## 5. Memory Safety & Architecture
- [x] **Object Slab Allocation:** `http_task_t` objects are leased from a pre-allocated Object Pool (`task_pool.c`) at startup. The hot path requires zero `malloc()` or `free()` calls.
- [x] **Strict Buffer Boundaries:** Bound-checking is enforced everywhere (`snprintf` and bounds-checked `memcpy` only, bypassing compiler truncation heuristics by avoiding `strcpy`, `strncpy`, `strcat`, or `sprintf`).
- [x] **Constant-Time Cryptography:** Secrets and API keys are validated using `libsodium`'s `sodium_memcmp` to prevent cryptographic timing side-channel attacks.
- [x] **Deterministic Shutdown:** Graceful cleanup sequences strictly deallocate all memory and handles, avoiding unpredictable `atexit()` bugs.

## 6. Business Logic Integration
- [x] **Declarative Validation:** Incoming JSON payloads are validated against a strict schema engine (enforcing types, bounds, and string sizes) before any handler executes.
- [x] **Secure Middleware:** Implements robust CORS validation, Method enforcement, JWT authentication, and automated audit logging out-of-the-box.
