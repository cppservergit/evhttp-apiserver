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
            struct evkeyvalq* in_headers = evhttp_request_get_input_headers(task->req);
            const char* req_id = evhttp_find_header(in_headers, "X-Request-Id");
            logger_set_request_id(req_id);
            
            bool is_authorized = true;

            if (ctx && ctx->auth_mode == AUTH_JWT) {
                handlers_set_identity(task->username, task->session_id);
            }
            
            const char* x_forwarded_for = evhttp_find_header(in_headers, "X-Forwarded-For");
            if (x_forwarded_for) {
                const char* peer_ip = nullptr;
                if (!is_trusted_proxy(task->req, &peer_ip)) {
                    if (ctx && ctx->auth_mode == AUTH_JWT && is_authorized) {
                        LOG_WARN("Untrusted X-Forwarded-For header '%s' from peer %s for URI %s (User: %s, Session: %s)",
                                 x_forwarded_for, peer_ip ? peer_ip : "unknown", evhttp_request_get_uri(task->req), task->username, task->session_id);
                    } else {
                        LOG_WARN("Untrusted X-Forwarded-For header '%s' from peer %s for URI %s",
                                 x_forwarded_for, peer_ip ? peer_ip : "unknown", evhttp_request_get_uri(task->req));
                    }
                }
            }
            
            if (is_authorized) {
                if (ctx && ctx->handler) {
                    task->worker_buf = evbuffer_new();
                    ctx->handler(task->req, task->parsed_body, ctx->user_arg, &task->status_code, &task->status_txt, task->worker_buf);
                }
            }
            
            
            logger_clear_request_id();
            handlers_clear_identity();
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
    if (fast_workers == 0) fast_workers = 1;
    if (fast_workers > num_workers) fast_workers = num_workers;
    
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
