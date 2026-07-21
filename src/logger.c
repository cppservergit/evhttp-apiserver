#include "logger.h"
#include "json_util.h"
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

static const char* level_strings[] = {
    "INFO", "WARN", "AUDIT", "ERROR", "FATAL", "DEBUG"
};

static _Thread_local char tl_logger_tid[32] = {0};
static _Thread_local char tl_request_id[128] = {0};

void logger_set_request_id(const char* req_id) {
    if (req_id) {
        strncpy(tl_request_id, req_id, sizeof(tl_request_id) - 1);
        tl_request_id[sizeof(tl_request_id) - 1] = '\0';
    } else {
        tl_request_id[0] = '\0';
    }
}

void logger_clear_request_id(void) {
    tl_request_id[0] = '\0';
}



void logger_log(LogLevel level, const char* format, ...) {
    if (tl_logger_tid[0] == '\0') {
        snprintf(tl_logger_tid, sizeof(tl_logger_tid), "0x%lx", pthread_self());
    }

    char msg_buf[2048];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(msg_buf, sizeof(msg_buf), format, args);
    va_end(args);
    
    if (written < 0 || written >= (int)sizeof(msg_buf)) {
        // Handle truncation explicitly
        const char trunc_msg[] = "... [TRUNCATED]";
        size_t trunc_len = sizeof(trunc_msg) - 1;
        if (sizeof(msg_buf) > trunc_len) {
            memcpy(msg_buf + sizeof(msg_buf) - trunc_len - 1, trunc_msg, trunc_len + 1);
        }
    }

    // Worst-case JSON escape expansion is 6x (\uXXXX for every char). 
    // msg_buf is 2048 bytes, so we need 2048 * 6 = 12288 bytes.
    static _Thread_local char escaped_msg[12288];
    json_encode_string(msg_buf, escaped_msg, sizeof(escaped_msg));

    static _Thread_local char out_buf[12800];
    int len;
    if (tl_request_id[0] != '\0') {
        len = snprintf(out_buf, sizeof(out_buf), 
            "{\"level\":\"%s\",\"reqID\":\"%s\",\"msg\":\"%s\",\"threadID\":\"%s\"}\n", 
            level_strings[level], tl_request_id, escaped_msg, tl_logger_tid);
    } else {
        len = snprintf(out_buf, sizeof(out_buf), 
            "{\"level\":\"%s\",\"msg\":\"%s\",\"threadID\":\"%s\"}\n", 
            level_strings[level], escaped_msg, tl_logger_tid);
    }
    
    if (len > 0) {
        int to_write = len < (int)sizeof(out_buf) ? len : (int)sizeof(out_buf) - 1;
        // Atomic single syscall to stderr prevents interleaved outputs between threads
        if (write(STDERR_FILENO, out_buf, (size_t)to_write)) {}
    }
    
    if (level == LOG_LEVEL_FATAL) {
        exit(EXIT_FAILURE);
    }
}
