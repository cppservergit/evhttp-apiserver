#include "server.h"
#include "handlers.h"
#include "mcp.h"
#include "validation.h"
#include <json-c/json.h>
#include "raii.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/utsname.h>
#include <curl/curl.h>
#include <sodium.h>
#include <liboath/oath.h>
#include <event2/thread.h>
#include "http_client.h"
#include "customer.h"
#include "jwt.h"

#include "config.h"
#include "login.h"
#include <arpa/inet.h>
#include <sql.h>
#include <sqlext.h>
#include <errno.h>
#include "logger.h"
#include <time.h>
#include <string.h>
#include <stdatomic.h>
#include <event2/bufferevent.h>
#include "worker_pool.h"
#include "task_pool.h"
#include <sys/eventfd.h>

constexpr int REQUEST_TIMEOUT_SECONDS = 15;
constexpr char SERVER_VERSION[] = "APIServer 1.00";

const char* get_server_version(void) {
    return SERVER_VERSION;
}

static const char* server_get_client_ip_fast(struct evhttp_connection* evcon) {
    static _Thread_local char ip_buf[INET6_ADDRSTRLEN];
    
    struct bufferevent* bev = evhttp_connection_get_bufferevent(evcon);
    if (!bev) return "unknown";
    
    int fd = bufferevent_getfd(bev);
    if (fd < 0) return "unknown";
    
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    
    if (getpeername(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
        if (addr.ss_family == AF_INET) {
            struct sockaddr_in* s = (struct sockaddr_in*)&addr;
            if (inet_ntop(AF_INET, &s->sin_addr, ip_buf, sizeof(ip_buf))) return ip_buf;
        } else if (addr.ss_family == AF_INET6) {
            struct sockaddr_in6* s = (struct sockaddr_in6*)&addr;
            if (inet_ntop(AF_INET6, &s->sin6_addr, ip_buf, sizeof(ip_buf))) return ip_buf;
        }
    }
    return "unknown";
}

static bool is_trusted_proxy(struct evhttp_request* req, const char** out_peer_ip) {
    struct evhttp_connection* evcon = evhttp_request_get_connection(req);
    if (!evcon) return false;

    const char* peer_ip = server_get_client_ip_fast(evcon);
    if (out_peer_ip) *out_peer_ip = peer_ip;

    if (strcmp(peer_ip, "unknown") == 0) return false;

    char trusted_proxy[MAX_CONFIG_STR];
    config_get_trust_proxy_ip(trusted_proxy, sizeof(trusted_proxy));

    if (trusted_proxy[0] != '\0' && strcmp(peer_ip, trusted_proxy) == 0) {
        return true;
    }
    
    return false;
}

static const char* server_extract_client_ip(struct evhttp_request* req) {
    struct evkeyvalq* headers = evhttp_request_get_input_headers(req);
    const char* x_forwarded_for = evhttp_find_header(headers, "X-Forwarded-For");

    if (!x_forwarded_for || !is_trusted_proxy(req, nullptr)) {
        struct evhttp_connection* evcon = evhttp_request_get_connection(req);
        if (evcon) {
            return server_get_client_ip_fast(evcon);
        }
        return "unknown";
    }

    const char* last_comma = strrchr(x_forwarded_for, ',');
    if (!last_comma) {
        return x_forwarded_for;
    }

    static _Thread_local char parsed_ip[INET6_ADDRSTRLEN];
    const char* start = last_comma + 1;
    
    while (*start == ' ') {
        start++;
    }
    
    size_t len = strlen(start);
    if (len >= sizeof(parsed_ip)) len = sizeof(parsed_ip) - 1;
    
    memcpy(parsed_ip, start, len);
    parsed_ip[len] = '\0';
    return parsed_ip;
}

static _Atomic(struct event_base*) *g_reactor_bases = nullptr;
static size_t g_num_reactors = 0;
static pthread_barrier_t g_startup_barrier;
static _Atomic bool g_startup_failed = false;

bool server_did_startup_fail(void) {
    return atomic_load_explicit(&g_startup_failed, memory_order_acquire);
}

typedef struct {
    http_task_t* head;
    http_task_t* tail;
    pthread_mutex_t lock;
    int eventfd;
    char _padding[4];
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

struct reactor_stats {
    _Atomic uint64_t total_requests_fast;
    _Atomic uint64_t total_time_fast_ms;
    _Atomic uint64_t total_requests_slow;
    _Atomic uint64_t total_time_slow_ms;
    char _padding[32];
} __attribute__((aligned(64)));

static struct reactor_stats* g_reactor_stats = nullptr;
static _Thread_local size_t tl_reactor_id = 0;
static _Thread_local char tl_tid_str[32] = {0};

// Removed static void server_free_globals(void);

static SQLHENV g_odbc_env = SQL_NULL_HENV;

SQLHENV server_get_odbc_env(void) {
    return g_odbc_env;
}

static void cleanup_external_libraries(void) {
    if (g_odbc_env != SQL_NULL_HENV) {
        SQLFreeHandle(SQL_HANDLE_ENV, g_odbc_env);
        g_odbc_env = SQL_NULL_HENV;
    }
    
    oath_done();
    curl_global_cleanup();
}

static int init_server_metadata(size_t num_reactors) {
    if (evthread_use_pthreads() != 0) {
        LOG_FATAL("Failed to initialize libevent pthreads.");
        return -1;
    }
    
    g_num_reactors = num_reactors;
    if (gethostname(g_hostname, sizeof(g_hostname)) != 0) {
        (void)snprintf(g_hostname, sizeof(g_hostname), "unknown-host");
    }
    
    struct utsname os_info;
    if (uname(&os_info) == 0) {
        (void)snprintf(g_os_version, sizeof(g_os_version), "%s %s", os_info.sysname, os_info.release);
    } else {
        (void)snprintf(g_os_version, sizeof(g_os_version), "unknown-os");
    }
    
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    (void)strftime(g_start_time, sizeof(g_start_time), "%Y-%m-%dT%H:%M:%S", &tm_info);
    
    if (sodium_init() < 0) {
        LOG_FATAL("Failed to initialize libsodium");
        return -1;
    }
    
    oath_init();
    
    curl_global_init(CURL_GLOBAL_ALL);
    
    SQLRETURN ret = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &g_odbc_env);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_FATAL("Failed to allocate ODBC environment handle");
        return -1;
    }
    
    ret = SQLSetEnvAttr(g_odbc_env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_FATAL("Failed to set ODBC version to v3");
        SQLFreeHandle(SQL_HANDLE_ENV, g_odbc_env);
        g_odbc_env = SQL_NULL_HENV;
        return -1;
    }
    
    g_page_size = sysconf(_SC_PAGE_SIZE);
    long pages = sysconf(_SC_PHYS_PAGES);
    g_total_ram_kb = (((uint64_t)pages * (uint64_t)g_page_size) / 1024);
    return 0;
}

static int init_reactor_queues(size_t bg_workers_count) {
    g_reactor_stats = calloc(g_num_reactors, sizeof(struct reactor_stats));
    g_reactor_bases = calloc(g_num_reactors, sizeof(_Atomic(struct event_base*)));
    g_reactor_queues = calloc(g_num_reactors, sizeof(reactor_queue_t));
    
    if (!g_reactor_stats || !g_reactor_bases || !g_reactor_queues) return -1;
    
    for (size_t i = 0; i < g_num_reactors; ++i) {
        pthread_mutex_init(&g_reactor_queues[i].lock, nullptr);
        int efd = eventfd(0, EFD_NONBLOCK);
        if (efd < 0) {
            char errbuf[256];
            LOG_FATAL("Failed to create eventfd for Reactor %zu: %s", i, strerror_r(errno, errbuf, sizeof(errbuf)));
            return -1;
        }
        g_reactor_queues[i].eventfd = efd;
    }

    int rc_bar = pthread_barrier_init(&g_startup_barrier, nullptr, g_num_reactors + 1);
    if (rc_bar != 0) {
        char errbuf[256];
        LOG_FATAL("Failed to initialize startup barrier: %s", strerror_r(rc_bar, errbuf, sizeof(errbuf)));
        return -1;
    }

    if (worker_pool_init(bg_workers_count) != 0) return -1;
    return 0;
}

int server_init_globals(size_t num_reactors) {
    if (init_server_metadata(num_reactors) != 0) return -1;
    
    size_t configured_threads = config_get_num_threads();
    size_t bg_workers_count = (configured_threads > 0) ? configured_threads : (num_reactors * 2);
    size_t q_size = config_get_max_queue_size();
    
    size_t safe_q_size = (q_size == 0) ? 100000 : q_size;
    size_t slab_size = (safe_q_size * 2) + bg_workers_count + 10000;
    if (task_pool_init(slab_size) != 0) {
        server_cleanup_globals();
        return -1;
    }
    
    if (init_reactor_queues(bg_workers_count) != 0) {
        server_cleanup_globals();
        return -1;
    }
    
    return 0;
}

void server_cleanup_globals(void) {
    worker_pool_shutdown();
    task_pool_shutdown();
    if (g_reactor_stats) {
        free(g_reactor_stats);
        g_reactor_stats = nullptr;
    }
    if (g_reactor_bases) {
        free(g_reactor_bases);
        g_reactor_bases = nullptr;
    }
    if (!g_reactor_queues) {
        cleanup_external_libraries();
        return;
    }
    for (size_t i = 0; i < g_num_reactors; ++i) {
        if (g_reactor_queues[i].eventfd >= 0) {
            close(g_reactor_queues[i].eventfd);
        }
        pthread_mutex_destroy(&g_reactor_queues[i].lock);
    }
    free(g_reactor_queues);
    g_reactor_queues = nullptr;
    pthread_barrier_destroy(&g_startup_barrier);
    cleanup_external_libraries();
}

void server_wait_startup_barrier(void) {
    pthread_barrier_wait(&g_startup_barrier);
}

void server_shutdown_workers(void) {
    if (!g_reactor_bases) return;
    for (size_t i = 0; i < g_num_reactors; ++i) {
        struct event_base* base = atomic_load_explicit(&g_reactor_bases[i], memory_order_acquire);
        if (base != nullptr) {
            event_base_loopbreak(base);
        }
    }
}

static void server_record_request_stats(long long elapsed_ms, bool is_fast) {
    if (g_reactor_stats) {
        if (is_fast) {
            atomic_fetch_add_explicit(&g_reactor_stats[tl_reactor_id].total_requests_fast, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_reactor_stats[tl_reactor_id].total_time_fast_ms, elapsed_ms, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&g_reactor_stats[tl_reactor_id].total_requests_slow, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_reactor_stats[tl_reactor_id].total_time_slow_ms, elapsed_ms, memory_order_relaxed);
        }
    }
}

void server_get_request_stats(server_request_stats_t* out_stats) {
    if (!out_stats) return;
    
    uint64_t reqs_fast = 0;
    uint64_t time_fast = 0;
    uint64_t reqs_slow = 0;
    uint64_t time_slow = 0;
    if (g_reactor_stats) {
        for (size_t i = 0; i < g_num_reactors; ++i) {
            reqs_fast += atomic_load_explicit(&g_reactor_stats[i].total_requests_fast, memory_order_relaxed);
            time_fast += atomic_load_explicit(&g_reactor_stats[i].total_time_fast_ms, memory_order_relaxed);
            reqs_slow += atomic_load_explicit(&g_reactor_stats[i].total_requests_slow, memory_order_relaxed);
            time_slow += atomic_load_explicit(&g_reactor_stats[i].total_time_slow_ms, memory_order_relaxed);
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
    
    if (!mem_usage_kb) return;
    
    uint64_t mem_usage = 0;
    *mem_usage_kb = mem_usage;
    
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return;
    
    char buf[128];
    ssize_t bytes = read(fd, buf, sizeof(buf) - 1);
    if (bytes <= 0) {
        close(fd);
        return;
    }
    
    buf[bytes] = '\0';
    char* p = buf;
    while (*p && *p != ' ') p++;
    if (*p != ' ') {
        close(fd);
        return;
    }
    
    p++;
    
    // NOTE: We intentionally hand-roll this integer parser instead of using `strtoul`.
    // `strtoul` invokes locale-checking overhead, which degrades performance. Since 
    // we are parsing simple ASCII digits directly from the kernel (/proc/self/statm),
    // this manual loop is an intentional, significantly faster low-latency optimization.
    long resident = 0;
    while (*p >= '0' && *p <= '9') {
        resident = resident * 10 + (*p - '0');
        p++;
    }
    *mem_usage_kb = (uint64_t)((resident * g_page_size) / 1024);
    close(fd);
}

// middleware_ctx_t now in server.h

static const middleware_ctx_t g_routes[] = {
    { .path = "/ping", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &ping_handler, .is_fast = true, .auth_mode = AUTH_NONE },
    { .path = "/version", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &version_handler, .is_fast = true, .auth_mode = AUTH_API_KEY },
    { .path = "/sysinfo", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &sysinfo_handler, .is_fast = true, .auth_mode = AUTH_API_KEY },
    { .path = "/rsysinfo", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &rsysinfo_handler, .is_fast = false, .auth_mode = AUTH_JWT },
    { .path = "/rcustomer", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &CustomerContext, .handler = &rcustomer_handler, .is_fast = false, .auth_mode = AUTH_JWT },
    { .path = "/customer", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &CustomerContext, .handler = &customer_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/sales", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &SalesContext, .handler = &sales_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/shippers", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &shippers_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/products", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &products_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/uuid", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &uuid_handler, .is_fast = true, .auth_mode = AUTH_NONE },
    { .path = "/secretb32", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &secretb32_handler, .is_fast = true, .auth_mode = AUTH_NONE },
    { .path = "/login", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &LoginContext, .handler = &login_handler, .is_fast = false, .auth_mode = AUTH_NONE },
    { .path = "/getqr", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &getqr_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/verifytotp", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &VerifyTotpContext, .handler = &verifytotp_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/metrics", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &metrics_handler, .is_fast = true, .auth_mode = AUTH_API_KEY },
    { .path = "/employee", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &EmployeeContext, .handler = &employee_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/prodget", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &ProdgetContext, .handler = &prodget_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/customers", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &CustomersContext, .handler = &customers_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/salespgsql", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &SalesContext, .handler = &sales_pgsql_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/upload", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &UploadContext, .handler = &upload_handler, .is_fast = false, .auth_mode = AUTH_JWT },
    { .path = "/mcp", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = nullptr, .handler = &mcp_handler, .is_fast = true, .auth_mode = AUTH_NONE }
};
static const size_t g_route_count = sizeof(g_routes) / sizeof(g_routes[0]);



static long long measure_elapsed_ms(const struct timespec* start, const struct timespec* end) {
    long long seconds = (long long)(end->tv_sec - start->tv_sec);
    long long nanoseconds = (long long)(end->tv_nsec - start->tv_nsec);
    return (seconds * 1000LL) + (nanoseconds / 1000000LL);
}

void server_notify_task_done(void* arg) {
    http_task_t* task = arg;
    size_t rid = task->reactor_id;
    if (rid >= g_num_reactors) return;
    
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
    
    if (was_empty) {
        uint64_t one = 1;
        while (write(g_reactor_queues[rid].eventfd, &one, sizeof(one)) < 0) {
            if (errno == EINTR) continue;
            char err_buf[256];
            char* err_str = strerror_r(errno, err_buf, sizeof(err_buf));
            LOG_ERROR("Failed to write to reactor eventfd: %s", err_str);
            break;
        }
    }
}

static void request_on_complete_cb(struct evhttp_request *req, void *arg) {
    (void)req;
    http_task_t* task = arg;
    atomic_store(&task->cancelled, true);
}

static void cleanup_cancelled_task(http_task_t* task) {
    if (task->parsed_body) json_object_put(task->parsed_body);
    if (task->req) evhttp_request_free(task->req);
    task_pool_free(task);
}

static void send_http_reply(http_task_t* task, struct evbuffer* out_buf, struct evkeyvalq* headers, bool has_body) {
    if (has_body) {
        if (task->out_content_type[0] != '\0') {
            evhttp_add_header(headers, "Content-Type", task->out_content_type);
        } else if (!evhttp_find_header(headers, "Content-Type")) {
            evhttp_add_header(headers, "Content-Type", "application/json");
        }
        evhttp_send_reply(task->req, task->status_code, task->status_txt, out_buf);
    } else if (task->status_code >= 400) {
        if (!evhttp_find_header(headers, "Content-Type")) evhttp_add_header(headers, "Content-Type", "application/json");
        char err_str[256];
        if (task->status_code != HTTP_INTERNAL) {
            int len = snprintf(err_str, sizeof(err_str), "{\"error\":\"%s\"}", task->status_txt ? task->status_txt : "Error");
            evbuffer_add(out_buf, err_str, len < (int)sizeof(err_str) ? (size_t)len : sizeof(err_str) - 1);
            evhttp_send_reply(task->req, task->status_code, task->status_txt, out_buf);
        } else {
            int len = snprintf(err_str, sizeof(err_str), "{\"error\":\"Internal Server Error\"}");
            evbuffer_add(out_buf, err_str, len < (int)sizeof(err_str) ? (size_t)len : sizeof(err_str) - 1);
            evhttp_send_reply(task->req, HTTP_INTERNAL, "Internal Server Error", out_buf);
        }
    } else {
        evhttp_send_reply(task->req, task->status_code, task->status_txt, out_buf);
    }
}

static void process_completed_task(http_task_t* task) {
    evhttp_request_set_on_complete_cb(task->req, nullptr, nullptr);
    
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    long long elapsed_ms = measure_elapsed_ms(&task->start_time, &end_time);
    const middleware_ctx_t* ctx = (const middleware_ctx_t*)task->middleware_ctx;
    server_record_request_stats(elapsed_ms, ctx ? ctx->is_fast : false);
    
    logger_set_request_id(task->request_id);
    if (config_get_access_log()) LOG_INFO("clientIP=%s uri=%s elapsed_ms=%lld", task->client_ip, task->uri, elapsed_ms);
    
    struct evbuffer* out_buf = evhttp_request_get_output_buffer(task->req);
    if (task->worker_buf && evbuffer_get_length(task->worker_buf) > 0) {
        evbuffer_add_buffer(out_buf, task->worker_buf);
    }
    
    bool has_body = (evbuffer_get_length(out_buf) > 0);
    struct evkeyvalq* headers = evhttp_request_get_output_headers(task->req);
    
    if (ctx && strcmp(ctx->path, "/mcp") == 0) {
        evhttp_add_header(headers, "MCP-Protocol-Version", "2026-07-28");
    }

    send_http_reply(task, out_buf, headers, has_body);
    
    if (task->parsed_body) json_object_put(task->parsed_body);
    logger_clear_request_id();
    task_pool_free(task);
}

static void reactor_eventfd_cb(evutil_socket_t fd, short events, void *arg) {
    (void)events; (void)arg;
    uint64_t val;
    // Drain eventfd counter
    while (read(fd, &val, sizeof(val)) == sizeof(val)) {}
    
    size_t rid = tl_reactor_id;
    
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

static void inject_security_headers(struct evhttp_request* req) {
    struct evkeyvalq* headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(headers, "X-Content-Type-Options", "nosniff");
    evhttp_add_header(headers, "X-Frame-Options", "DENY");
    evhttp_add_header(headers, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    evhttp_add_header(headers, "Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
}

static bool validate_telemetry_api_key(struct evhttp_request* req) {
    char expected_key[MAX_CONFIG_STR] = {0};
    config_get_telemetry_api_key(expected_key, sizeof(expected_key));
    if (expected_key[0] == '\0') {
        LOG_WARN("TELEMETRY_API_KEY is not configured!");
        return false;
    }
    
    struct evkeyvalq* headers = evhttp_request_get_input_headers(req);
    const char* auth_header = evhttp_find_header(headers, "X-API-Key");
    const char* bearer = evhttp_find_header(headers, "Authorization");
    
    char provided_key[MAX_CONFIG_STR] = {0};
    if (auth_header) {
        (void)snprintf(provided_key, sizeof(provided_key), "%s", auth_header);
    } else if (bearer && strncasecmp(bearer, "Bearer ", 7) == 0) {
        (void)snprintf(provided_key, sizeof(provided_key), "%s", bearer + 7);
    }
    
    // Constant-time compare over the entire maximum buffer size to prevent length leakage
    int match = sodium_memcmp(provided_key, expected_key, MAX_CONFIG_STR);
    bool valid_len = (provided_key[0] != '\0');
    
    sodium_memzero(expected_key, sizeof(expected_key));
    sodium_memzero(provided_key, sizeof(provided_key));
    
    return (match == 0 && valid_len);
}


static bool server_validate_cors(struct evhttp_request* req) {
    struct evkeyvalq* in_headers = evhttp_request_get_input_headers(req);
    const char* origin = evhttp_find_header(in_headers, "Origin");
    if (origin) {
        if (config_is_origin_allowed(origin)) {
            struct evkeyvalq* out_headers = evhttp_request_get_output_headers(req);
            evhttp_add_header(out_headers, "Access-Control-Allow-Origin", origin);
            evhttp_add_header(out_headers, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            evhttp_add_header(out_headers, "Access-Control-Allow-Headers", "Content-Type, Authorization, X-API-Key");
            evhttp_add_header(out_headers, "Access-Control-Max-Age", "86400");
            evhttp_add_header(out_headers, "Vary", "Origin");
        } else {
            const char* client_ip = server_extract_client_ip(req);
            LOG_WARN("CORS validation failed for Origin: '%s' from IP: %s accessing URI: %s", origin, client_ip, evhttp_request_get_uri(req));
            struct evbuffer* out_buf = evhttp_request_get_output_buffer(req);
            const char* msg = "{\"error\":\"CORS origin not allowed.\"}";
            evbuffer_add(out_buf, msg, strlen(msg));
            evhttp_send_reply(req, 403, "Forbidden", nullptr);
            return false;
        }
    }
    return true;
}

static bool server_validate_method_and_auth(struct evhttp_request* req, const middleware_ctx_t* ctx) {
    if (evhttp_request_get_command(req) != ctx->allowed_method) {
        struct evbuffer* out_buf = evhttp_request_get_output_buffer(req);
        const char* msg = "{\"error\":\"Method not permitted.\"}";
        evbuffer_add(out_buf, msg, strlen(msg));
        evhttp_send_reply(req, HTTP_BADMETHOD, "Method Not Allowed", nullptr);
        return false;
    }
    
    if (ctx->auth_mode == AUTH_API_KEY) {
        if (!validate_telemetry_api_key(req)) {
            struct evbuffer* out_buf = evhttp_request_get_output_buffer(req);
            const char* msg = "{\"error\":\"Access Denied\"}";
            evbuffer_add(out_buf, msg, strlen(msg));
            evhttp_send_reply(req, 403, "Forbidden", nullptr);
            return false;
        }
    }
    return true;
}

static void server_enqueue_task(struct evhttp_request* req, const middleware_ctx_t* ctx, struct timespec start_time) {
    const char* extracted_client_ip = server_extract_client_ip(req);
    struct evkeyvalq* in_headers = evhttp_request_get_input_headers(req);

    http_task_t* task = task_pool_alloc();
    if (!task) {
        struct evbuffer* out_buf = evhttp_request_get_output_buffer(req);
        const char* msg = "{\"error\":\"Server Too Busy\"}";
        evbuffer_add(out_buf, msg, strlen(msg));
        evhttp_send_reply(req, HTTP_SERVUNAVAIL, "Service Unavailable", nullptr);
        return;
    }
    
    const char* raw_uri = evhttp_request_get_uri(req);
    const char* req_id = evhttp_find_header(in_headers, "X-Request-Id");
    
    if (strlen(raw_uri) >= sizeof(task->uri) || 
        (req_id && strlen(req_id) >= sizeof(task->request_id)) ||
        (extracted_client_ip && strlen(extracted_client_ip) >= sizeof(task->client_ip))) {
        
        task_pool_free(task);
        struct evbuffer* out_buf = evhttp_request_get_output_buffer(req);
        const char* msg = "{\"error\":\"Request Headers or URI Too Long\"}";
        evbuffer_add(out_buf, msg, strlen(msg));
        evhttp_send_reply(req, HTTP_BADREQUEST, "Bad Request", nullptr);
        return;
    }

    task->req = req;
    task->parsed_body = nullptr;
    task->middleware_ctx = ctx;
    task->start_time = start_time;
    task->reactor_id = tl_reactor_id;
    task->username[0] = '\0';
    task->session_id[0] = '\0';
    (void)snprintf(task->client_ip, sizeof(task->client_ip), "%s", extracted_client_ip ? extracted_client_ip : "unknown");
    (void)snprintf(task->uri, sizeof(task->uri), "%s", raw_uri);
    (void)snprintf(task->request_id, sizeof(task->request_id), "%s", req_id ? req_id : "");
    
    atomic_store_explicit(&task->cancelled, false, memory_order_release);
    
    evhttp_request_set_on_complete_cb(req, request_on_complete_cb, task);
    
    evhttp_request_own(req);
    
    if (!worker_pool_enqueue(task)) {
        evhttp_request_set_on_complete_cb(req, nullptr, nullptr);
        struct evbuffer* out_buf = evhttp_request_get_output_buffer(req);
        const char* msg = "{\"error\":\"Server Too Busy\"}";
        evbuffer_add(out_buf, msg, strlen(msg));
        evhttp_send_reply(req, HTTP_SERVUNAVAIL, "Service Unavailable", nullptr);
        task_pool_free(task);
        evhttp_request_free(req);
    }
}

static void api_middleware_wrapper(struct evhttp_request* req, void* arg) {
    inject_security_headers(req);

    const middleware_ctx_t* ctx = (const middleware_ctx_t*)arg;
    if (ctx == nullptr || ctx->handler == nullptr) {
        evhttp_send_error(req, HTTP_INTERNAL, "Middleware Routing Fault");
        return;
    }
    
    if (!server_validate_cors(req)) {
        return;
    }
    
    if (evhttp_request_get_command(req) == EVHTTP_REQ_OPTIONS) {
        evhttp_send_reply(req, 204, "No Content", nullptr);
        return;
    }

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    struct evkeyvalq* in_headers = evhttp_request_get_input_headers(req);
    const char* x_forwarded_for = evhttp_find_header(in_headers, "X-Forwarded-For");
    if (x_forwarded_for) {
        const char* peer_ip = nullptr;
        if (!is_trusted_proxy(req, &peer_ip)) {
            LOG_WARN("Untrusted X-Forwarded-For header '%s' from peer %s for URI %s",
                     x_forwarded_for, peer_ip ? peer_ip : "unknown", evhttp_request_get_uri(req));
        }
    }

    if (!server_validate_method_and_auth(req, ctx)) {
        return;
    }
    
    server_enqueue_task(req, ctx, start_time);
}

static struct event_base* create_optimized_event_base(void) {
    [[gnu::cleanup(cleanup_event_config)]] struct event_config* cfg = event_config_new();
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
    if (inet_pton(AF_INET, addr, &sin.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr*)&sin, sizeof(sin)) < 0 || listen(fd, SOMAXCONN) < 0) {
        close(fd);
        return -1;
    }
    
    return fd;
}

static struct evhttp* configure_http_server(struct event_base* base, evutil_socket_t fd, const middleware_ctx_t* routes, size_t route_count) {
    struct evhttp* http = evhttp_new(base);
    if (http == nullptr) return nullptr;

    evhttp_set_max_body_size(http, config_get_max_payload_size());
    evhttp_set_max_headers_size(http, 8192); // 8KB max header size
    evhttp_set_timeout(http, REQUEST_TIMEOUT_SECONDS); 
    evhttp_set_allowed_methods(http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_OPTIONS);

    for (size_t i = 0; i < route_count; ++i) {
        evhttp_set_cb(http, routes[i].path, api_middleware_wrapper, (void*)&routes[i]);
    }
    
    if (evhttp_accept_socket_with_handle(http, fd) == nullptr) {
        evhttp_free(http);
        return nullptr;
    }
    return http;
}

void* reactor_thread_logic(void* arg) {
    size_t worker_id = (size_t)arg;
    tl_reactor_id = worker_id;
    
    [[gnu::cleanup(cleanup_event_base)]] struct event_base* base = create_optimized_event_base();
    if (base == nullptr) {
        atomic_store_explicit(&g_startup_failed, true, memory_order_release);
        pthread_barrier_wait(&g_startup_barrier);
        return nullptr;
    }
    
    atomic_store_explicit(&g_reactor_bases[worker_id], base, memory_order_release);

    int efd = g_reactor_queues[worker_id].eventfd;
    
    struct event* efd_ev = event_new(base, efd, EV_READ | EV_PERSIST, reactor_eventfd_cb, nullptr);
    event_add(efd_ev, nullptr);

    evutil_socket_t fd = create_and_bind_socket((uint16_t)SERVER_PORT, SERVER_ADDR);
    if (fd < 0) {
        char errbuf[256];
        LOG_FATAL("Failed to bind socket for Reactor %zu: %s", worker_id, strerror_r(errno, errbuf, sizeof(errbuf)));
        atomic_store_explicit(&g_startup_failed, true, memory_order_release);
        event_free(efd_ev);
        atomic_store_explicit(&g_reactor_bases[worker_id], nullptr, memory_order_release);
        pthread_barrier_wait(&g_startup_barrier);
        return nullptr;
    }

    [[gnu::cleanup(cleanup_evhttp)]] struct evhttp* http = configure_http_server(base, fd, g_routes, g_route_count);
    if (http == nullptr) {
        LOG_FATAL("Failed to configure HTTP server for Reactor %zu", worker_id);
        close(fd);
        atomic_store_explicit(&g_startup_failed, true, memory_order_release);
        event_free(efd_ev);
        atomic_store_explicit(&g_reactor_bases[worker_id], nullptr, memory_order_release);
        pthread_barrier_wait(&g_startup_barrier);
        return nullptr;
    }

    unsigned long long tid = (unsigned long long)pthread_self();
    (void)snprintf(tl_tid_str, sizeof(tl_tid_str), "0x%llx", tid);
    LOG_INFO("Reactor %zu started", worker_id);
    
    pthread_barrier_wait(&g_startup_barrier);
    
    if (atomic_load_explicit(&g_startup_failed, memory_order_acquire)) {
        LOG_INFO("Reactor %zu aborting due to startup failure in another reactor", worker_id);
        close(fd);
        event_free(efd_ev);
        atomic_store_explicit(&g_reactor_bases[worker_id], nullptr, memory_order_release);
        return nullptr;
    }
    
    event_base_dispatch(base);

    LOG_INFO("Reactor %zu stopped", worker_id);
    event_free(efd_ev);
    return nullptr;
}
