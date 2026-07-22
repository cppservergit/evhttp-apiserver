#include "worker_pool.h"
#include "server.h"
#include "http_client.h"
#include "logger.h"
#include "config.h"
#include "jwt.h"
#include "handlers.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sodium.h>
#include <event2/buffer.h>
#include "raii.h"

typedef struct {
    http_task_t* head;
    http_task_t* tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    size_t size;
    size_t num_workers;
    pthread_t* workers;
    bool shutdown;
    bool initialized;
} pool_t;

static pool_t g_fast_pool = {0};
static pool_t g_slow_pool = {0};


static void* worker_thread_main(void* arg) {
    pool_t* pool = (pool_t*)arg;
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
        if (pool->head == nullptr) pool->tail = nullptr;
        pool->size--;
        pthread_mutex_unlock(&pool->mutex);
        
        if (!atomic_load(&task->cancelled)) {
            const middleware_ctx_t* ctx = (const middleware_ctx_t*)task->middleware_ctx;
            logger_set_request_id(task->request_id);
            
            bool is_authorized = true;

            if (ctx && ctx->auth_mode == AUTH_JWT) {
                struct evkeyvalq* in_headers = evhttp_request_get_input_headers(task->req);
                const char* auth_hdr = evhttp_find_header(in_headers, "Authorization");
                if (!auth_hdr || strncmp(auth_hdr, "Bearer ", 7) != 0) {
                    task->status_code = 403;
                    task->status_txt = "Forbidden";
                    const char* msg = "{\"error\":\"Missing or invalid Authorization header\"}";
                    evbuffer_add(task->worker_buf, msg, strlen(msg));
                    is_authorized = false;
                } else {
                    char jwt_secret[MAX_CONFIG_STR];
                    config_get_jwt_secret(jwt_secret, sizeof(jwt_secret));
                    int jwt_res = jwt_verify(auth_hdr + 7, jwt_secret, task->username, sizeof(task->username), task->session_id, sizeof(task->session_id));
                    sodium_memzero(jwt_secret, sizeof(jwt_secret));
                    
                    if (jwt_res == JWT_ERR_EXPIRED) {
                        task->status_code = 401;
                        task->status_txt = "Unauthorized";
                        const char* msg = "{\"error\":\"Token has expired\"}";
                        evbuffer_add(task->worker_buf, msg, strlen(msg));
                        is_authorized = false;
                    } else if (jwt_res != JWT_OK) {
                        task->status_code = 403;
                        task->status_txt = "Forbidden";
                        const char* msg = "{\"error\":\"Invalid token\"}";
                        evbuffer_add(task->worker_buf, msg, strlen(msg));
                        is_authorized = false;
                    }
                }
                if (is_authorized) {
                    handlers_set_context(task->username, task->session_id, task->client_ip, task->uri);
                }
            } else {
                handlers_set_context(nullptr, nullptr, task->client_ip, task->uri);
            }
            
            bool body_ok = true;
            if (is_authorized && evhttp_request_get_command(task->req) == EVHTTP_REQ_POST) {
                struct evkeyvalq* in_headers = evhttp_request_get_input_headers(task->req);
                const char* ctype = evhttp_find_header(in_headers, "Content-Type");
                if (ctype == nullptr || strncasecmp(ctype, "application/json", 16) != 0) {
                    task->status_code = HTTP_BADREQUEST;
                    task->status_txt = "Bad Request";
                    const char* msg = "{\"error\":\"Invalid Content-Type. Expected application/json.\"}";
                    evbuffer_add(task->worker_buf, msg, strlen(msg));
                    body_ok = false;
                } else {
                    struct evbuffer* in_buf = evhttp_request_get_input_buffer(task->req);
                    size_t len = evbuffer_get_length(in_buf);
                    if (len == 0) {
                        task->status_code = HTTP_BADREQUEST;
                        task->status_txt = "Bad Request";
                        const char* msg = "{\"error\":\"Empty request body.\"}";
                        evbuffer_add(task->worker_buf, msg, strlen(msg));
                        body_ok = false;
                    } else {
                        unsigned char* data = evbuffer_pullup(in_buf, -1);
                        if (!data) {
                            task->status_code = HTTP_INTERNAL;
                            task->status_txt = "Internal Server Error";
                            const char* msg = "{\"error\":\"Failed to pull up request body buffer.\"}";
                            evbuffer_add(task->worker_buf, msg, strlen(msg));
                            body_ok = false;
                        } else {
                            raii_json_tokener tok = json_tokener_new();
                            task->parsed_body = json_tokener_parse_ex(tok, (const char*)data, (int)len);
                            enum json_tokener_error jerr = json_tokener_get_error(tok);
                            if (!task->parsed_body || jerr != json_tokener_success || !json_object_is_type(task->parsed_body, json_type_object)) {
                                task->status_code = HTTP_BADREQUEST;
                                task->status_txt = "Bad Request";
                                const char* msg = "{\"error\":\"Invalid JSON payload or not a JSON object.\"}";
                                evbuffer_add(task->worker_buf, msg, strlen(msg));
                                body_ok = false;
                            }
                        }
                    }
                }
            }
            
            bool valid = true;
            if (is_authorized && body_ok && ctx && ctx->validation_ctx != nullptr && task->parsed_body != nullptr) {
                char err_buf[MAX_ERR_MSG_LEN] = {0};
                if (!validate_json(ctx->validation_ctx, task->parsed_body, err_buf, sizeof(err_buf))) {
                    task->status_code = HTTP_BADREQUEST;
                    task->status_txt = "Bad Request";
                    char ev_err[1024];
                    int len = snprintf(ev_err, sizeof(ev_err), "{\"error\":\"%s\"}", err_buf);
                    evbuffer_add(task->worker_buf, ev_err, len < (int)sizeof(ev_err) ? (size_t)len : sizeof(ev_err) - 1);
                    valid = false;
                }
            }
            
            if (is_authorized && body_ok && valid) {
                if (ctx && ctx->handler) {
                    ctx->handler(task->parsed_body, ctx->user_arg, &task->status_code, &task->status_txt, task->worker_buf);
                    const char* ctype = get_content_type();
                    if (ctype) {
                        snprintf(task->out_content_type, sizeof(task->out_content_type), "%s", ctype);
                    } else {
                        task->out_content_type[0] = '\0';
                    }
                }
            }
            
            logger_clear_request_id();
            handlers_clear_context();
        }
        
        // Notify reactor
        server_notify_task_done(task);
    }
    
    http_client_cleanup_thread();
    return nullptr;
}

int worker_pool_init(size_t num_workers) {
    // Calculate sizes (fast vs slow)
    
    // Allocate dynamic percentage of workers to the fast pool (minimum 1)
    size_t pct = config_get_fast_pool_percentage();
    size_t fast_workers = (num_workers * pct) / 100;
    
    // Guarantee at least 1 fast worker
    if (fast_workers == 0) fast_workers = 1;
    
    // Guarantee at least 1 slow worker without exceeding total num_workers (if > 1)
    if (fast_workers == num_workers && num_workers > 1) {
        fast_workers = num_workers - 1;
    }
    
    size_t slow_workers = (num_workers > fast_workers) ? (num_workers - fast_workers) : 1;
    
    pthread_mutex_init(&g_fast_pool.mutex, nullptr);
    pthread_cond_init(&g_fast_pool.cond, nullptr);
    g_fast_pool.initialized = true;
    g_fast_pool.num_workers = fast_workers;
    g_fast_pool.workers = calloc(fast_workers, sizeof(pthread_t));
    if (!g_fast_pool.workers) {
        LOG_FATAL("Out of memory allocating fast pool workers");
        return -1;
    }
    for (size_t i = 0; i < fast_workers; ++i) {
        if (pthread_create(&g_fast_pool.workers[i], nullptr, worker_thread_main, &g_fast_pool) != 0) {
            LOG_ERROR("Failed to create fast pool worker thread %zu", i);
            g_fast_pool.num_workers = i;
            worker_pool_shutdown();
            return -1;
        }
    }
    
    pthread_mutex_init(&g_slow_pool.mutex, nullptr);
    pthread_cond_init(&g_slow_pool.cond, nullptr);
    g_slow_pool.initialized = true;
    g_slow_pool.num_workers = slow_workers;
    g_slow_pool.workers = calloc(slow_workers, sizeof(pthread_t));
    if (!g_slow_pool.workers) {
        LOG_FATAL("Out of memory allocating slow pool workers");
        worker_pool_shutdown();
        return -1;
    }
    for (size_t i = 0; i < slow_workers; ++i) {
        if (pthread_create(&g_slow_pool.workers[i], nullptr, worker_thread_main, &g_slow_pool) != 0) {
            LOG_ERROR("Failed to create slow pool worker thread %zu", i);
            g_slow_pool.num_workers = i;
            worker_pool_shutdown();
            return -1;
        }
    }
    
    LOG_INFO("Initialized async worker pool with %zu threads (%zu fast, %zu slow)", num_workers, fast_workers, slow_workers);
    return 0;
}

void worker_pool_shutdown(void) {
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
    
    size_t max_size = config_get_max_queue_size();
    if (max_size > 0 && pool->size >= max_size) {
        pthread_mutex_unlock(&pool->mutex);
        return false;
    }
    
    task->next = nullptr;
    
    if (pool->tail == nullptr) {
        pool->head = pool->tail = task;
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
