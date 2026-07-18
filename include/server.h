#pragma once

#include <stddef.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <event2/http.h>
#include <sql.h>
#include <sqlext.h>
#include "validation.h"

/**
 * \file server.h
 * \brief Core libevent HTTP server lifecycle and statistics manager.
 */

/** \brief Retrieves the global ODBC environment handle. \return SQLHENV handle. */
SQLHENV server_get_odbc_env(void);

constexpr int SERVER_PORT = 8080;
constexpr char SERVER_ADDR[] = "0.0.0.0";
#define MAX_ERR_MSG_LEN 256

/** \brief Initializes the global server state and ODBC environment. */
int server_init_globals(size_t num_reactors);

/** \brief Instructs all worker threads to safely shut down and breaks the main reactor. */
void server_shutdown_workers(void);

typedef struct json_object* (*json_handler_fn)(struct evhttp_request*, struct json_object*, void*, int*, const char**);
typedef struct evbuffer* (*text_handler_fn)(struct evhttp_request*, struct json_object*, void*, int*, const char**);

typedef struct {
    const char* path;
    enum evhttp_cmd_type allowed_method;
    const ValidationContext* validation_ctx;
    json_handler_fn json_handler;
    text_handler_fn text_handler;
    void* user_arg;
    bool is_fast; /**< \brief If true, routes to the dedicated fast thread pool to prevent starvation (Bulkheading). */
    bool is_secure; /**< \brief If true, requires and validates a JWT token in the Authorization header. */
} middleware_ctx_t;

/** \brief Retrieves the hardcoded server version string. */
const char* get_server_version(void);

/** \brief Notifies the originating reactor that a task is complete. */
void server_notify_task_done(void* task);

/** \brief Records telemetry for a processed HTTP request. \param elapsed_ms Processing time in milliseconds. \param is_fast True if processed in fast pool. */
void server_record_request_stats(long long elapsed_ms, bool is_fast);

typedef struct {
    uint64_t total_requests;
    uint64_t total_requests_fast;
    uint64_t total_requests_slow;
    uint64_t total_time_ms;
    uint64_t total_time_fast_ms;
    uint64_t total_time_slow_ms;
    uint64_t avg_time_fast_ms;
    uint64_t avg_time_slow_ms;
} server_request_stats_t;

/** \brief Retrieves aggregated HTTP request performance metrics. */
void server_get_request_stats(server_request_stats_t* out_stats);

/** \brief Retrieves memory utilization metrics for the current process. */
void server_get_memory_stats(uint64_t* total_ram_kb, uint64_t* mem_usage_kb);

/** \brief Entry point logic for libevent worker threads. */
void* reactor_thread_logic(void* arg);

/** \brief Retrieves the ISO-8601 startup timestamp of the server. */
const char* server_get_start_time(void);

/** \brief Retrieves the server hostname. */
const char* server_get_hostname(void);

/** \brief Retrieves the OS and Kernel version. */
const char* server_get_os_version(void);

/** \brief Helper to create standard JSON error payloads */
struct json_object* create_error_json(const char* error_msg);
