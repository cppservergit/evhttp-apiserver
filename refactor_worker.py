import os

def rewrite_worker():
    with open("src/worker_pool.c", "r") as f:
        s = f.read()

    # 1. Add #include "thread_error.h"
    if '#include "thread_error.h"' not in s:
        s = s.replace('#include "raii.h"', '#include "raii.h"\n#include "thread_error.h"')

    # 2. Add get_http_status_text function
    status_func = """
static const char* get_http_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "Unknown Status";
    }
}
"""
    if 'get_http_status_text' not in s:
        s = s.replace('static pool_t g_slow_pool = {0};\n', 'static pool_t g_slow_pool = {0};\n' + status_func)

    # 3. Fix handler call and add logging logic
    target_block = """            if (is_authorized && body_ok && valid) {
                if (ctx && ctx->handler) {
                    ctx->handler(task->parsed_body, ctx->user_arg, &task->status_code, &task->status_txt, task->worker_buf);
                    const char* ctype = get_content_type();
                    if (ctype) {
                        snprintf(task->out_content_type, sizeof(task->out_content_type), "%s", ctype);
                    } else {
                        task->out_content_type[0] = '\\0';
                    }
                }
            }
            
            logger_clear_request_id();
            handlers_clear_context();
        }
        
        // Notify reactor"""

    replacement_block = """            if (is_authorized && body_ok && valid) {
                if (ctx && ctx->handler) {
                    ctx->handler(task->parsed_body, ctx->user_arg, &task->status_code, task->worker_buf);
                    const char* ctype = get_content_type();
                    if (ctype) {
                        snprintf(task->out_content_type, sizeof(task->out_content_type), "%s", ctype);
                    } else {
                        task->out_content_type[0] = '\\0';
                    }
                }
            }
            
            task->status_txt = get_http_status_text(task->status_code);

            ThreadErrorLevel err_lvl = get_thread_error_level();
            if (err_lvl == TL_ERR_ERROR) {
                LOG_ERROR("Request failed for URI %s: %s", task->uri, get_thread_error_msg());
            } else if (err_lvl == TL_ERR_WARN) {
                LOG_WARN("Request warning for URI %s: %s", task->uri, get_thread_error_msg());
            } else if (task->status_code >= 500) {
                LOG_ERROR("Request failed with status %d for URI %s", task->status_code, task->uri);
            }
            clear_thread_error();
            
            logger_clear_request_id();
            handlers_clear_context();
        }
        
        // Notify reactor"""

    s = s.replace(target_block, replacement_block)
    
    with open("src/worker_pool.c", "w") as f:
        f.write(s)

if __name__ == "__main__":
    rewrite_worker()
