#pragma once

#include <event2/http.h>
#include <json-c/json.h>
#include <stdatomic.h>
#include <time.h>
#include <stdalign.h>
#include <stdbool.h>

/**
 * \file worker_pool.h
 * \brief Asynchronous worker thread pool and task queue.
 */

/** \brief Represents a single HTTP request task processed by a worker thread. */
typedef struct http_task_s {
    alignas(64) struct timespec start_time;
    struct evhttp_request* req;
    struct json_object* parsed_body;
    const void* middleware_ctx;
    const char* status_txt;
    struct evbuffer* worker_buf;
    struct http_task_s* next;
    size_t reactor_id;
    int status_code;
    _Atomic bool cancelled;
    
    char out_content_type[128];
    char username[33];
    char session_id[37];
    char client_ip[64];
    char request_id[64];
    char uri[1024];
    char _padding[45];
} http_task_t;

/** \brief Initializes the worker thread pools, splitting them into fast and slow pools (Bulkheading). */
int worker_pool_init(size_t num_workers);

/** \brief Gracefully stops the worker pool threads. */
void worker_pool_stop(void);

/** \brief Gracefully shuts down the worker pool and destroys primitives. */
void worker_pool_shutdown(void);

/** \brief Retrieves the number of worker threads currently running. */
size_t worker_pool_get_size(void);

/** \brief Enqueues a task for a worker to process. It routes to the fast or slow pool automatically based on middleware context. Returns false if the target queue is full. */
bool worker_pool_enqueue(http_task_t* task);
