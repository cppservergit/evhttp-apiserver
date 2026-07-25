#include "thread_error.h"
#include <stdio.h>
#include <stdarg.h>

#define MAX_ERR_MSG_LEN 1024

static _Thread_local ThreadErrorLevel tl_err_level = TL_ERR_NONE;
static _Thread_local char tl_err_msg[MAX_ERR_MSG_LEN] = {0};

void set_thread_error(ThreadErrorLevel level, const char* format, ...) {
    tl_err_level = level;
    va_list args;
    va_start(args, format);
    vsnprintf(tl_err_msg, sizeof(tl_err_msg), format, args);
    va_end(args);
}

const char* get_thread_error_msg(void) {
    return tl_err_msg;
}

ThreadErrorLevel get_thread_error_level(void) {
    return tl_err_level;
}

void clear_thread_error(void) {
    tl_err_level = TL_ERR_NONE;
    tl_err_msg[0] = '\0';
}
