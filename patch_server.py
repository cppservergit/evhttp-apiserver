import sys

with open("src/server.c", "r") as f:
    content = f.read()

# 1. Remove extract_json_body
start_idx = content.find("static bool extract_json_body")
end_idx = content.find("void server_notify_task_done")
if start_idx != -1 and end_idx != -1:
    content = content[:start_idx] + content[end_idx:]

# 2. Update request_on_complete_cb
old_cb = """static void request_on_complete_cb(struct evhttp_request *req, void *arg) {
    (void)req;
    http_task_t* task = (http_task_t*)arg;
    atomic_store(&task->cancelled, true);
    task->req = nullptr;
}"""
new_cb = """static void request_on_complete_cb(struct evhttp_request *req, void *arg) {
    (void)req;
    http_task_t* task = (http_task_t*)arg;
    atomic_store(&task->cancelled, true);
}"""
content = content.replace(old_cb, new_cb)

# 3. Update cleanup_cancelled_task
old_cleanup = """static void cleanup_cancelled_task(http_task_t* task) {
    if (task->parsed_body) json_object_put(task->parsed_body);
    task_pool_free(task);
}"""
new_cleanup = """static void cleanup_cancelled_task(http_task_t* task) {
    if (task->parsed_body) json_object_put(task->parsed_body);
    if (task->req) evhttp_request_free(task->req);
    task_pool_free(task);
}"""
content = content.replace(old_cleanup, new_cleanup)

# 4. Update process_completed_task
old_proc_end = """    if (task->parsed_body) json_object_put(task->parsed_body);
    logger_clear_request_id();
    task_pool_free(task);
}"""
new_proc_end = """    if (task->parsed_body) json_object_put(task->parsed_body);
    logger_clear_request_id();
    if (task->req) evhttp_request_free(task->req);
    task_pool_free(task);
}"""
content = content.replace(old_proc_end, new_proc_end)

# 5. Update api_middleware_wrapper
# Find the start of the heavy logic
target = """    char username[33] = {0};
    char session_id[37] = {0};"""
    
start_api = content.find(target)
end_api = content.find("}\n\nstatic struct event_base* create_optimized_event_base(void)", start_api)

new_api = """    if (ctx->auth_mode == AUTH_API_KEY) {
        if (!validate_telemetry_api_key(req)) {
            struct evbuffer* out_buf = evhttp_request_get_output_buffer(req);
            const char* msg = "{\\"error\\":\\"Access Denied\\"}";
            evbuffer_add(out_buf, msg, strlen(msg));
            evhttp_send_reply(req, 403, "Forbidden", nullptr);
            return;
        }
    }
    
    http_task_t* task = task_pool_alloc();
    if (!task) {
        struct evbuffer* out_buf = evhttp_request_get_output_buffer(req);
        const char* msg = "{\\"error\\":\\"Server Too Busy\\"}";
        evbuffer_add(out_buf, msg, strlen(msg));
        evhttp_send_reply(req, HTTP_SERVUNAVAIL, "Service Unavailable", nullptr);
        return;
    }
    
    task->req = req;
    task->parsed_body = nullptr;
    task->middleware_ctx = ctx;
    task->start_time = start_time;
    task->reactor_id = tl_reactor_id;
    task->username[0] = '\\0';
    task->session_id[0] = '\\0';
    snprintf(task->client_ip, sizeof(task->client_ip), "%s", extracted_client_ip ? extracted_client_ip : "unknown");
    snprintf(task->uri, sizeof(task->uri), "%s", evhttp_request_get_uri(req));
    
    const char* req_id = evhttp_find_header(in_headers, "X-Request-Id");
    snprintf(task->request_id, sizeof(task->request_id), "%s", req_id ? req_id : "");
    
    atomic_store_explicit(&task->cancelled, false, memory_order_release);
    
    evhttp_request_set_on_complete_cb(req, request_on_complete_cb, task);
    
    // PHASE 1: Take ownership to prevent libevent from freeing it on disconnect
    evhttp_request_own(req);
    
    if (!worker_pool_enqueue(task)) {
        evhttp_request_set_on_complete_cb(req, nullptr, nullptr);
        struct evbuffer* out_buf = evhttp_request_get_output_buffer(req);
        const char* msg = "{\\"error\\":\\"Server Too Busy\\"}";
        evbuffer_add(out_buf, msg, strlen(msg));
        evhttp_send_reply(req, HTTP_SERVUNAVAIL, "Service Unavailable", nullptr);
        evhttp_request_free(req); // Free owned request
        task_pool_free(task);
        return;
    }
"""
content = content[:start_api] + new_api + content[end_api:]

with open("src/server.c", "w") as f:
    f.write(content)
