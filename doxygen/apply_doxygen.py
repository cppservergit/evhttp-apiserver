import os

files = {
    "include/config.h": """#pragma once
#include <stddef.h>
#include <stdbool.h>

/**
 * \\file config.h
 * \\brief Application configuration and hot-reloading subsystem.
 */

/** \\brief Maximum length for a configuration string value. */
#define MAX_CONFIG_STR 1024

/** \\brief Initializes the configuration subsystem. */
void config_init(void);

/** \\brief Reloads configuration from the environment/file dynamically (e.g., on SIGHUP). */
void config_reload(void);

/** \\brief Checks if access logging is enabled. \\return true if enabled, false otherwise. */
bool config_get_access_log(void);

/** 
 * \\brief Safely retrieves the ODBC connection string.
 * \\param out Buffer to copy the string into.
 * \\param max_len Size of the buffer.
 */
void config_get_odbc_conn_str(char* out, size_t max_len);

/** 
 * \\brief Safely retrieves the remote API URL.
 * \\param out Buffer to copy the string into.
 * \\param max_len Size of the buffer.
 */
void config_get_api_url(char* out, size_t max_len);

/** 
 * \\brief Safely retrieves the remote API username.
 * \\param out Buffer to copy the string into.
 * \\param max_len Size of the buffer.
 */
void config_get_api_user(char* out, size_t max_len);

/** 
 * \\brief Safely retrieves the remote API password.
 * \\param out Buffer to copy the string into.
 * \\param max_len Size of the buffer.
 */
void config_get_api_pass(char* out, size_t max_len);
""",

    "include/customer.h": """#pragma once
#include <json-c/json.h>

/**
 * \\file customer.h
 * \\brief External API integration for customer data.
 */

/** 
 * \\brief Fetches customer info from the remote REST API.
 * \\param customer_id The ID of the customer.
 * \\param out_http_code Pointer to store the returned HTTP status code.
 * \\return A newly allocated json_object (caller must free), or NULL on failure.
 */
struct json_object* customer_service_get_info(const char* customer_id, long* out_http_code);
""",

    "include/customerdb.h": """#pragma once
#include <json-c/json.h>

/**
 * \\file customerdb.h
 * \\brief Database integration for customer data.
 */

/**
 * \\brief Retrieves customer data from the database using a stored procedure.
 * \\param customer_id The customer identifier.
 * \\return A newly allocated json_object array of results (caller must free).
 */
struct json_object* customerdb_get_data(const char* customer_id);
""",

    "include/handlers.h": """#pragma once
#include <json-c/json.h>
#include <event2/http.h>
#include "validation.h"

/**
 * \\file handlers.h
 * \\brief HTTP request handlers for the libevent web server.
 */

extern const ValidationContext CustomerContext;
extern const ValidationContext SalesContext;

/** \\brief Handles /ping requests for liveness checks. */
struct json_object* ping_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \\brief Handles /version requests. */
struct json_object* version_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \\brief Handles /sysinfo requests. */
struct json_object* sysinfo_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \\brief Handles /rsysinfo requests. */
struct json_object* rsysinfo_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \\brief Handles /customer requests (external REST integration). */
struct json_object* customer_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \\brief Handles /customer_get requests (database integration). */
struct json_object* customer_get_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \\brief Handles /sales requests. */
struct json_object* sales_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \\brief Handles /shippers requests. */
struct json_object* shippers_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \\brief Handles /products requests. */
struct json_object* products_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);

/** \\brief Handles /metrics requests. Returns plain text Prometheus format instead of JSON. */
struct evbuffer* metrics_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt);
""",

    "include/http_client.h": """#pragma once
#include <json-c/json.h>

/**
 * \\file http_client.h
 * \\brief Libcurl wrapper for HTTP client requests.
 */

/** \\brief Initializes the thread-local libcurl handle (must be called once per thread). */
void http_client_init_thread(void);

/** \\brief Cleans up the thread-local libcurl handle. */
void http_client_cleanup_thread(void);

/** 
 * \\brief Performs an HTTP GET request and parses the response as JSON.
 * \\param url The destination URL.
 * \\param headers Array of HTTP headers, or NULL.
 * \\param num_headers Number of headers in the array.
 * \\param out_http_code Pointer to store the HTTP status code.
 * \\return A newly allocated json_object, or NULL on failure.
 */
struct json_object* http_client_get_json(const char* url, const char** headers, int num_headers, long* out_http_code);

/** 
 * \\brief Performs an HTTP POST request and parses the response as JSON.
 * \\param url The destination URL.
 * \\param body The POST payload body.
 * \\param headers Array of HTTP headers, or NULL.
 * \\param num_headers Number of headers in the array.
 * \\param out_http_code Pointer to store the HTTP status code.
 * \\return A newly allocated json_object, or NULL on failure.
 */
struct json_object* http_client_post_json(const char* url, const char* body, const char** headers, int num_headers, long* out_http_code);
""",

    "include/jwt.h": """#pragma once
#include <time.h>

/**
 * \\file jwt.h
 * \\brief JSON Web Token (JWT) decoding utilities.
 */

/** 
 * \\brief Decodes the base64 payload from a JWT token.
 * \\param jwt The raw JWT string.
 * \\return A malloc'd string containing the payload (must be freed), or NULL on failure.
 */
char* jwt_decode_payload(const char* jwt);

/** 
 * \\brief Extracts the 'exp' (expiration) claim from a JWT token.
 * \\param jwt The raw JWT string.
 * \\return The expiration epoch time, or 0 if missing/invalid.
 */
time_t jwt_get_expiration(const char* jwt);
""",

    "include/logger.h": """#pragma once

#include <stdio.h>
#include <stdarg.h>

/**
 * \\file logger.h
 * \\brief Thread-safe JSON structured logging framework.
 */

/** \\brief Available severity levels for structured logging. */
typedef enum {
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_AUDIT,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
    LOG_LEVEL_DEBUG
} LogLevel;

#define LOG_INFO(...)  logger_log(LOG_LEVEL_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  logger_log(LOG_LEVEL_WARN,  __VA_ARGS__)
#define LOG_AUDIT(...) logger_log(LOG_LEVEL_AUDIT, __VA_ARGS__)
#define LOG_ERROR(...) logger_log(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_FATAL(...) logger_log(LOG_LEVEL_FATAL, __VA_ARGS__)
#define LOG_DEBUG(...) logger_log(LOG_LEVEL_DEBUG, __VA_ARGS__)

/**
 * \\brief Writes a structured JSON log entry to stderr.
 * \\param level The severity level.
 * \\param format printf-style format string.
 */
__attribute__((format(printf, 2, 3)))
void logger_log(LogLevel level, const char* format, ...);
""",

    "include/odbcutil.h": """#pragma once

#include <sql.h>
#include <sqlext.h>
#include <json-c/json.h>

/**
 * \\file odbcutil.h
 * \\brief ODBC database utility wrappers and abstractions.
 */

#define MAX_ODBC_CONN_STR_LEN 1024
#define ODBC_FETCH_CHUNK_SIZE 8192

/** \\brief Retrieves a new database connection from the driver pool. \\return The ODBC connection handle. */
SQLHDBC odbcutil_connect(void);

/** \\brief Extracts JSON data natively from an executed SQL statement. \\param hstmt The executed statement. \\return A newly allocated json_object array. */
struct json_object* odbcutil_fetch_json(SQLHSTMT hstmt);

/** \\brief Extracts and logs ODBC diagnostic records. */
void odbcutil_log_error(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg);

/** \\brief Allocates a statement handle and handles cleanup/logging on failure. */
SQLHSTMT odbcutil_alloc_stmt(SQLHDBC hdbc, const char* func_name);

/** \\brief Frees the statement and disconnects/frees the database connection. */
void odbcutil_disconnect(SQLHDBC hdbc, SQLHSTMT hstmt);

/** \\brief Encapsulates the entire connect, execute, fetch, and disconnect flow for parameter-less queries. */
struct json_object* odbcutil_get_json(const char* sp_call, const char* func_name);
""",

    "include/products.h": """#pragma once
#include <json-c/json.h>

/**
 * \\file products.h
 * \\brief Database integration for product catalog data.
 */

/** \\brief Retrieves a list of products from the database. \\return A json_object array (caller frees). */
struct json_object* products_get_data(void);
""",

    "include/raii.h": """#pragma once

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <json-c/json.h>
#include <stdlib.h>

/**
 * \\file raii.h
 * \\brief Resource Acquisition Is Initialization (RAII) memory cleanup macros using GCC's __attribute__((cleanup)).
 */

static inline void cleanup_event_config(struct event_config** cfg) { if (*cfg != nullptr) event_config_free(*cfg); }
static inline void cleanup_event_base(struct event_base** base) { if (*base != nullptr) event_base_free(*base); }
static inline void cleanup_evhttp(struct evhttp** http) { if (*http != nullptr) evhttp_free(*http); }
static inline void cleanup_evbuffer(struct evbuffer** buf) { if (*buf != nullptr) evbuffer_free(*buf); }
static inline void cleanup_json_object(struct json_object** json) { if (*json != nullptr) json_object_put(*json); }
static inline void cleanup_json_tokener(struct json_tokener** tok) { if (*tok != nullptr) json_tokener_free(*tok); }

/** \\brief Scoped auto-cleanup for event_config. */
#define raii_event_config [[gnu::cleanup(cleanup_event_config)]] struct event_config*
/** \\brief Scoped auto-cleanup for event_base. */
#define raii_event_base [[gnu::cleanup(cleanup_event_base)]] struct event_base*
/** \\brief Scoped auto-cleanup for evhttp. */
#define raii_evhttp [[gnu::cleanup(cleanup_evhttp)]] struct evhttp*
/** \\brief Scoped auto-cleanup for evbuffer. */
#define raii_evbuffer [[gnu::cleanup(cleanup_evbuffer)]] struct evbuffer*
/** \\brief Scoped auto-cleanup for json_object (drops reference count). */
#define raii_json_object [[gnu::cleanup(cleanup_json_object)]] struct json_object*
/** \\brief Scoped auto-cleanup for json_tokener. */
#define raii_json_tokener [[gnu::cleanup(cleanup_json_tokener)]] struct json_tokener*
""",

    "include/sales.h": """#pragma once
#include <json-c/json.h>

/**
 * \\file sales.h
 * \\brief Database integration for sales analytics data.
 */

/** 
 * \\brief Fetch sales data from ODBC backend between a date range. 
 * \\param start_date Beginning of the range (YYYY-MM-DD).
 * \\param end_date End of the range (YYYY-MM-DD).
 * \\return A newly allocated json_object array (caller must free).
 */
struct json_object* sales_service_get_data(const char* start_date, const char* end_date);
""",

    "include/server.h": """#pragma once

#include <stddef.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <event2/http.h>
#include <sql.h>
#include <sqlext.h>

/**
 * \\file server.h
 * \\brief Core libevent HTTP server lifecycle and statistics manager.
 */

/** \\brief Retrieves the global ODBC environment handle. \\return SQLHENV handle. */
SQLHENV server_get_odbc_env(void);

constexpr int SERVER_PORT = 8080;
constexpr char SERVER_ADDR[] = "0.0.0.0";
#define MAX_ERR_MSG_LEN 256

/** \\brief Initializes the global server state and ODBC environment. */
int server_init_globals(size_t total_workers);

/** \\brief Instructs all worker threads to safely shut down and breaks the main reactor. */
void server_shutdown_workers(void);

typedef struct json_object* (*json_handler_fn)(struct evhttp_request*, struct json_object*, void*, int*, const char**);
typedef struct evbuffer* (*text_handler_fn)(struct evhttp_request*, struct json_object*, void*, int*, const char**);

/** \\brief Retrieves the hardcoded server version string. */
const char* get_server_version(void);

/** \\brief Records telemetry for a processed HTTP request. \\param elapsed_ns Processing time in nanoseconds. */
void server_record_request_stats(long long elapsed_ns);

/** \\brief Retrieves aggregated HTTP request performance metrics. */
void server_get_request_stats(uint64_t* total_requests, uint64_t* total_time_ns, uint64_t* avg_time_ns);

/** \\brief Retrieves memory utilization metrics for the current process. */
void server_get_memory_stats(uint64_t* total_ram_kb, uint64_t* mem_usage_kb);

/** \\brief Entry point logic for libevent worker threads. */
void* worker_thread_logic(void* arg);

/** \\brief Retrieves the ISO-8601 startup timestamp of the server. */
const char* server_get_start_time(void);
""",

    "include/shippers.h": """#pragma once
#include <json-c/json.h>

/**
 * \\file shippers.h
 * \\brief Database integration for shipping carrier data.
 */

/** \\brief Retrieves a list of active shippers from the database. \\return A json_object array (caller frees). */
struct json_object* shippers_get_data(void);
""",

    "include/validation.h": """#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <json-c/json.h>

/**
 * \\file validation.h
 * \\brief JSON schema validation framework.
 */

typedef enum {
    ERR_REQUIRED,
    ERR_NOT_INT,
    ERR_NOT_DOUBLE,
    ERR_NOT_STRING,
    ERR_NOT_DATE,
    ERR_INVALID_DATE,
    ERR_UNKNOWN_TYPE,
    ERR_NEGATIVE_AMOUNT,
    ERR_START_AFTER_END,
    ERR_INVALID_CUSTOMER_ID,
    ERR_MAX_ERRORS
} ErrorCode;

typedef enum {
    TYPE_INT,
    TYPE_DOUBLE,
    TYPE_STRING,
    TYPE_DATE,
    TYPE_MAX_TYPES
} FieldType;

typedef struct ValidationContext ValidationContext;
typedef bool (*CustomValidatorFunc)(const ValidationContext *ctx, const json_object *obj, const char *field_name, char *err_buf, size_t err_len);

/** \\brief Defines a schema rule for a single JSON field. */
typedef struct {
    const char *field_name;
    FieldType type;
    bool is_required;
    CustomValidatorFunc custom_validator;
} FieldValidator;

/** \\brief Holds an entire JSON validation schema. */
struct ValidationContext {
    const FieldValidator *schema;
    size_t schema_count;
    CustomValidatorFunc global_validator;
};

/** \\brief Formats and copies a validation error message into a buffer. */
bool emit_error(char *err_buf, size_t err_len, ErrorCode code, const char *arg);

/** \\brief Validates a parsed JSON object against a ValidationContext schema. */
bool validate_json(const ValidationContext *ctx, const json_object *root, char *err_buf, size_t err_len);

// JSON extraction utility functions
/** \\brief Retrieves a string property from a JSON object. */
const char* json_get_string(const struct json_object* obj, const char* key);

/** \\brief Retrieves a 64-bit integer property from a JSON object. */
int64_t json_get_int(const struct json_object* obj, const char* key);

/** \\brief Retrieves a double-precision property from a JSON object. */
double json_get_double(const struct json_object* obj, const char* key);
"""
}

for filepath, content in files.items():
    print(f"Writing {filepath}...")
    with open(filepath, "w") as f:
        f.write(content)

print("All Doxygen documentation headers written successfully.")
