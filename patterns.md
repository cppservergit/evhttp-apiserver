# APIServer Architectural Patterns

This document enumerates the well-known C programming and concurrent architecture patterns utilized throughout the APIServer codebase to achieve extreme scalability, deterministic memory safety, and high-performance I/O.

## 1. Concurrency & Networking

### Reactor Pattern
**Location:** `server.c`
**Description:** Uses `libevent` to multiplex non-blocking network I/O across thousands of concurrent client connections within a single thread. The reactor loop exclusively handles socket acceptance and byte-shuffling, ensuring the system can sustain massive TCP bursts.

### Worker Pool Pattern
**Location:** `worker_pool.c`
**Description:** A pre-spawned pool of background threads that offloads blocking I/O (like ODBC queries and cURL HTTP calls) and CPU-bound work (JSON parsing, JWT cryptographic validation) from the reactor thread.

### Bulkheading Pattern
**Location:** `worker_pool.c`
**Description:** The Worker Pool is intentionally partitioned into a **Fast Pool** and a **Slow Pool**. Fast queries (like local database reads) route to the fast pool, while inherently slow external network proxies route to the slow pool. This physically prevents a latency spike in a 3rd-party remote API from starving threads needed for fast, healthy local endpoints.

### Leader-Follower (Single-Flight) Pattern
**Location:** `customer.c` (`login_and_get_token`)
**Description:** Solves the classic "Thundering Herd" race condition. When a globally cached JWT expires while 500 threads are trying to use it, the first thread (the Leader) acquires the lock and initiates the network refresh. The other 499 threads (Followers) immediately yield via `pthread_cond_wait`. Once the leader fetches the token, it broadcasts to wake all followers simultaneously.

### Asynchronous Ring-Buffer Logging Pattern
**Location:** `logger.c`
**Description:** Prevents slow `STDERR` terminal/journald writes from blocking the critical worker threads. Application threads push log pointers onto a fixed-size ring buffer queue, instantly returning to request processing. A dedicated background logger thread wakes up, pops the queue, and writes to `STDERR`. By keeping log payloads under the Linux `PIPE_BUF` limit (4096 bytes), the POSIX `write()` syscall inherently guarantees atomic, non-interleaved output across concurrent threads.

## 2. Memory Management & Optimization

### Object Pool (Slab Allocator) Pattern
**Location:** `task_pool.c`
**Description:** Pre-allocates a massive contiguous array (slab) of `http_task_t` objects at server startup. The hot path strictly checks out and returns these objects, mathematically eliminating dynamic heap allocation latency (`malloc` and `free`) and heap fragmentation under heavy load.

### Thread-Local Storage (TLS) Cache Pattern
**Location:** `task_pool.c` and `server.c`
**Description:** The Object Pool utilizes `_Thread_local` variables to maintain a per-thread free-list cache (`tl_cache[64]`). Because tasks are allocated and deallocated on the exact same reactor thread, the system achieves true O(1) lock-free allocation for the vast majority of network requests without ever contending for the global pool mutex.

### Zero-Copy Safe Ownership Transfer
**Location:** `server.c`
**Description:** Instead of deep-copying request payloads to pass them to background threads, the reactor calls `evhttp_request_own(req)`. This elegantly detaches the memory from libevent's internal lifecycle, safely handing the raw pointer to the worker pool. This achieves zero-copy throughput while strictly preventing Use-After-Free data races on abrupt client disconnects.

### Intrusive Linked List Pattern
**Location:** `task_pool.h` (`http_task_t`)
**Description:** To pass tasks between queues without triggering dynamic allocation for queue nodes, the task object itself embeds a `struct http_task_s* next` pointer. The object *is* the queue node.

## 3. Synchronization & POSIX Threading

### Unlock-Before-Signal Pattern
**Location:** `worker_pool.c` (`worker_pool_enqueue`)
**Description:** When waking a background worker via condition variables, `pthread_mutex_unlock` is deliberately called *right before* `pthread_cond_signal`. This micro-optimization avoids the "wait-morphing" penalty, wherein a woken thread immediately stalls and goes back to sleep trying to acquire a lock still held by the signaling thread.

### Eventfd Syscall Coalescing
**Location:** `server.c` (`server_notify_task_done`)
**Description:** Workers use a lightweight Linux `eventfd` to signal the reactor thread when tasks are complete. To prevent flooding the kernel with `write()` syscalls, workers aggressively coalesce wakeups by only writing to the `eventfd` if the completion queue was previously empty. The reactor thread then drains the entire queue in a single batch.

## 4. Modern C Engineering

### RAII (Resource Acquisition Is Initialization) via C Attributes
**Location:** `raii.h`
**Description:** Brings C++ style scope-safety and deterministic destruction to C. By leveraging the GCC/Clang `[[gnu::cleanup(function)]]` attribute, pointers to libevent structs, evbuffers, and json-c objects automatically invoke their respective `free()` functions the moment they go out of scope, elegantly eliminating memory leaks in complex error branches.
