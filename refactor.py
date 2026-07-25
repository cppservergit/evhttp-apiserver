import os
import re

def rewrite():
    # 1. include/thread_error.h
    with open("include/thread_error.h", "w") as f:
        f.write('''#pragma once

typedef enum {
    TL_ERR_NONE = 0,
    TL_ERR_WARN,
    TL_ERR_ERROR
} ThreadErrorLevel;

void set_thread_error(ThreadErrorLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));
const char* get_thread_error_msg(void);
ThreadErrorLevel get_thread_error_level(void);
void clear_thread_error(void);
''')

    # 2. src/thread_error.c
    with open("src/thread_error.c", "w") as f:
        f.write('''#include "thread_error.h"
#include <stdio.h>
#include <stdarg.h>

#define MAX_ERR_MSG_LEN 512

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
    tl_err_msg[0] = '\\0';
}
''')

    # 3. include/server.h
    with open("include/server.h", "r") as f:
        s = f.read()
    s = s.replace("typedef void (*handler_fn)(struct json_object* body, void* user_arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf);", 
                  "typedef void (*handler_fn)(struct json_object* body, void* user_arg, int* out_status, struct evbuffer* out_buf);")
    with open("include/server.h", "w") as f:
        f.write(s)

    # 4. include/handlers.h
    with open("include/handlers.h", "r") as f:
        s = f.read()
    s = s.replace("const char** out_status_txt, ", "")
    s = s.replace("const char** out_status_txt", "")
    with open("include/handlers.h", "w") as f:
        f.write(s)

    # 5. include/totp.h
    with open("include/totp.h", "r") as f:
        s = f.read()
    s = s.replace("const char** out_status_txt, ", "")
    with open("include/totp.h", "w") as f:
        f.write(s)

    # 6. src/totp.c
    with open("src/totp.c", "r") as f:
        s = f.read()
    s = s.replace("#include \"logger.h\"", "#include \"thread_error.h\"")
    s = s.replace("const char** out_status_txt, ", "")
    s = s.replace("*out_status_txt = \"Bad Request\";", "")
    s = s.replace("*out_status_txt = (db_status == HTTP_NOTFOUND) ? \"Not Found\" : \"Internal Server Error\";", "")
    s = s.replace("*out_status_txt = \"Internal Server Error\";", "")
    s = s.replace("*out_status_txt = \"OK\";", "")
    with open("src/totp.c", "w") as f:
        f.write(s)

    # 7. src/handlers.c
    with open("src/handlers.c", "r") as f:
        s = f.read()
    s = s.replace("const char** out_status_txt,", "")
    s = s.replace("const char** out_status_txt", "")
    s = re.sub(r'^\s*\*out_status_txt\s*=\s*[^;]+;\n', '', s, flags=re.MULTILINE)
    s = s.replace("totp_generate_svg(user, out_status, out_status_txt, out_buf);", "totp_generate_svg(user, out_status, out_buf);")
    s = s.replace("handle_login_success(username, remote_ip, out_status, out_buf);", "handle_login_success(username, remote_ip, out_status, out_buf);")
    s = s.replace("handle_login_failure(username, remote_ip, http_code, remote_response, out_status, out_buf);", "handle_login_failure(username, remote_ip, http_code, remote_response, out_status, out_buf);")
    with open("src/handlers.c", "w") as f:
        f.write(s)

    # 8. src/odbcutil.c and include/odbcutil.h
    with open("include/odbcutil.h", "r") as f:
        s = f.read()
    s = s.replace("void odbcutil_log_error(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg);", 
                  "void odbcutil_set_error(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg);")
    with open("include/odbcutil.h", "w") as f:
        f.write(s)

    with open("src/odbcutil.c", "r") as f:
        s = f.read()
    s = s.replace("#include \"logger.h\"", "#include \"thread_error.h\"")
    s = s.replace("void odbcutil_log_error", "void odbcutil_set_error")
    s = s.replace("odbcutil_log_error", "odbcutil_set_error")
    
    s = s.replace("LOG_ERROR(\"%s | ODBC Error [%s]: %s\", context_msg, sqlState, msg);", 
                  "set_thread_error(TL_ERR_ERROR, \"%s | ODBC Error [%s]: %s\", context_msg, sqlState, msg);")
    s = s.replace("LOG_ERROR(\"Database connection lost. Resetting thread-local connection pool.\");\\n", "")
    s = s.replace("LOG_ERROR(\"%s | Unknown ODBC Error\", context_msg);", "set_thread_error(TL_ERR_ERROR, \"%s | Unknown ODBC Error\", context_msg);")
    
    with open("src/odbcutil.c", "w") as f:
        f.write(s)

    # 9. src/http_client.c
    with open("src/http_client.c", "r") as f:
        s = f.read()
    s = s.replace("#include \"logger.h\"", "#include \"thread_error.h\"")
    s = s.replace('LOG_ERROR("Failed to allocate curl handle");', 'set_thread_error(TL_ERR_ERROR, "Failed to allocate curl handle");')
    s = s.replace('LOG_ERROR("HTTP GET failed: %s", curl_easy_strerror(res));', 'set_thread_error(TL_ERR_ERROR, "HTTP GET failed: %s", curl_easy_strerror(res));')
    s = s.replace('LOG_ERROR("HTTP POST failed: %s", curl_easy_strerror(res));', 'set_thread_error(TL_ERR_ERROR, "HTTP POST failed: %s", curl_easy_strerror(res));')
    s = s.replace('LOG_ERROR("JSON parsing failed in HTTP client");', 'set_thread_error(TL_ERR_ERROR, "JSON parsing failed in HTTP client");')
    with open("src/http_client.c", "w") as f:
        f.write(s)

    # 10. src/customer.c
    with open("src/customer.c", "r") as f:
        s = f.read()
    s = s.replace("#include \"logger.h\"", "#include \"thread_error.h\"")
    s = s.replace("LOG_WARN(\"Failed to fetch customer data for ID %s, HTTP Code: %ld\", customer_id, *out_http_code);", 
                  "set_thread_error(TL_ERR_WARN, \"Failed to fetch customer data for ID %s, HTTP Code: %ld\", customer_id, *out_http_code);")
    with open("src/customer.c", "w") as f:
        f.write(s)

if __name__ == "__main__":
    rewrite()
