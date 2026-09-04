#include <apiserver/worker_pool.h>
#include <apiserver/server.h>
#include <apiserver/http_client.h>
#include <apiserver/logger.h>
#include <apiserver/config.h>
#include <apiserver/jwt.h>
#include <apiserver/context.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sodium.h>
#include <event2/buffer.h>
#include <apiserver/raii.h>
#include <apiserver/thread_error.h>
#include <stdalign.h>

typedef struct {
    alignas(64) pthread_cond_t cond;
    pthread_mutex_t mutex;
    http_task_t* head;
    http_task_t* tail;
    pthread_t* workers;
    size_t size;
    size_t num_workers;
    bool shutdown;
    bool initialized;
    char _padding[62];
} pool_t;

static pool_t g_fast_pool = {0};
static pool_t g_slow_pool = {0};

static const char* get_http_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "Unknown Status";
    }
}



static bool worker_process_jwt(http_task_t* task, const middleware_ctx_t* ctx) {
    if (!ctx || ctx->auth_mode != AUTH_JWT) {
        context_set(nullptr, nullptr, task->client_ip, task->uri);
        return true;
    }

    const struct evkeyvalq* in_headers = evhttp_request_get_input_headers(task->req);
    const char* auth_hdr = evhttp_find_header(in_headers, "Authorization");
    if (!auth_hdr || strncasecmp(auth_hdr, "Bearer ", 7) != 0) {
        task->status_code = 401;
        task->status_txt = "Unauthorized";
        const char* msg = "{\"error\":\"Missing or invalid Authorization header\"}";
        evbuffer_add(task->worker_buf, msg, strlen(msg));
        return false;
    }

    const char* jwt_secret = config_get_jwt_secret();
    int jwt_res = jwt_verify(&auth_hdr[7], jwt_secret, task->username, sizeof(task->username), task->session_id, sizeof(task->session_id));
    
    if (jwt_res == JWT_ERR_EXPIRED) {
        task->status_code = 401;
        task->status_txt = "Unauthorized";
        const char* msg = "{\"error\":\"Token has expired\"}";
        evbuffer_add(task->worker_buf, msg, strlen(msg));
        return false;
    } 
    
    if (jwt_res != JWT_OK) {
        task->status_code = 401;
        task->status_txt = "Unauthorized";
        const char* msg = "{\"error\":\"Invalid token\"}";
        evbuffer_add(task->worker_buf, msg, strlen(msg));
        return false;
    }

    context_set(task->username, task->session_id, task->client_ip, task->uri);
    return true;
}

static bool worker_process_payload(http_task_t* task) {
    if (evhttp_request_get_command(task->req) != EVHTTP_REQ_POST) {
        return true;
    }

    const struct evkeyvalq* in_headers = evhttp_request_get_input_headers(task->req);
    const char* ctype = evhttp_find_header(in_headers, "Content-Type");
    if (ctype == nullptr || strncasecmp(ctype, "application/json", 16) != 0 || (ctype[16] != '\0' && ctype[16] != ';' && ctype[16] != ' ')) {
        task->status_code = HTTP_BADREQUEST;
        task->status_txt = "Bad Request";
        const char* msg = "{\"error\":\"Invalid Content-Type. Expected application/json.\"}";
        evbuffer_add(task->worker_buf, msg, strlen(msg));
        return false;
    }
    
    struct evbuffer* in_buf = evhttp_request_get_input_buffer(task->req);
    size_t len = evbuffer_get_length(in_buf);
    if (len == 0) {
        task->status_code = HTTP_BADREQUEST;
        task->status_txt = "Bad Request";
        const char* msg = "{\"error\":\"Empty request body.\"}";
        evbuffer_add(task->worker_buf, msg, strlen(msg));
        return false;
    }
    [[gnu::cleanup(cleanup_json_tokener)]] struct json_tokener* tok = json_tokener_new();
    
    struct evbuffer_ptr ptr;
    evbuffer_ptr_set(in_buf, &ptr, 0, EVBUFFER_PTR_SET);
    struct evbuffer_iovec v[1];
    
    while (evbuffer_peek(in_buf, -1, &ptr, v, 1) > 0) {
        struct json_object* obj = json_tokener_parse_ex(tok, (const char*)v[0].iov_base, (int)v[0].iov_len);
        if (obj) {
            task->parsed_body = obj;
        }
        enum json_tokener_error jerr = json_tokener_get_error(tok);
        if (jerr == json_tokener_success) break;
        if (jerr != json_tokener_continue) break;
        if (evbuffer_ptr_set(in_buf, &ptr, v[0].iov_len, EVBUFFER_PTR_ADD) < 0) break;
    }
    
    enum json_tokener_error jerr = json_tokener_get_error(tok);
    if (!task->parsed_body || jerr != json_tokener_success || !json_object_is_type(task->parsed_body, json_type_object)) {
        task->status_code = HTTP_BADREQUEST;
        task->status_txt = "Bad Request";
        const char* msg = "{\"error\":\"Invalid JSON payload or not a JSON object.\"}";
        evbuffer_add(task->worker_buf, msg, strlen(msg));
        return false;
    }
    
    return true;
}

static bool worker_process_validation(http_task_t* task, const middleware_ctx_t* ctx) {
    if (ctx && ctx->validation_ctx != nullptr && task->parsed_body != nullptr) {
        char err_buf[MAX_ERR_MSG_LEN] = {0};
        if (!validate_json(ctx->validation_ctx, task->parsed_body, err_buf, sizeof(err_buf))) {
            task->status_code = HTTP_BADREQUEST;
            task->status_txt = "Bad Request";
            char ev_err[1024];
            int len = snprintf(ev_err, sizeof(ev_err), "{\"error\":\"%s\"}", err_buf);
            evbuffer_add(task->worker_buf, ev_err, len < (int)sizeof(ev_err) ? (size_t)len : sizeof(ev_err) - 1);
            return false;
        }
    }
    return true;
}

static void worker_process_task(http_task_t* task) {
    const middleware_ctx_t* ctx = (const middleware_ctx_t*)task->middleware_ctx;
    logger_set_request_id(task->request_id);
    
    if (worker_process_jwt(task, ctx) && worker_process_payload(task) && worker_process_validation(task, ctx)) {
        if (ctx && ctx->handler) {
            if (task->parsed_body) {
                ctx->handler(task->parsed_body, &task->status_code, task->worker_buf);
            } else {
                ctx->handler(nullptr, &task->status_code, task->worker_buf);
            }
            const char* ctype = context_get_content_type();
            if (ctype) {
                (void)snprintf(task->out_content_type, sizeof(task->out_content_type), "%s", ctype);
            } else {
                task->out_content_type[0] = '\0';
            }
        }
    }
    
    if (!task->status_txt) {
        task->status_txt = get_http_status_text(task->status_code);
    }

    ThreadErrorLevel err_lvl = get_thread_error_level();
    if (err_lvl == TL_ERR_ERROR) {
        LOG_ERROR("Request failed for URI %s: %s", task->uri, get_thread_error_msg());
    } else if (err_lvl == TL_ERR_WARN) {
        LOG_WARN("Request warning for URI %s: %s", task->uri, get_thread_error_msg());
    } else if (task->status_code >= 500) {
        LOG_ERROR("Request failed with status %d for URI %s", task->status_code, task->uri);
    } else {
        // MISRA 15.7: Fallback else
    }
    clear_thread_error();
    
    logger_clear_request_id();
    context_clear();
}

static void* worker_thread_main(void* arg) {
    pool_t* pool = arg;
    http_client_init_thread();
    
    while (1) {
        pthread_mutex_lock(&pool->mutex);
        while (pool->head == nullptr && !pool->shutdown) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }
        
        if (pool->shutdown && pool->head == nullptr) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }
        
        http_task_t* task = pool->head;
        pool->head = task->next;
        if (pool->head == nullptr) { pool->tail = nullptr; }
        pool->size--;
        pthread_mutex_unlock(&pool->mutex);
        
        if (!atomic_load(&task->cancelled)) {
            worker_process_task(task);
        }
        
        server_notify_task_done(task);
    }
    
    http_client_cleanup_thread();
    return nullptr;
}

static int init_pool(pool_t* pool, size_t num_workers, const char* name) {
    pthread_mutex_init(&pool->mutex, nullptr);
    pthread_cond_init(&pool->cond, nullptr);
    pool->initialized = true;
    pool->num_workers = num_workers;
    pool->workers = calloc(num_workers, sizeof(pthread_t));
    if (!pool->workers) {
        char errbuf[256];
        LOG_FATAL("Out of memory allocating %s pool workers: %s", name, strerror_r(errno, errbuf, sizeof(errbuf)));
        return -1;
    }
    for (size_t i = 0; i < num_workers; ++i) {
        int rc = pthread_create(&pool->workers[i], nullptr, worker_thread_main, pool);
        if (rc != 0) {
            char errbuf[256];
            LOG_ERROR("Failed to create %s pool worker thread %zu: %s", name, i, strerror_r(rc, errbuf, sizeof(errbuf)));
            pool->num_workers = i;
            return -1;
        }
    }
    return 0;
}

int worker_pool_init(size_t num_workers) {
    size_t pct = config_get_fast_pool_percentage();
    size_t fast_workers = (num_workers * pct) / 100;
    
    if (fast_workers == 0) fast_workers = 1;
    
    if (fast_workers == num_workers && num_workers > 1) {
        fast_workers = num_workers - 1;
    }
    
    size_t slow_workers = (num_workers > fast_workers) ? (num_workers - fast_workers) : 1;
    
    if (init_pool(&g_fast_pool, fast_workers, "fast") != 0) {
        worker_pool_shutdown();
        return -1;
    }
    
    if (init_pool(&g_slow_pool, slow_workers, "slow") != 0) {
        worker_pool_shutdown();
        return -1;
    }
    
    LOG_INFO("Initialized async worker pool with %zu threads (%zu fast, %zu slow)", num_workers, fast_workers, slow_workers);
    return 0;
}

void worker_pool_stop(void) {
    if (g_fast_pool.initialized) {
        pthread_mutex_lock(&g_fast_pool.mutex);
        g_fast_pool.shutdown = true;
        pthread_mutex_unlock(&g_fast_pool.mutex);
        pthread_cond_broadcast(&g_fast_pool.cond);
    }
    
    if (g_slow_pool.initialized) {
        pthread_mutex_lock(&g_slow_pool.mutex);
        g_slow_pool.shutdown = true;
        pthread_mutex_unlock(&g_slow_pool.mutex);
        pthread_cond_broadcast(&g_slow_pool.cond);
    }
    
    if (g_fast_pool.workers) {
        for (size_t i = 0; i < g_fast_pool.num_workers; ++i) {
            pthread_join(g_fast_pool.workers[i], nullptr);
        }
        free(g_fast_pool.workers);
        g_fast_pool.workers = nullptr;
    }
    
    if (g_slow_pool.workers) {
        for (size_t i = 0; i < g_slow_pool.num_workers; ++i) {
            pthread_join(g_slow_pool.workers[i], nullptr);
        }
        free(g_slow_pool.workers);
        g_slow_pool.workers = nullptr;
    }
}

void worker_pool_shutdown(void) {
    worker_pool_stop();
    
    if (g_fast_pool.initialized) {
        pthread_mutex_destroy(&g_fast_pool.mutex);
        pthread_cond_destroy(&g_fast_pool.cond);
        g_fast_pool.initialized = false;
    }
    if (g_slow_pool.initialized) {
        pthread_mutex_destroy(&g_slow_pool.mutex);
        pthread_cond_destroy(&g_slow_pool.cond);
        g_slow_pool.initialized = false;
    }
}

bool worker_pool_enqueue(http_task_t* task) {
    const middleware_ctx_t* ctx = (const middleware_ctx_t*)task->middleware_ctx;
    pool_t* pool = (ctx && ctx->is_fast) ? &g_fast_pool : &g_slow_pool;
    
    pthread_mutex_lock(&pool->mutex);
    
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        return false;
    }
    
    size_t max_size = config_get_max_queue_size();
    if (max_size > 0 && pool->size >= max_size) {
        pthread_mutex_unlock(&pool->mutex);
        return false;
    }
    
    task->next = nullptr;
    
    if (pool->tail == nullptr) {
        pool->tail = task;
        pool->head = task;
    } else {
        pool->tail->next = task;
        pool->tail = task;
    }
    pool->size++;
    pthread_mutex_unlock(&pool->mutex);
    
    // Unlock-before-Signal pattern: Wake up one thread outside the critical section
    // to prevent the woken thread from immediately blocking on the mutex.
    pthread_cond_signal(&pool->cond);
    return true;
}

size_t worker_pool_get_size(void) {
    return g_fast_pool.num_workers + g_slow_pool.num_workers;
}
