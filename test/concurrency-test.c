#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>
#include "worker_pool.h"
#include "task_pool.h"
#include "config.h"
#include "server.h"
#include "handlers.h"
#include "jwt.h"

#define NUM_PRODUCERS 8
#define TASKS_PER_PRODUCER 2000

void dummy_cb(struct evhttp_request *req, void *arg) {
    (void)req;
    (void)arg;
}

void dummy_handler(struct json_object *body, void *arg, int *status_code, struct evbuffer *out_buf) {
    (void)body; (void)arg; (void)status_code; (void)out_buf;
    char buf[1024];
    config_get_odbc_conn_str(buf, sizeof(buf));
    config_get_api_url(buf, sizeof(buf));
    config_get_api_user(buf, sizeof(buf));
    config_get_api_pass(buf, sizeof(buf));
    config_get_jwt_secret(buf, sizeof(buf));
    config_get_login_provider(buf, sizeof(buf));
    config_get_login_uri(buf, sizeof(buf));
    config_is_origin_allowed("http://example.com");
}

static middleware_ctx_t mock_ctx = {
    .is_fast = true,
    .auth_mode = AUTH_NONE,
    .handler = dummy_handler
};

void* producer_thread(void* arg) {
    (void)arg;
    for (int i = 0; i < TASKS_PER_PRODUCER; ++i) {
        http_task_t* task = task_pool_alloc();
        if (!task) {
            sched_yield();
            i--;
            continue;
        }
        
        struct evhttp_request* req = evhttp_request_new(dummy_cb, nullptr);
        
        task->req = req;
        task->middleware_ctx = &mock_ctx;
        
        while (!worker_pool_enqueue(task)) {
            sched_yield();
        }
    }
    return nullptr;
}

void* reloader_thread(void* arg) {
    (void)arg;
    for (int i = 0; i < 20; ++i) {
        usleep(10000); // 10ms
        config_reload();
    }
    return nullptr;
}

void server_notify_task_done(void* t) {
    http_task_t* task = (http_task_t*)t;
    if (task->req) {
        evhttp_request_free(task->req);
    }
    task_pool_free(task);
}

void handlers_set_context(const char* username, const char* session_id, const char* client_ip, const char* uri) { (void)username; (void)session_id; (void)client_ip; (void)uri; }
void handlers_clear_context(void) {}
const char* get_content_type(void) { return nullptr; }
void http_client_init_thread(void) {}
void http_client_cleanup_thread(void) {}
int jwt_verify(const char* token, const char* secret_hex, char* out_username, size_t out_uname_size, char* out_session_id, size_t out_sess_size) {
    (void)token; (void)secret_hex; (void)out_username; (void)out_uname_size; (void)out_session_id; (void)out_sess_size;
    return JWT_OK;
}

int main() {
    printf("Starting TSAN concurrency test...\n");
    
    setenv("FAST_POOL_PERCENTAGE", "50", 1);
    setenv("MAX_QUEUE_SIZE", "100000", 1);
    setenv("ODBC_CONN_STR", "mock", 1);
    setenv("API_URL", "mock", 1);
    setenv("API_USER", "mock", 1);
    setenv("API_PASS", "mock", 1);
    
    config_init();
    task_pool_init(20000);
    worker_pool_init(4);
    
    pthread_t producers[NUM_PRODUCERS];
    pthread_t reloader;
    
    pthread_create(&reloader, nullptr, reloader_thread, nullptr);
    
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        pthread_create(&producers[i], nullptr, producer_thread, (void*)(intptr_t)i);
    }
    
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        pthread_join(producers[i], nullptr);
    }
    pthread_join(reloader, nullptr);
    
    worker_pool_shutdown();
    task_pool_shutdown();
    
    printf("Concurrency test completed successfully.\n");
    return 0;
}
