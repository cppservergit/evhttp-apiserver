#pragma once

typedef enum {
    TL_ERR_NONE = 0,
    TL_ERR_WARN,
    TL_ERR_ERROR
} ThreadErrorLevel;

void set_thread_error(ThreadErrorLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));
const char* get_thread_error_msg(void);
ThreadErrorLevel get_thread_error_level(void);
void clear_thread_error(void);
