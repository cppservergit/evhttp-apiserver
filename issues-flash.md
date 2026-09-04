# Hot-Path Concurrency & Performance Audit Report

## 1. Executive Architecture Overview

The EvHttp API Server implements an **Asymmetric Multi-Reactor + Dual-Worker Pool** architecture:

1. **Reactors (`lib/server.c`)**:
   - Each CPU core binds an independent `event_base` (backed by Linux `epoll`) to the same port using `SO_REUSEPORT`.
   - Responsible for TCP connection lifecycles, HTTP request framing, initial header parsing, CORS validation, and response serialization.
2. **Worker Pools (`lib/worker_pool.c`)**:
   - Split into **Fast** and **Slow** pools to prevent compute-heavy or blocking tasks from starving low-latency routes.
   - Workers handle JWT verification, JSON deserialization, schema validation, and database operations.
3. **Eventfd Notification Channel (`lib/server.c`)**:
   - When a worker finishes processing, it queues the completed task into a thread-safe reactor queue (`g_reactor_queues[rid]`) and writes to an `eventfd`.
   - The reactor wakes from `epoll_wait`, pulls completed tasks, serializes responses via `evhttp_send_reply`, and returns the task to the slab.

---

## 2. Clarifications & Verified Non-Issues

* **Hot-Reload Data Race on `g_jwt_secret` / `g_allowed_origin` (False Positive)**:
  - During hot reload (`SIGHUP`), `config_reload()` invokes `locate_and_load_env(true)`.
  - With `hot_reload = true`, `load_env_file()` **exclusively** parses and updates `ACCESS_LOG` via `atomic_store_explicit(&g_access_log, ...)`.
  - `apply_initial_config_vars()` runs strictly once at startup prior to spawning reactor or worker threads.
  - All shared configuration string buffers (`g_jwt_secret`, `g_allowed_origin`, `g_odbc_conn_strs`, etc.) are immutable across threads during runtime, making concurrent lockless reads completely safe.

---

* **Client Disconnection Race Condition on Libevent Handles (False Positive)**:
  - Original concern: `task->cancelled` is set in `request_on_complete_cb`, but worker threads access `req->input_buffer` causing potential use-after-free.
  - Finding: Libevent safely detaches the request (`req->evcon = NULL`) on early disconnect because `evhttp_request_own(req)` is used. The request memory is isolated and safe. `request_on_complete_cb` is actually dead code.

* **Task Buffer Retention & Memory Bloat (False Positive)**:
  - Original concern: `task->worker_buf` retains memory chains when `evbuffer_drain` is called.
  - Finding: When `server.c` serializes the response, it calls `evbuffer_add_buffer(out_buf, task->worker_buf)`. In libevent, this function destructively moves the memory chains from the source buffer to the destination. Thus, `worker_buf` is left completely empty with zero capacity and retains no memory allocations, making this safe.

## 3. Concurrency & Linux Scaling Findings

### 3.1 Hard Capping of CPU Cores (`lib/server.c:893-895`)
* **Code**:
  ```c
  long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
  if (num_cores <= 0 || num_cores > 64) num_cores = 4;
  ```
* **Issue**: On modern multi-socket server hardware or cloud instances with > 64 vCPUs (e.g. 96, 128, or 256 cores on AMD EPYC / Ampere / Intel Xeon), `num_cores > 64` triggers and **silently falls back to only 4 reactors**.
* **Impact**: Up to 95% of available CPU cores remain unallocated for network I/O.
* **Recommendation**: Remove the 64-core ceiling or raise it to match platform capabilities (e.g. `num_cores > 256`).

