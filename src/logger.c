#include "logger.h"
#include "json_util.h"
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#define POOL_SIZE 4096
#define RING_SIZE (POOL_SIZE + 1)

typedef struct {
    int len;
    char data[12800];
} log_entry_t;

static log_entry_t g_log_buffers[POOL_SIZE];

static log_entry_t* g_free_stack[POOL_SIZE];
static int g_free_top = 0;
static pthread_mutex_t g_free_mutex = PTHREAD_MUTEX_INITIALIZER;

static log_entry_t* g_log_queue[RING_SIZE];
static int g_log_head = 0;
static int g_log_tail = 0;

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_log_cond = PTHREAD_COND_INITIALIZER;
static bool g_logger_running = false;
static pthread_t g_logger_thread;

static void* logger_thread_func(void* arg) {
    (void)arg;
    while (true) {
        pthread_mutex_lock(&g_log_mutex);
        while (g_log_head == g_log_tail && g_logger_running) {
            pthread_cond_wait(&g_log_cond, &g_log_mutex);
        }
        
        if (g_log_head == g_log_tail && !g_logger_running) {
            pthread_mutex_unlock(&g_log_mutex);
            break;
        }
        
        int tail = g_log_tail;
        log_entry_t* entry = g_log_queue[tail];
        g_log_tail = (tail + 1) % RING_SIZE;
        pthread_mutex_unlock(&g_log_mutex);
        
        if (write(STDERR_FILENO, entry->data, entry->len)) {}
        
        // Return pointer to free pool
        pthread_mutex_lock(&g_free_mutex);
        g_free_stack[g_free_top++] = entry;
        pthread_mutex_unlock(&g_free_mutex);
    }
    return nullptr;
}

void logger_init(void) {
    if (!g_logger_running) {
        for (int i = 0; i < POOL_SIZE; i++) {
            g_free_stack[i] = &g_log_buffers[i];
        }
        g_free_top = POOL_SIZE;
        g_logger_running = true;
        pthread_create(&g_logger_thread, nullptr, logger_thread_func, nullptr);
    }
}

void logger_shutdown(void) {
    if (g_logger_running) {
        pthread_mutex_lock(&g_log_mutex);
        g_logger_running = false;
        pthread_cond_broadcast(&g_log_cond);
        pthread_mutex_unlock(&g_log_mutex);
        pthread_join(g_logger_thread, nullptr);
    }
}

static const char* level_strings[] = {
    "INFO", "WARN", "AUDIT", "ERROR", "FATAL", "DEBUG"
};

static _Thread_local char tl_logger_tid[32] = {0};
static _Thread_local char tl_request_id[128] = {0};

void logger_set_request_id(const char* req_id) {
    if (req_id) {
        (void)snprintf(tl_request_id, sizeof(tl_request_id), "%s", req_id);
    } else {
        tl_request_id[0] = '\0';
    }
}

void logger_clear_request_id(void) {
    tl_request_id[0] = '\0';
}



static void logger_format_message(log_entry_t* entry, char* sync_buf, LogLevel level, const char* escaped_msg, int* out_to_write) {
    char* target_buf = entry ? entry->data : sync_buf;
    int target_size = entry ? (int)sizeof(entry->data) : 12800; // sizeof(sync_buf) is 12800

    int len;
    if (tl_request_id[0] != '\0') {
        len = snprintf(target_buf, target_size, 
            "{\"level\":\"%s\",\"reqID\":\"%s\",\"msg\":\"%s\",\"threadID\":\"%s\"}\n", 
            level_strings[level], tl_request_id, escaped_msg, tl_logger_tid);
    } else {
        len = snprintf(target_buf, target_size, 
            "{\"level\":\"%s\",\"msg\":\"%s\",\"threadID\":\"%s\"}\n", 
            level_strings[level], escaped_msg, tl_logger_tid);
    }
    
    if (len > 0) {
        *out_to_write = len < target_size ? len : target_size - 1;
    } else {
        *out_to_write = 0;
    }
}

static bool logger_enqueue_entry(log_entry_t* entry, int to_write) {
    entry->len = to_write;
    pthread_mutex_lock(&g_log_mutex);
    int next_head = (g_log_head + 1) % RING_SIZE;
    if (next_head != g_log_tail) {
        g_log_queue[g_log_head] = entry;
        g_log_head = next_head;
        pthread_cond_signal(&g_log_cond);
        pthread_mutex_unlock(&g_log_mutex);
        return true;
    }
    pthread_mutex_unlock(&g_log_mutex);
    return false;
}

void logger_log(LogLevel level, const char* format, ...) {
    if (tl_logger_tid[0] == '\0') {
        (void)snprintf(tl_logger_tid, sizeof(tl_logger_tid), "0x%lx", pthread_self());
    }

    log_entry_t* entry = nullptr;
    if (g_logger_running) {
        pthread_mutex_lock(&g_free_mutex);
        if (g_free_top > 0) entry = g_free_stack[--g_free_top];
        pthread_mutex_unlock(&g_free_mutex);
    }

    char msg_buf[2048];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(msg_buf, sizeof(msg_buf), format, args);
    va_end(args);
    
    if (written < 0 || written >= (int)sizeof(msg_buf)) {
        const char trunc_msg[] = "... [TRUNCATED]";
        size_t trunc_len = sizeof(trunc_msg) - 1;
        if (sizeof(msg_buf) > trunc_len) {
            memcpy(msg_buf + sizeof(msg_buf) - trunc_len - 1, trunc_msg, trunc_len + 1);
        }
    }

    static _Thread_local char escaped_msg[12288];
    json_encode_string(msg_buf, escaped_msg, sizeof(escaped_msg));

    static _Thread_local char sync_buf[12800];
    int to_write = 0;
    logger_format_message(entry, sync_buf, level, escaped_msg, &to_write);
    
    if (to_write > 0) {
        if (entry && logger_enqueue_entry(entry, to_write)) {
            entry = nullptr;
        } else {
            if (write(STDERR_FILENO, entry ? entry->data : sync_buf, (size_t)to_write)) {}
        }
    }
    
    if (entry) {
        pthread_mutex_lock(&g_free_mutex);
        g_free_stack[g_free_top++] = entry;
        pthread_mutex_unlock(&g_free_mutex);
    }
    
    if (level == LOG_LEVEL_FATAL) exit(EXIT_FAILURE);
}
