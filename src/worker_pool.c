#include "worker_pool.h"
#include "server.h"
#include "http_client.h"
#include "logger.h"
#include "config.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

typedef struct task_node {
    http_task_t* task;
    struct task_node* next;
} task_node_t;

typedef struct {
    task_node_t* head;
    task_node_t* tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    size_t size;
    size_t num_workers;
    pthread_t* workers;
    bool shutdown;
} pool_t;

static pool_t g_fast_pool = {0};
static pool_t g_slow_pool = {0};
static size_t g_total_workers = 0;

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
        
        task_node_t* node = pool->head;
        pool->head = node->next;
        if (pool->head == nullptr) pool->tail = nullptr;
        pool->size--;
        pthread_mutex_unlock(&pool->mutex);
        
        http_task_t* task = node->task;
        free(node);
        
        if (!atomic_load(&task->cancelled)) {
            const middleware_ctx_t* ctx = (const middleware_ctx_t*)task->middleware_ctx;
            if (ctx && ctx->json_handler) {
                task->response_json = ctx->json_handler(nullptr, task->parsed_body, ctx->user_arg, &task->status_code, &task->status_txt);
            } else if (ctx && ctx->text_handler) {
                task->response_text = ctx->text_handler(nullptr, task->parsed_body, ctx->user_arg, &task->status_code, &task->status_txt);
            }
        }
        
        // Notify reactor
        server_notify_task_done(task);
    }
    
    http_client_cleanup_thread();
    return nullptr;
}

int worker_pool_init(size_t num_workers) {
    g_total_workers = num_workers;
    
    // Allocate dynamic percentage of workers to the fast pool (minimum 1)
    size_t pct = config_get_fast_pool_percentage();
    size_t fast_workers = (num_workers * pct) / 100;
    if (fast_workers == 0) fast_workers = 1;
    if (fast_workers > num_workers) fast_workers = num_workers;
    
    size_t slow_workers = (num_workers > fast_workers) ? (num_workers - fast_workers) : 1;
    
    pthread_mutex_init(&g_fast_pool.mutex, nullptr);
    pthread_cond_init(&g_fast_pool.cond, nullptr);
    g_fast_pool.num_workers = fast_workers;
    g_fast_pool.workers = calloc(fast_workers, sizeof(pthread_t));
    for (size_t i = 0; i < fast_workers; ++i) {
        pthread_create(&g_fast_pool.workers[i], nullptr, worker_thread_main, &g_fast_pool);
    }
    
    pthread_mutex_init(&g_slow_pool.mutex, nullptr);
    pthread_cond_init(&g_slow_pool.cond, nullptr);
    g_slow_pool.num_workers = slow_workers;
    g_slow_pool.workers = calloc(slow_workers, sizeof(pthread_t));
    for (size_t i = 0; i < slow_workers; ++i) {
        pthread_create(&g_slow_pool.workers[i], nullptr, worker_thread_main, &g_slow_pool);
    }
    
    LOG_INFO("Initialized async worker pool with %zu threads (%zu fast, %zu slow)", num_workers, fast_workers, slow_workers);
    return 0;
}

void worker_pool_shutdown(void) {
    pthread_mutex_lock(&g_fast_pool.mutex);
    g_fast_pool.shutdown = true;
    pthread_cond_broadcast(&g_fast_pool.cond);
    pthread_mutex_unlock(&g_fast_pool.mutex);
    
    pthread_mutex_lock(&g_slow_pool.mutex);
    g_slow_pool.shutdown = true;
    pthread_cond_broadcast(&g_slow_pool.cond);
    pthread_mutex_unlock(&g_slow_pool.mutex);
    
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

bool worker_pool_enqueue(http_task_t* task) {
    const middleware_ctx_t* ctx = (const middleware_ctx_t*)task->middleware_ctx;
    pool_t* pool = (ctx && ctx->is_fast) ? &g_fast_pool : &g_slow_pool;
    
    pthread_mutex_lock(&pool->mutex);
    
    size_t max_size = config_get_max_queue_size();
    if (max_size > 0 && pool->size >= max_size) {
        pthread_mutex_unlock(&pool->mutex);
        return false;
    }
    
    task_node_t* node = malloc(sizeof(task_node_t));
    node->task = task;
    node->next = nullptr;
    
    if (pool->tail == nullptr) {
        pool->head = pool->tail = node;
    } else {
        pool->tail->next = node;
        pool->tail = node;
    }
    pool->size++;
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
    return true;
}

size_t worker_pool_get_size(void) {
    return g_total_workers;
}
