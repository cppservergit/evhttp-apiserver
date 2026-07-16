#include "server.h"
#include "handlers.h"
#include "validation.h"
#include <json-c/json.h>
#include "raii.h"
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/utsname.h>
#include <curl/curl.h>
#include "http_client.h"
#include "customer.h"
#include "sales.h"
#include "shippers.h"
#include "products.h"
#include "config.h"
#include <arpa/inet.h>
#include <sql.h>
#include <sqlext.h>
#include "logger.h"
#include <time.h>
#include <string.h>
#include <stdatomic.h>
#include <event2/thread.h>
#include "worker_pool.h"
#include "task_pool.h"
#include <sys/eventfd.h>

constexpr size_t MAX_PAYLOAD_SIZE = 5 * 1024 * 1024;
constexpr int REQUEST_TIMEOUT_SECONDS = 15;
constexpr char SERVER_VERSION[] = "APIServer 1.00";

const char* get_server_version(void) {
    return SERVER_VERSION;
}

static _Atomic(struct event_base*) *g_worker_bases = nullptr;
static size_t g_total_workers = 0;

typedef struct {
    http_task_t* head;
    http_task_t* tail;
    pthread_mutex_t lock;
    int eventfd;
} reactor_queue_t;

static reactor_queue_t* g_reactor_queues = nullptr;
static char g_hostname[256] = {0};
static char g_start_time[32] = {0};
static char g_os_version[256] = {0};
static uint64_t g_total_ram_kb = 0;
static long g_page_size = 0;

const char* server_get_start_time(void) { return g_start_time; }

const char* server_get_hostname(void) { return g_hostname; }

const char* server_get_os_version(void) { return g_os_version; }

struct worker_stats {
    _Atomic uint64_t total_requests_fast;
    _Atomic uint64_t total_time_fast_ms;
    _Atomic uint64_t total_requests_slow;
    _Atomic uint64_t total_time_slow_ms;
} __attribute__((aligned(64)));

static struct worker_stats* g_worker_stats = nullptr;
static _Thread_local size_t tl_worker_id = 0;
static _Thread_local char tl_tid_str[32] = {0};

static void server_free_globals(void);

static SQLHENV g_odbc_env = SQL_NULL_HENV;

SQLHENV server_get_odbc_env(void) {
    return g_odbc_env;
}

static void odbc_cleanup(void) {
    if (g_odbc_env != SQL_NULL_HENV) {
        SQLFreeHandle(SQL_HANDLE_ENV, g_odbc_env);
        g_odbc_env = SQL_NULL_HENV;
    }
}

int server_init_globals(size_t total_workers) {
    g_total_workers = total_workers;
    if (gethostname(g_hostname, sizeof(g_hostname)) != 0) {
        snprintf(g_hostname, sizeof(g_hostname), "unknown-host");
    }
    
    struct utsname os_info;
    if (uname(&os_info) == 0) {
        snprintf(g_os_version, sizeof(g_os_version), "%s %s", os_info.sysname, os_info.release);
    } else {
        snprintf(g_os_version, sizeof(g_os_version), "unknown-os");
    }
    
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    strftime(g_start_time, sizeof(g_start_time), "%Y-%m-%dT%H:%M:%S", tm_info);
    
    curl_global_init(CURL_GLOBAL_ALL);
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &g_odbc_env);
    SQLSetEnvAttr(g_odbc_env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    atexit(odbc_cleanup);
    
    g_page_size = sysconf(_SC_PAGE_SIZE);
    long pages = sysconf(_SC_PHYS_PAGES);
    g_total_ram_kb = (uint64_t)((pages * g_page_size) / 1024);
    
    size_t configured_threads = config_get_num_threads();
    size_t num_workers = (configured_threads > 0) ? configured_threads : (total_workers * 2);
    size_t q_size = config_get_max_queue_size();
    task_pool_init(q_size == 0 ? 100000 : q_size);
    worker_pool_init(num_workers);

    g_worker_stats = calloc(g_total_workers, sizeof(struct worker_stats));
    g_worker_bases = calloc(g_total_workers, sizeof(_Atomic(struct event_base*)));
    g_reactor_queues = calloc(g_total_workers, sizeof(reactor_queue_t));
    
    if (g_worker_stats == nullptr || g_worker_bases == nullptr || g_reactor_queues == nullptr) {
        server_free_globals();
        return -1;
    }
    
    for (size_t i = 0; i < g_total_workers; ++i) {
        pthread_mutex_init(&g_reactor_queues[i].lock, nullptr);
    }
    
    atexit(server_free_globals);
    return 0;
}

static void server_free_globals(void) {
    worker_pool_shutdown();
    task_pool_shutdown();
    if (g_worker_stats) {
        free(g_worker_stats);
        g_worker_stats = nullptr;
    }
    curl_global_cleanup();
    if (g_worker_bases) {
        free(g_worker_bases);
        g_worker_bases = nullptr;
    }
    if (g_reactor_queues) {
        for (size_t i = 0; i < g_total_workers; ++i) {
            pthread_mutex_destroy(&g_reactor_queues[i].lock);
        }
        free(g_reactor_queues);
        g_reactor_queues = nullptr;
    }
}

void server_shutdown_workers(void) {
    if (!g_worker_bases) return;
    for (size_t i = 0; i < g_total_workers; ++i) {
        struct event_base* base = atomic_load_explicit(&g_worker_bases[i], memory_order_acquire);
        if (base != nullptr) {
            event_base_loopbreak(base);
        }
    }
}

void server_record_request_stats(long long elapsed_ms, bool is_fast) {
    if (g_worker_stats) {
        if (is_fast) {
            atomic_fetch_add_explicit(&g_worker_stats[tl_worker_id].total_requests_fast, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_worker_stats[tl_worker_id].total_time_fast_ms, elapsed_ms, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&g_worker_stats[tl_worker_id].total_requests_slow, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_worker_stats[tl_worker_id].total_time_slow_ms, elapsed_ms, memory_order_relaxed);
        }
    }
}

void server_get_request_stats(server_request_stats_t* out_stats) {
    if (!out_stats) return;
    
    uint64_t reqs_fast = 0;
    uint64_t time_fast = 0;
    uint64_t reqs_slow = 0;
    uint64_t time_slow = 0;
    if (g_worker_stats) {
        for (size_t i = 0; i < g_total_workers; ++i) {
            reqs_fast += atomic_load_explicit(&g_worker_stats[i].total_requests_fast, memory_order_relaxed);
            time_fast += atomic_load_explicit(&g_worker_stats[i].total_time_fast_ms, memory_order_relaxed);
            reqs_slow += atomic_load_explicit(&g_worker_stats[i].total_requests_slow, memory_order_relaxed);
            time_slow += atomic_load_explicit(&g_worker_stats[i].total_time_slow_ms, memory_order_relaxed);
        }
    }
    
    out_stats->total_requests_fast = reqs_fast;
    out_stats->total_requests_slow = reqs_slow;
    out_stats->total_requests = reqs_fast + reqs_slow;
    
    out_stats->total_time_fast_ms = time_fast;
    out_stats->total_time_slow_ms = time_slow;
    out_stats->total_time_ms = time_fast + time_slow;
    
    out_stats->avg_time_fast_ms = reqs_fast ? time_fast / reqs_fast : 0;
    out_stats->avg_time_slow_ms = reqs_slow ? time_slow / reqs_slow : 0;
}

void server_get_memory_stats(uint64_t* total_ram_kb, uint64_t* mem_usage_kb) {
    if (total_ram_kb) {
        *total_ram_kb = g_total_ram_kb;
    }
    
    if (mem_usage_kb) {
        uint64_t mem_usage = 0;
        FILE* f = fopen("/proc/self/statm", "r");
        if (f) {
            long size, resident, share, text, lib, data, dt;
            if (fscanf(f, "%ld %ld %ld %ld %ld %ld %ld", &size, &resident, &share, &text, &lib, &data, &dt) == 7) {
                mem_usage = (uint64_t)((resident * g_page_size) / 1024);
            }
            fclose(f);
        }
        *mem_usage_kb = mem_usage;
    }
}

// middleware_ctx_t now in server.h

static const middleware_ctx_t g_routes[] = {
    { .path = "/ping", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .json_handler = ping_handler, .text_handler = nullptr, .user_arg = nullptr, .is_fast = true },
    { .path = "/version", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .json_handler = version_handler, .text_handler = nullptr, .user_arg = nullptr, .is_fast = true },
    { .path = "/sysinfo", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .json_handler = sysinfo_handler, .text_handler = nullptr, .user_arg = nullptr, .is_fast = true },
    { .path = "/rsysinfo", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .json_handler = rsysinfo_handler, .text_handler = nullptr, .user_arg = nullptr, .is_fast = false },
    { .path = "/customer", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &CustomerContext, .json_handler = customer_handler, .text_handler = nullptr, .user_arg = nullptr, .is_fast = false },
    { .path = "/customer/get", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &CustomerContext, .json_handler = customer_get_handler, .text_handler = nullptr, .user_arg = nullptr, .is_fast = true },
    { .path = "/sales", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &SalesContext, .json_handler = sales_handler, .text_handler = nullptr, .user_arg = nullptr, .is_fast = true },
    { .path = "/shippers", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .json_handler = shippers_handler, .text_handler = nullptr, .user_arg = nullptr, .is_fast = true },
    { .path = "/products", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .json_handler = products_handler, .text_handler = nullptr, .user_arg = nullptr, .is_fast = true },
    { .path = "/metrics", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .json_handler = nullptr, .text_handler = metrics_handler, .user_arg = nullptr, .is_fast = true }
};
static const size_t g_route_count = sizeof(g_routes) / sizeof(g_routes[0]);

static struct json_object* create_error_json(const char* error_msg) {
    struct json_object* err_root = json_object_new_object();
    if (err_root != nullptr) {
        json_object_object_add(err_root, "error", json_object_new_string(error_msg));
    }
    return err_root;
}

static void deferred_json_cleanup(const void *data, size_t datalen, void *extra) {
    (void)data;
    (void)datalen;
    struct json_object *json_root = (struct json_object *)extra;
    if (json_root) {
        json_object_put(json_root);
    }
}

static void send_json_response(struct evhttp_request* req, int status_code, const char* status_txt, struct json_object* json_root) {
    if (json_root == nullptr) {
        evhttp_send_error(req, HTTP_INTERNAL, "Internal Serialization Error");
        return;
    }

    const char* serialized_response = json_object_to_json_string(json_root);
    if (serialized_response == nullptr) {
        json_object_put(json_root); // Explicit cleanup on failure
        evhttp_send_error(req, HTTP_INTERNAL, "Allocation Error");
        return;
    }

    raii_evbuffer response_buf = evbuffer_new();
    if (response_buf == nullptr) {
        json_object_put(json_root); // Explicit cleanup on failure
        evhttp_send_error(req, HTTP_INTERNAL, "Allocation Error");
        return;
    }
    
    // Transfer ownership to libevent network queue via Zero-Copy reference
    if (evbuffer_add_reference(response_buf, serialized_response, strlen(serialized_response), deferred_json_cleanup, json_root) != 0) {
        json_object_put(json_root); // Explicit cleanup on failure
        evhttp_send_error(req, HTTP_INTERNAL, "Buffer Allocation Error");
        return;
    }
    
    struct evkeyvalq* headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(headers, "Content-Type", "application/json");
        
    evhttp_send_reply(req, status_code, status_txt, response_buf);
}

static long long measure_elapsed_ms(const struct timespec* start, const struct timespec* end) {
    long long seconds = (long long)(end->tv_sec - start->tv_sec);
    long long nanoseconds = (long long)(end->tv_nsec - start->tv_nsec);
    return (seconds * 1000LL) + (nanoseconds / 1000000LL);
}

static bool extract_json_body(struct evhttp_request* req, struct json_object** out_body) {
    *out_body = nullptr;
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        return true;
    }
    
    struct evkeyvalq* in_headers = evhttp_request_get_input_headers(req);
    const char* ctype = evhttp_find_header(in_headers, "Content-Type");
    if (ctype == nullptr || strncmp(ctype, "application/json", 16) != 0) {
        send_json_response(req, HTTP_BADREQUEST, "Bad Request", create_error_json("Invalid Content-Type. Expected application/json."));
        return false;
    }
    
    struct evbuffer* in_buf = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(in_buf);
    if (len == 0) {
        send_json_response(req, HTTP_BADREQUEST, "Bad Request", create_error_json("Empty request body."));
        return false;
    }
    
    unsigned char* data = evbuffer_pullup(in_buf, -1);
    if (!data) {
        send_json_response(req, HTTP_INTERNAL, "Internal Server Error", create_error_json("Failed to pull up request body buffer."));
        return false;
    }
    
    raii_json_tokener tok = json_tokener_new();
    struct json_object* req_body = json_tokener_parse_ex(tok, (const char*)data, (int)len);
    enum json_tokener_error jerr = json_tokener_get_error(tok);
    
    if (!req_body || jerr != json_tokener_success || !json_object_is_type(req_body, json_type_object)) {
        if (req_body) json_object_put(req_body);
        send_json_response(req, HTTP_BADREQUEST, "Bad Request", create_error_json("Invalid JSON payload or not a JSON object."));
        return false;
    }
    
    *out_body = req_body;
    return true;
}

void server_notify_task_done(void* arg) {
    http_task_t* task = (http_task_t*)arg;
    size_t rid = task->reactor_id;
    if (rid >= g_total_workers) return;
    
    task->next = nullptr;
    
    pthread_mutex_lock(&g_reactor_queues[rid].lock);
    bool was_empty = (g_reactor_queues[rid].tail == nullptr);
    if (was_empty) {
        g_reactor_queues[rid].head = g_reactor_queues[rid].tail = task;
    } else {
        g_reactor_queues[rid].tail->next = task;
        g_reactor_queues[rid].tail = task;
    }
    pthread_mutex_unlock(&g_reactor_queues[rid].lock);
    
    // Event Coalescing pattern: only signal the reactor if it was asleep/empty
    if (was_empty) {
        uint64_t one = 1;
        if (write(g_reactor_queues[rid].eventfd, &one, sizeof(one)) < 0) {
            LOG_ERROR("Failed to write to reactor eventfd");
        }
    }
}

static void request_on_complete_cb(struct evhttp_request *req, void *arg) {
    (void)req;
    http_task_t* task = (http_task_t*)arg;
    atomic_store(&task->cancelled, true);
}

static const char* extract_client_ip(struct evhttp_request* req) {
    struct evkeyvalq* in_headers = evhttp_request_get_input_headers(req);
    const char* x_forwarded_for = evhttp_find_header(in_headers, "X-Forwarded-For");
    if (x_forwarded_for) {
        return x_forwarded_for;
    }
    
    struct evhttp_connection* evcon = evhttp_request_get_connection(req);
    char* peer_ip = nullptr;
    uint16_t port = 0;
    if (evcon) evhttp_connection_get_peer(evcon, &peer_ip, &port);
    if (peer_ip) return peer_ip;
    
    return "unknown";
}

static void cleanup_cancelled_task(http_task_t* task) {
    if (task->response_json) json_object_put(task->response_json);
    if (task->response_text) evbuffer_free(task->response_text);
    if (task->parsed_body) json_object_put(task->parsed_body);
    task_pool_free(task);
}

static void process_completed_task(http_task_t* task) {
    evhttp_request_set_on_complete_cb(task->req, nullptr, nullptr);
    
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    long long elapsed_ms = measure_elapsed_ms(&task->start_time, &end_time);
    const middleware_ctx_t* ctx = (const middleware_ctx_t*)task->middleware_ctx;
    bool is_fast = ctx ? ctx->is_fast : false;
    server_record_request_stats(elapsed_ms, is_fast);
    
    const char* client_ip = extract_client_ip(task->req);
    
    if (config_get_access_log()) {
        struct evkeyvalq* in_headers = evhttp_request_get_input_headers(task->req);
        const char* req_id = evhttp_find_header(in_headers, "X-Request-Id");
        if (req_id) {
            LOG_INFO("clientIP=%s reqID=%s uri=%s elapsed_ms=%lld", client_ip, req_id, evhttp_request_get_uri(task->req), elapsed_ms);
        } else {
            LOG_INFO("clientIP=%s uri=%s elapsed_ms=%lld", client_ip, evhttp_request_get_uri(task->req), elapsed_ms);
        }
    }
    
    if (task->response_json) {
        send_json_response(task->req, task->status_code, task->status_txt, task->response_json);
    } else if (task->response_text) {
        struct evkeyvalq* headers = evhttp_request_get_output_headers(task->req);
        evhttp_add_header(headers, "Content-Type", "text/plain");
        evhttp_send_reply(task->req, task->status_code, task->status_txt, task->response_text);
        evbuffer_free(task->response_text);
    } else {
        send_json_response(task->req, HTTP_INTERNAL, "Internal Server Error", create_error_json("Internal Server Error"));
    }
    
    if (task->parsed_body) json_object_put(task->parsed_body);
    task_pool_free(task);
}

static void reactor_eventfd_cb(evutil_socket_t fd, short events, void *arg) {
    (void)events; (void)arg;
    uint64_t val;
    // Drain eventfd counter
    while (read(fd, &val, sizeof(val)) == sizeof(val)) {}
    
    size_t rid = tl_worker_id;
    
    pthread_mutex_lock(&g_reactor_queues[rid].lock);
    http_task_t* curr = g_reactor_queues[rid].head;
    g_reactor_queues[rid].head = g_reactor_queues[rid].tail = nullptr;
    pthread_mutex_unlock(&g_reactor_queues[rid].lock);
    
    while (curr != nullptr) {
        http_task_t* task = curr;
        curr = curr->next;
        
        if (atomic_load(&task->cancelled)) {
            cleanup_cancelled_task(task);
            continue;
        }
        
        process_completed_task(task);
    }
}

static void api_middleware_wrapper(struct evhttp_request* req, void* arg) {
    const middleware_ctx_t* ctx = (const middleware_ctx_t*)arg;
    if (ctx == nullptr || (ctx->json_handler == nullptr && ctx->text_handler == nullptr)) {
        evhttp_send_error(req, HTTP_INTERNAL, "Middleware Routing Fault");
        return;
    }

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    if (evhttp_request_get_command(req) != ctx->allowed_method) {
        send_json_response(req, HTTP_BADMETHOD, "Method Not Allowed", create_error_json("Method not permitted."));
        return;
    }
    
    struct json_object* parsed_body = nullptr;
    if (!extract_json_body(req, &parsed_body)) {
        return;
    }
    if (ctx->validation_ctx != nullptr && parsed_body != nullptr) {
        char err_buf[MAX_ERR_MSG_LEN] = {0};
        if (!validate_json(ctx->validation_ctx, parsed_body, err_buf, sizeof(err_buf))) {
            send_json_response(req, HTTP_BADREQUEST, "Bad Request", create_error_json(err_buf));
            json_object_put(parsed_body);
            return;
        }
    }
    
    http_task_t* task = task_pool_alloc();
    task->req = req;
    task->parsed_body = parsed_body;
    task->middleware_ctx = ctx;
    task->start_time = start_time;
    task->reactor_id = tl_worker_id;
    atomic_init(&task->cancelled, false);
    
    evhttp_request_set_on_complete_cb(req, request_on_complete_cb, task);
    
    if (!worker_pool_enqueue(task)) {
        // Backpressure limit reached, queue is full.
        evhttp_request_set_on_complete_cb(req, nullptr, nullptr);
        send_json_response(req, HTTP_SERVUNAVAIL, "Service Unavailable", create_error_json("Server Too Busy"));
        if (parsed_body) json_object_put(parsed_body);
        task_pool_free(task);
        return;
    }
}

static struct event_base* create_optimized_event_base(void) {
    raii_event_config cfg = event_config_new();
    if (cfg != nullptr) {
        event_config_avoid_method(cfg, "select");
        event_config_avoid_method(cfg, "poll");
    }
    return event_base_new_with_config(cfg);
}

static evutil_socket_t create_and_bind_socket(uint16_t port, const char* addr) {
    evutil_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    if (evutil_make_socket_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) { close(fd); return -1; }
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) < 0) { close(fd); return -1; }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) { close(fd); return -1; }

#ifdef TCP_KEEPIDLE
    int keepidle = 10;
    if (setsockopt(fd, SOL_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) < 0) { close(fd); return -1; }
#elif defined(TCP_KEEPALIVE)
    int keepalive = 10;
    if (setsockopt(fd, SOL_TCP, TCP_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) { close(fd); return -1; }
#endif

    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port)
    };
    inet_pton(AF_INET, addr, &sin.sin_addr);

    if (bind(fd, (struct sockaddr*)&sin, sizeof(sin)) < 0 || listen(fd, SOMAXCONN) < 0) {
        close(fd);
        return -1;
    }
    
    return fd;
}

static struct evhttp* configure_http_server(struct event_base* base, evutil_socket_t fd, const middleware_ctx_t* routes, size_t route_count) {
    struct evhttp* http = evhttp_new(base);
    if (http == nullptr) return nullptr;

    evhttp_set_max_body_size(http, MAX_PAYLOAD_SIZE);
    evhttp_set_timeout(http, REQUEST_TIMEOUT_SECONDS); 

    for (size_t i = 0; i < route_count; ++i) {
        evhttp_set_cb(http, routes[i].path, api_middleware_wrapper, (void*)&routes[i]);
    }
    
    if (evhttp_accept_socket_with_handle(http, fd) == nullptr) {
        evhttp_free(http);
        return nullptr;
    }
    return http;
}

void* worker_thread_logic(void* arg) {
    size_t worker_id = (size_t)arg;
    tl_worker_id = worker_id;
    
    raii_event_base base = create_optimized_event_base();
    if (base == nullptr) return nullptr;
    
    atomic_store_explicit(&g_worker_bases[worker_id], base, memory_order_release);

    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) {
        LOG_FATAL("Failed to create eventfd for Worker %zu", worker_id);
        return nullptr;
    }
    g_reactor_queues[worker_id].eventfd = efd;
    
    struct event* efd_ev = event_new(base, efd, EV_READ | EV_PERSIST, reactor_eventfd_cb, nullptr);
    event_add(efd_ev, nullptr);

    evutil_socket_t fd = create_and_bind_socket((uint16_t)SERVER_PORT, SERVER_ADDR);
    if (fd < 0) {
        LOG_FATAL("Failed to bind socket for Worker %zu", worker_id);
        return nullptr;
    }

    raii_evhttp http = configure_http_server(base, fd, g_routes, g_route_count);
    if (http == nullptr) {
        LOG_FATAL("Failed to configure HTTP server for Worker %zu", worker_id);
        close(fd);
        return nullptr;
    }

    unsigned long long tid = (unsigned long long)pthread_self();
    snprintf(tl_tid_str, sizeof(tl_tid_str), "0x%llx", tid);
    LOG_INFO("Worker %zu started", worker_id);
    
    event_base_dispatch(base);

    LOG_INFO("Worker %zu stopped", worker_id);
    event_free(efd_ev);
    close(efd);
    return nullptr;
}
