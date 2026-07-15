#pragma once

#include <event2/http.h>
#include <json-c/json.h>
#include <stdatomic.h>
#include <time.h>
#include <stdbool.h>

/**
 * \\file worker_pool.h
 * \\brief Asynchronous worker thread pool and task queue.
 */

typedef struct {
    struct evhttp_request* req;
    struct json_object* parsed_body;
    const void* middleware_ctx;
    struct timespec start_time;
    size_t reactor_id;
    _Atomic bool cancelled;
    
    // Output fields
    struct json_object* response_json;
    struct evbuffer* response_text;
    int status_code;
    const char* status_txt;
} http_task_t;

/** \brief Initializes the worker thread pools, splitting them into fast and slow pools (Bulkheading). */
int worker_pool_init(size_t num_workers);

/** \brief Gracefully shuts down the worker pool. */
void worker_pool_shutdown(void);

/** \brief Retrieves the number of worker threads currently running. */
size_t worker_pool_get_size(void);

/** \brief Enqueues a task for a worker to process. It routes to the fast or slow pool automatically based on middleware context. Returns false if the target queue is full. */
bool worker_pool_enqueue(http_task_t* task);
