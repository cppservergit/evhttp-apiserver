#include "task_pool.h"
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <errno.h>
#include "logger.h"
#include <event2/buffer.h>

// Slab must never be realloc'd to preserve pointer range checks in task_pool_free.
static http_task_t* g_task_slab = nullptr;
static http_task_t** g_free_stack = nullptr;
static _Atomic bool* g_is_free_flag = nullptr;
static size_t g_stack_top = 0;
static size_t g_pool_size = 0;
static pthread_mutex_t g_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

#define TL_CACHE_SIZE 64
static _Thread_local http_task_t* tl_cache[TL_CACHE_SIZE];
static _Thread_local size_t tl_cache_count = 0;

int task_pool_init(size_t pool_size) {
    if (pool_size == 0) pool_size = 100000;
    g_pool_size = pool_size;
    g_task_slab = calloc(pool_size, sizeof(http_task_t));
    g_free_stack = malloc(pool_size * sizeof(http_task_t*));
    g_is_free_flag = calloc(pool_size, sizeof(_Atomic bool));
    
    if (!g_task_slab || !g_free_stack || !g_is_free_flag) {
        char errbuf[256];
        LOG_FATAL("Out of memory in task_pool_init: %s", strerror_r(errno, errbuf, sizeof(errbuf)));
        if (g_task_slab) { free(g_task_slab); g_task_slab = nullptr; }
        if (g_free_stack) { free(g_free_stack); g_free_stack = nullptr; }
        if (g_is_free_flag) { free(g_is_free_flag); g_is_free_flag = nullptr; }
        return -1;
    }

    for (size_t i = 0; i < pool_size; i++) {
        g_free_stack[i] = &g_task_slab[i];
        atomic_init(&g_is_free_flag[i], true);
    }
    g_stack_top = pool_size;
    return 0;
}

void task_pool_shutdown(void) {
    if (!g_task_slab) {
        free(g_free_stack);
        free(g_is_free_flag);
        g_free_stack = nullptr;
        g_is_free_flag = nullptr;
        return;
    }
    
    for (size_t i = 0; i < g_pool_size; i++) {
        if (g_task_slab[i].worker_buf) {
            evbuffer_free(g_task_slab[i].worker_buf);
        }
    }
    free(g_free_stack);
    free(g_task_slab);
    free(g_is_free_flag);
    g_free_stack = nullptr;
    g_task_slab = nullptr;
    g_is_free_flag = nullptr;
}

http_task_t* task_pool_alloc(void) {
    http_task_t* task = nullptr;
    if (tl_cache_count > 0) {
        task = tl_cache[--tl_cache_count];
    } else {
        pthread_mutex_lock(&g_pool_mutex);
        size_t batch = (g_stack_top < TL_CACHE_SIZE) ? g_stack_top : TL_CACHE_SIZE;
        for (size_t i = 0; i < batch; i++) {
            http_task_t* t = g_free_stack[--g_stack_top];
            size_t idx = (size_t)(t - g_task_slab);
            atomic_store_explicit(&g_is_free_flag[idx], false, memory_order_relaxed);
            tl_cache[tl_cache_count++] = t;
        }
        if (batch > 0) {
            task = tl_cache[--tl_cache_count];
        }
        pthread_mutex_unlock(&g_pool_mutex);
    }
    
    if (!task) return nullptr;
    
    size_t idx = (size_t)(task - g_task_slab);
    atomic_store_explicit(&g_is_free_flag[idx], false, memory_order_relaxed);
    
    struct evbuffer* cached_buf = task->worker_buf;
    memset(task, 0, sizeof(http_task_t));
    if (cached_buf) {
        evbuffer_drain(cached_buf, evbuffer_get_length(cached_buf));
        task->worker_buf = cached_buf;
    } else {
        task->worker_buf = evbuffer_new();
    }
    return task;
}

void task_pool_free(http_task_t* task) {
    if (!task) return;
    
    uintptr_t t_ptr = (uintptr_t)task;
    uintptr_t slab_start = (uintptr_t)g_task_slab;
    uintptr_t slab_end = slab_start + (g_pool_size * sizeof(http_task_t));

    if (t_ptr < slab_start || t_ptr >= slab_end) {
        LOG_ERROR("Out-of-bounds pointer passed to task_pool_free (%p). Ignoring.", (void*)task);
        return;
    }

    size_t idx = (size_t)(task - g_task_slab);
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&g_is_free_flag[idx], &expected, true, memory_order_relaxed, memory_order_relaxed)) {
        (void)fprintf(stderr, "FATAL: Double free detected in task_pool (idx %zu)\n", idx);
        abort();
    }

    if (tl_cache_count < TL_CACHE_SIZE) {
        tl_cache[tl_cache_count++] = task;
        return;
    }
    
    pthread_mutex_lock(&g_pool_mutex);
    size_t flush_count = tl_cache_count;
    if (g_stack_top + flush_count > g_pool_size) {
        (void)fprintf(stderr, "FATAL: task_pool stack overflow\n");
        abort();
    }
    for (size_t i = 0; i < flush_count; i++) {
        g_free_stack[g_stack_top++] = tl_cache[i];
    }
    pthread_mutex_unlock(&g_pool_mutex);
    
    tl_cache_count = 0;
    tl_cache[tl_cache_count++] = task;
}
