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

/** \brief Initializes the global server state and ODBC environment. */
int server_init_globals(size_t num_reactors);

/** \brief Blocks until all reactor threads are fully initialized and registered. */
void server_wait_startup_barrier(void);

/** \brief Returns true if any reactor thread failed to initialize. */
bool server_did_startup_fail(void);

/** \brief Instructs all worker threads to safely shut down and breaks the main reactor. */
void server_shutdown_workers(void);

/** \brief Safely cleans up all global resources deterministically without relying on atexit */
void server_cleanup_globals(void);

/** 
 * \brief Signature for a libevent HTTP request handler. 
 * \param body Parsed JSON request payload (null if GET/no body). Memory is managed by the server.
 * \param user_arg Context pointer injected from RouteConfig (used for dependency injection/reusability).
 * \param out_status Pointer to the HTTP status code (e.g. 200, 404). Handlers must set this.
 * \param out_buf The libevent network buffer where response data should be appended directly.
 */
typedef void (*handler_fn)(struct json_object* body, void* user_arg, int* out_status, struct evbuffer* out_buf);

/** \brief Security enforcement mode for an HTTP route. */
typedef enum {
    AUTH_NONE = 0,
    AUTH_JWT = 1,
    AUTH_API_KEY = 2
} auth_mode_t;

/** \brief Middleware configuration context for an HTTP route. */
typedef struct {
    const char* path;
    const ValidationContext* validation_ctx;
    handler_fn handler;
    void* user_arg;
    enum evhttp_cmd_type allowed_method;
    auth_mode_t auth_mode; /**< \brief Security enforcement mode for this route. */
    bool is_fast; /**< \brief If true, routes to the dedicated fast thread pool to prevent starvation (Bulkheading). */
    char _padding[7];
} middleware_ctx_t;

/** \brief Retrieves the hardcoded server version string. */
const char* get_server_version(void);

/** \brief Notifies the originating reactor that a task is complete. */
void server_notify_task_done(void* task);

/** \brief Request telemetry statistics aggregated by the server. */
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
