#include "task_pool.h"
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

// Slab must never be realloc'd to preserve pointer range checks in task_pool_free.
static http_task_t* g_task_slab = nullptr;
static http_task_t** g_free_stack = nullptr;
static bool* g_is_free_flag = nullptr;
static size_t g_stack_top = 0;
static size_t g_pool_size = 0;
static pthread_mutex_t g_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

void task_pool_init(size_t pool_size) {
    if (pool_size == 0) pool_size = 100000;
    g_pool_size = pool_size;
    g_task_slab = calloc(pool_size, sizeof(http_task_t));
    g_free_stack = malloc(pool_size * sizeof(http_task_t*));
    g_is_free_flag = calloc(pool_size, sizeof(bool));
    
    for (size_t i = 0; i < pool_size; i++) {
        g_free_stack[i] = &g_task_slab[i];
        g_is_free_flag[i] = true;
    }
    g_stack_top = pool_size;
}

void task_pool_shutdown(void) {
    free(g_free_stack);
    free(g_task_slab);
    free(g_is_free_flag);
    g_free_stack = nullptr;
    g_task_slab = nullptr;
    g_is_free_flag = nullptr;
}

http_task_t* task_pool_alloc(void) {
    pthread_mutex_lock(&g_pool_mutex);
    if (g_stack_top > 0) {
        http_task_t* task = g_free_stack[--g_stack_top];
        size_t idx = task - g_task_slab;
        g_is_free_flag[idx] = false;
        pthread_mutex_unlock(&g_pool_mutex);
        
        memset(task, 0, sizeof(http_task_t));
        return task;
    }
    pthread_mutex_unlock(&g_pool_mutex);
    
    // Fallback allocation if pool is exhausted
    return calloc(1, sizeof(http_task_t));
}

void task_pool_free(http_task_t* task) {
    if (!task) return;
    
    if (task >= g_task_slab && task < (g_task_slab + g_pool_size)) {
        pthread_mutex_lock(&g_pool_mutex);
        size_t idx = task - g_task_slab;
        if (g_is_free_flag[idx]) {
            fprintf(stderr, "FATAL: Double free detected in task_pool (idx %zu)\n", idx);
            abort();
        }
        if (g_stack_top >= g_pool_size) {
            fprintf(stderr, "FATAL: task_pool stack overflow\n");
            abort();
        }
        g_is_free_flag[idx] = true;
        g_free_stack[g_stack_top++] = task;
        pthread_mutex_unlock(&g_pool_mutex);
    } else {
        free(task);
    }
}
