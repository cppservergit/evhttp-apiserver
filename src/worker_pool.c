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

static task_node_t* q_head = nullptr;
static task_node_t* q_tail = nullptr;
static pthread_mutex_t q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static bool q_shutdown = false;
static size_t q_size = 0;
static size_t g_num_workers = 0;
static pthread_t* g_workers = nullptr;

static void* worker_thread_main(void* arg) {
    size_t worker_id = (size_t)arg;
    (void)worker_id;
    http_client_init_thread();
    
    while (1) {
        pthread_mutex_lock(&q_mutex);
        while (q_head == nullptr && !q_shutdown) {
            pthread_cond_wait(&q_cond, &q_mutex);
        }
        
        if (q_shutdown && q_head == nullptr) {
            pthread_mutex_unlock(&q_mutex);
            break;
        }
        
        task_node_t* node = q_head;
        q_head = node->next;
        if (q_head == nullptr) q_tail = nullptr;
        q_size--;
        pthread_mutex_unlock(&q_mutex);
        
        http_task_t* task = node->task;
        free(node);
        
        if (!atomic_load(&task->cancelled)) {
            const middleware_ctx_t* ctx = (const middleware_ctx_t*)task->middleware_ctx;
            if (ctx->json_handler) {
                task->response_json = ctx->json_handler(nullptr, task->parsed_body, ctx->user_arg, &task->status_code, &task->status_txt);
            } else if (ctx->text_handler) {
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
    g_num_workers = num_workers;
    g_workers = calloc(num_workers, sizeof(pthread_t));
    for (size_t i = 0; i < num_workers; ++i) {
        pthread_create(&g_workers[i], nullptr, worker_thread_main, (void*)i);
    }
    LOG_INFO("Initialized async worker pool with %zu threads", num_workers);
    return 0;
}

void worker_pool_shutdown(void) {
    pthread_mutex_lock(&q_mutex);
    q_shutdown = true;
    pthread_cond_broadcast(&q_cond);
    pthread_mutex_unlock(&q_mutex);
    
    if (g_workers) {
        for (size_t i = 0; i < g_num_workers; ++i) {
            pthread_join(g_workers[i], nullptr);
        }
        free(g_workers);
        g_workers = nullptr;
    }
}

bool worker_pool_enqueue(http_task_t* task) {
    pthread_mutex_lock(&q_mutex);
    
    size_t max_size = config_get_max_queue_size();
    if (max_size > 0 && q_size >= max_size) {
        pthread_mutex_unlock(&q_mutex);
        return false;
    }
    
    task_node_t* node = malloc(sizeof(task_node_t));
    node->task = task;
    node->next = nullptr;
    
    if (q_tail == nullptr) {
        q_head = q_tail = node;
    } else {
        q_tail->next = node;
        q_tail = node;
    }
    q_size++;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_mutex);
    return true;
}

size_t worker_pool_get_size(void) {
    return g_num_workers;
}
