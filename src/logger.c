#include "logger.h"
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <string.h>

static const char* level_strings[] = {
    "INFO", "WARN", "AUDIT", "ERROR", "FATAL", "DEBUG"
};

static _Thread_local char tl_logger_tid[32] = {0};

// Stack-based JSON escaper (Zero Malloc, fully memory safe)
static void escape_json_string(const char* src, char* dest, size_t dest_size) {
    size_t i = 0, j = 0;
    while (src[i] && j < dest_size - 7) { 
        switch (src[i]) {
            case '"':  dest[j++] = '\\'; dest[j++] = '"'; break;
            case '\\': dest[j++] = '\\'; dest[j++] = '\\'; break;
            case '\n': dest[j++] = '\\'; dest[j++] = 'n'; break;
            case '\r': dest[j++] = '\\'; dest[j++] = 'r'; break;
            case '\t': dest[j++] = '\\'; dest[j++] = 't'; break;
            default:
                if ((unsigned char)src[i] < 0x20) {
                    int written = snprintf(&dest[j], dest_size - j, "\\u%04x", (unsigned char)src[i]);
                    if (written > 0 && written < (int)(dest_size - j)) {
                        j += (size_t)written;
                    }
                } else {
                    dest[j++] = src[i];
                }
                break;
        }
        i++;
    }
    dest[j] = '\0';
}

void logger_log(LogLevel level, const char* format, ...) {
    if (tl_logger_tid[0] == '\0') {
        snprintf(tl_logger_tid, sizeof(tl_logger_tid), "0x%lx", pthread_self());
    }

    char msg_buf[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(msg_buf, sizeof(msg_buf), format, args);
    va_end(args);

    char escaped_msg[3072];
    escape_json_string(msg_buf, escaped_msg, sizeof(escaped_msg));

    char out_buf[4096];
    int len = snprintf(out_buf, sizeof(out_buf), 
        "{\"level\":\"%s\",\"msg\":\"%s\",\"threadID\":\"%s\"}\n", 
        level_strings[level], escaped_msg, tl_logger_tid);
    
    if (len > 0) {
        int to_write = len < (int)sizeof(out_buf) ? len : (int)sizeof(out_buf) - 1;
        // Atomic single syscall to stderr prevents interleaved outputs between threads
        if (write(STDERR_FILENO, out_buf, (size_t)to_write)) {}
    }
}
