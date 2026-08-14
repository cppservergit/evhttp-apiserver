#include "logger.h"
#include "json_util.h"
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <errno.h>
#include <stdint.h>

#define POOL_SIZE 4096
#define SYNC_BUF_SIZE 4096

typedef struct {
    int len;
    char data[SYNC_BUF_SIZE];
} log_entry_t;

static log_entry_t g_log_buffers[POOL_SIZE];
static log_entry_t* g_free_stack[POOL_SIZE];
static int g_free_top = 0;

static log_entry_t* g_log_queue[POOL_SIZE];
static int g_log_head = 0;
static int g_log_tail = 0;
static int g_log_count = 0;

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_log_cond = PTHREAD_COND_INITIALIZER;
static atomic_bool g_logger_running = false;
static pthread_t g_logger_thread;
static atomic_uint_fast64_t g_dropped_logs = 0;
static atomic_bool g_shutdown_done = false;

static void robust_write(int fd, const char* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, buf + total, len - total);
        if (n > 0) {
            total += (size_t)n;
        } else if (n < 0 && errno != EINTR) {
            break;
        }
    }
}

static void* logger_thread_func(void* arg) {
    (void)arg;
    
    #define BATCH_SIZE 128
    log_entry_t* batch[BATCH_SIZE];
    struct iovec iov[BATCH_SIZE];
    
    while (true) {
        pthread_mutex_lock(&g_log_mutex);
        while (g_log_count == 0 && atomic_load(&g_logger_running)) {
            pthread_cond_wait(&g_log_cond, &g_log_mutex);
        }
        
        if (g_log_count == 0 && !atomic_load(&g_logger_running)) {
            pthread_mutex_unlock(&g_log_mutex);
            break;
        }
        
        int batch_count = 0;
        while (g_log_count > 0 && batch_count < BATCH_SIZE) {
            batch[batch_count] = g_log_queue[g_log_tail];
            g_log_tail = (g_log_tail + 1) % POOL_SIZE;
            g_log_count--;
            batch_count++;
        }
        pthread_mutex_unlock(&g_log_mutex);
        
        if (batch_count > 0) {
            size_t total_to_write = 0;
            for (int i = 0; i < batch_count; i++) {
                iov[i].iov_base = batch[i]->data;
                iov[i].iov_len = (size_t)batch[i]->len;
                total_to_write += iov[i].iov_len;
            }
            
            size_t total_written = 0;
            int iov_offset = 0;
            
            while (total_written < total_to_write && iov_offset < batch_count) {
                ssize_t n = writev(STDERR_FILENO, &iov[iov_offset], batch_count - iov_offset);
                if (n > 0) {
                    total_written += (size_t)n;
                    size_t n_left = (size_t)n;
                    while (n_left > 0 && iov_offset < batch_count) {
                        if (n_left >= iov[iov_offset].iov_len) {
                            n_left -= iov[iov_offset].iov_len;
                            iov_offset++;
                        } else {
                            iov[iov_offset].iov_base = (char*)iov[iov_offset].iov_base + n_left;
                            iov[iov_offset].iov_len -= n_left;
                            n_left = 0;
                        }
                    }
                } else if (n < 0 && errno != EINTR) {
                    break;
                }
            }
            
            pthread_mutex_lock(&g_log_mutex);
            for (int i = 0; i < batch_count; i++) {
                if (g_free_top < POOL_SIZE) {
                    g_free_stack[g_free_top++] = batch[i];
                }
            }
            pthread_mutex_unlock(&g_log_mutex);
        }
    }
    return nullptr;
}

static pthread_once_t g_logger_init_once = PTHREAD_ONCE_INIT;

static void logger_init_routine(void) {
    for (int i = 0; i < POOL_SIZE; i++) {
        g_free_stack[i] = &g_log_buffers[i];
    }
    g_free_top = POOL_SIZE;
    atomic_store(&g_logger_running, true);
    if (pthread_create(&g_logger_thread, nullptr, logger_thread_func, nullptr) != 0) {
        atomic_store(&g_logger_running, false);
        const char* err = "{\"level\":\"FATAL\",\"msg\":\"Failed to create logger thread\"}\n";
        robust_write(STDERR_FILENO, err, strlen(err));
    }
}

void logger_init(void) {
    pthread_once(&g_logger_init_once, logger_init_routine);
}

void logger_shutdown(void) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&g_shutdown_done, &expected, true)) {
        if (atomic_load(&g_logger_running)) {
            pthread_mutex_lock(&g_log_mutex);
            atomic_store(&g_logger_running, false);
            pthread_cond_broadcast(&g_log_cond);
            pthread_mutex_unlock(&g_log_mutex);
            pthread_join(g_logger_thread, nullptr);
        }
    }
}

static const char* level_strings[] = {
    "INFO", "WARN", "AUDIT", "ERROR", "FATAL"
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

void logger_log(LogLevel level, const char* format, ...) {
    if (tl_logger_tid[0] == '\0') {
        pid_t tid = (pid_t)syscall(SYS_gettid);
        (void)snprintf(tl_logger_tid, sizeof(tl_logger_tid), "%d", tid);
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

    static _Thread_local char sync_buf[SYNC_BUF_SIZE];
    int len;
    
    if (tl_request_id[0] != '\0') {
        char escaped_req_id[256];
        json_encode_string(tl_request_id, escaped_req_id, sizeof(escaped_req_id));
        len = snprintf(sync_buf, SYNC_BUF_SIZE, 
            "{\"level\":\"%s\",\"reqID\":\"%s\",\"msg\":\"%s\",\"threadID\":\"%s\"}\n", 
            level_strings[level], escaped_req_id, escaped_msg, tl_logger_tid);
    } else {
        len = snprintf(sync_buf, SYNC_BUF_SIZE, 
            "{\"level\":\"%s\",\"msg\":\"%s\",\"threadID\":\"%s\"}\n", 
            level_strings[level], escaped_msg, tl_logger_tid);
    }
    
    int to_write = 0;
    if (len > 0) {
        to_write = len < SYNC_BUF_SIZE ? len : SYNC_BUF_SIZE - 1;
    }
    
    if (to_write > 0) {
        bool enqueued = false;
        
        if (atomic_load(&g_logger_running)) {
            pthread_mutex_lock(&g_log_mutex);
            if (g_free_top > 0 && g_log_count < POOL_SIZE) {
                log_entry_t* entry = g_free_stack[--g_free_top];
                memcpy(entry->data, sync_buf, (size_t)to_write);
                entry->len = to_write;
                
                g_log_queue[g_log_head] = entry;
                g_log_head = (g_log_head + 1) % POOL_SIZE;
                g_log_count++;
                
                if (g_log_count == 1) {
                    pthread_cond_signal(&g_log_cond);
                }
                enqueued = true;
            }
            pthread_mutex_unlock(&g_log_mutex);
        }
        
        if (!enqueued) {
            if (level == LOG_LEVEL_FATAL || level == LOG_LEVEL_ERROR || level == LOG_LEVEL_WARN || level == LOG_LEVEL_AUDIT) {
                robust_write(STDERR_FILENO, sync_buf, (size_t)to_write);
            } else {
                uint64_t drops = atomic_fetch_add_explicit(&g_dropped_logs, 1, memory_order_relaxed) + 1;
                if ((drops % 100) == 0) {
                    char drop_msg[128];
                    int dlen = snprintf(drop_msg, sizeof(drop_msg), "{\"level\":\"WARN\",\"msg\":\"Logger dropping messages. Total dropped: %lu\"}\n", (unsigned long)drops);
                    robust_write(STDERR_FILENO, drop_msg, (size_t)dlen);
                }
            }
        }
    }
    
    if (level == LOG_LEVEL_FATAL) {
        logger_shutdown();
        exit(EXIT_FAILURE);
    }
}
