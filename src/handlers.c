#include "handlers.h"
#include <assert.h>
#include "server.h"
#include "http_client.h"
#include "customer.h"

#include "validation.h"
#include "config.h"
#include "login.h"
#include "totp.h"
#include "logger.h"
#include "jwt.h"
#include "odbcutil.h"
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sodium.h>
#include "json_util.h"
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>

void ping_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf) {
    (void)req; (void)body; (void)arg;
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    const char* msg = "{\"status\":\"OK\"}";
    evbuffer_add(out_buf, msg, strlen(msg));
}

static bool validate_telemetry_api_key(struct evhttp_request* req) {
    char expected_key[MAX_CONFIG_STR];
    config_get_telemetry_api_key(expected_key, sizeof(expected_key));
    if (expected_key[0] == '\0') {
        LOG_WARN("TELEMETRY_API_KEY is not configured!");
        return false;
    }
    size_t expected_len = strlen(expected_key);
    struct evkeyvalq* headers = evhttp_request_get_input_headers(req);
    
    const char* auth_header = evhttp_find_header(headers, "X-API-Key");
    if (auth_header && strlen(auth_header) == expected_len && sodium_memcmp(auth_header, expected_key, expected_len) == 0) {
        sodium_memzero(expected_key, sizeof(expected_key));
        return true;
    }
    
    const char* bearer = evhttp_find_header(headers, "Authorization");
    if (bearer && strncmp(bearer, "Bearer ", 7) == 0 && strlen(bearer + 7) == expected_len && sodium_memcmp(bearer + 7, expected_key, expected_len) == 0) {
        sodium_memzero(expected_key, sizeof(expected_key));
        return true;
    }
    
    sodium_memzero(expected_key, sizeof(expected_key));
    return false;
}

void version_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf) {
    (void)req; (void)body; (void)arg;
    
    if (!validate_telemetry_api_key(req)) {
        *out_status = 403;
        *out_status_txt = "Access Denied";
        return;
    }
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    char esc_version[64], esc_compiler[256], esc_date[128], esc_hostname[256], esc_os[256];
    json_encode_string(get_server_version(), esc_version, sizeof(esc_version));
    json_encode_string(__VERSION__, esc_compiler, sizeof(esc_compiler));
    json_encode_string(__DATE__ " " __TIME__, esc_date, sizeof(esc_date));
    json_encode_string(server_get_hostname(), esc_hostname, sizeof(esc_hostname));
    json_encode_string(server_get_os_version(), esc_os, sizeof(esc_os));

    char buf[1024];
    int len = snprintf(buf, sizeof(buf),
        "{\"version\":\"%s\",\"compiler\":\"%s\",\"compile_date\":\"%s\",\"hostname\":\"%s\",\"os_version\":\"%s\"}",
        esc_version,
        esc_compiler,
        esc_date,
        esc_hostname,
        esc_os
    );
    evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
}

void sysinfo_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf) {
    (void)req; (void)body; (void)arg;
    
    if (!validate_telemetry_api_key(req)) {
        *out_status = 403;
        *out_status_txt = "Access Denied";
        return;
    }
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    server_request_stats_t stats = {0};
    server_get_request_stats(&stats);
    
    uint64_t total_ram_kb = 0;
    uint64_t mem_usage_kb = 0;
    server_get_memory_stats(&total_ram_kb, &mem_usage_kb);
    double mem_usage_pct = total_ram_kb > 0 ? ((double)mem_usage_kb / (double)total_ram_kb) * 100.0 : 0.0;
    
    char esc_start[128], esc_hostname[256];
    json_encode_string(server_get_start_time(), esc_start, sizeof(esc_start));
    json_encode_string(server_get_hostname(), esc_hostname, sizeof(esc_hostname));

    char buf[1024];
    int len = snprintf(buf, sizeof(buf),
        "{"
        "\"start_time\":\"%s\","
        "\"total_requests\":%" PRIu64 ","
        "\"average_processing_time_fast_ms\":%" PRIu64 ","
        "\"average_processing_time_slow_ms\":%" PRIu64 ","
        "\"total_ram_kb\":%" PRIu64 ","
        "\"memory_usage_kb\":%" PRIu64 ","
        "\"memory_usage_percentage\":%.2f,"
        "\"hostname\":\"%s\""
        "}",
        esc_start,
        (uint64_t)stats.total_requests,
        (uint64_t)stats.avg_time_fast_ms,
        (uint64_t)stats.avg_time_slow_ms,
        total_ram_kb,
        mem_usage_kb,
        mem_usage_pct,
        esc_hostname
    );
    evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
}

void metrics_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf) {
    (void)req; (void)body; (void)arg;
    
    if (!validate_telemetry_api_key(req)) {
        *out_status = 403;
        *out_status_txt = "Access Denied";
        return;
    }
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    server_request_stats_t stats = {0};
    server_get_request_stats(&stats);
    
    uint64_t total_ram_kb = 0;
    uint64_t mem_usage_kb = 0;
    server_get_memory_stats(&total_ram_kb, &mem_usage_kb);
    
    char buf[2048];
    int len = snprintf(buf, sizeof(buf),
        "# HELP microservice_requests_total Total number of processed requests\n"
        "# TYPE microservice_requests_total counter\n"
        "microservice_requests_total %lu\n\n"
        "# HELP microservice_requests_fast_total Total number of processed requests in fast pool\n"
        "# TYPE microservice_requests_fast_total counter\n"
        "microservice_requests_fast_total %lu\n\n"
        "# HELP microservice_requests_slow_total Total number of processed requests in slow pool\n"
        "# TYPE microservice_requests_slow_total counter\n"
        "microservice_requests_slow_total %lu\n\n"
        "# HELP microservice_processing_time_milliseconds_total Total processing time across all requests\n"
        "# TYPE microservice_processing_time_milliseconds_total counter\n"
        "microservice_processing_time_milliseconds_total %lu\n\n"
        "# HELP microservice_processing_time_fast_milliseconds_total Total processing time for fast pool\n"
        "# TYPE microservice_processing_time_fast_milliseconds_total counter\n"
        "microservice_processing_time_fast_milliseconds_total %lu\n\n"
        "# HELP microservice_processing_time_slow_milliseconds_total Total processing time for slow pool\n"
        "# TYPE microservice_processing_time_slow_milliseconds_total counter\n"
        "microservice_processing_time_slow_milliseconds_total %lu\n\n"
        "# HELP microservice_memory_usage_bytes Current resident memory size in bytes\n"
        "# TYPE microservice_memory_usage_bytes gauge\n"
        "microservice_memory_usage_bytes %lu\n\n"
        "# HELP microservice_memory_total_bytes Total physical memory in bytes\n"
        "# TYPE microservice_memory_total_bytes gauge\n"
        "microservice_memory_total_bytes %lu\n",
        stats.total_requests,
        stats.total_requests_fast,
        stats.total_requests_slow,
        stats.total_time_ms,
        stats.total_time_fast_ms,
        stats.total_time_slow_ms,
        mem_usage_kb * 1024,
        total_ram_kb * 1024
    );
    evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
    
}

void rsysinfo_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt, struct evbuffer* out_buf) {
    (void)req; (void)body; (void)arg;
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";

    char api_key[256] = {0};
    config_get_remote_api_key(api_key, sizeof(api_key));
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

    const char* headers[] = {
        auth_header
    };

    char api_url[256];
    config_get_api_url(api_url, sizeof(api_url));

    long http_code = 0;
    struct json_object* remote_json = http_client_get_json(api_url, "/api/metrics", headers, 1, &http_code);

    char prefix[128];
    int len = snprintf(prefix, sizeof(prefix), "{\"remote_status\":%ld,\"remote_data\":", http_code);
    evbuffer_add(out_buf, prefix, len < (int)sizeof(prefix) ? (size_t)len : sizeof(prefix) - 1);
    if (remote_json) {
        const char* json_str = json_object_to_json_string_ext(remote_json, JSON_C_TO_STRING_PLAIN);
        evbuffer_add(out_buf, json_str, strlen(json_str));
        json_object_put(remote_json);
    } else {
        evbuffer_add(out_buf, "null", 4);
    }
    evbuffer_add(out_buf, "}", 1);
}

// --- Customer Handler & Schema ---

static bool validate_alpha_id(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *obj, 
    [[maybe_unused]] const char *name, 
    char *err_buf, 
    size_t err_len
) {
    const char *id = json_object_get_string((json_object *)obj);
    if (!id || strlen(id) != 5) {
        return emit_error(err_buf, err_len, ERR_INVALID_CUSTOMER_ID, id ? id : "null");
    }
    for (int i = 0; i < 5; ++i) {
        if (!isalpha((unsigned char)id[i])) {
            return emit_error(err_buf, err_len, ERR_INVALID_CUSTOMER_ID, id);
        }
    }
    return true;
}

static const FieldValidator CustomerSchema[] = {
    {.field_name = "id", .type = TYPE_STRING, .is_required = true, .custom_validator = validate_alpha_id}
};

const ValidationContext CustomerContext = {
    .schema = CustomerSchema,
    .schema_count = sizeof(CustomerSchema) / sizeof(CustomerSchema[0]),
    .global_validator = nullptr
};

static thread_local SQLLEN cb_nts = SQL_NTS;

static void customer_bind_cb(struct json_object* body, SQLHSTMT hstmt) {
    const char* customer_id = json_get_string(body, "id");
    size_t len = customer_id ? strlen(customer_id) : 0;
    SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, len, 0, (SQLPOINTER)customer_id, len, &cb_nts);
}

void customer_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    [[maybe_unused]] const char** out_status_txt,
    struct evbuffer* out_buf
) {
    const char *customer_id = json_get_string(body, "id");
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    long http_code = 0;
    struct json_object* remote_json = customer_service_get_info(customer_id, &http_code);
    
    char prefix[128];
    int len = snprintf(prefix, sizeof(prefix), "{\"remote_status\":%ld,\"remote_data\":", http_code);
    evbuffer_add(out_buf, prefix, len < (int)sizeof(prefix) ? (size_t)len : sizeof(prefix) - 1);
    if (remote_json) {
        const char* json_str = json_object_to_json_string_ext(remote_json, JSON_C_TO_STRING_PLAIN);
        evbuffer_add(out_buf, json_str, strlen(json_str));
        json_object_put(remote_json);
    } else {
        evbuffer_add(out_buf, "null", 4);
    }
    
    evbuffer_add(out_buf, ",\"db_data\":", 11);
    
    if (!odbcutil_get_json("{CALL sp_customer_get(?)}", customer_bind_cb, body, out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
        *out_status_txt = "Internal Server Error";
        return;
    }
    
    evbuffer_add(out_buf, "}", 1);
}


void customer_get_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    [[maybe_unused]] const char** out_status_txt,
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    if (!odbcutil_get_json("{CALL sp_customer_get(?)}", customer_bind_cb, body, out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
        *out_status_txt = "Internal Server Error";
    }
}

// --- Sales Handler & Schema ---

static bool validate_start_before_end(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *root, 
    [[maybe_unused]] const char *name, 
    char *err_buf, 
    size_t err_len
) {
    const char* start_str = json_get_string(root, "start_date");
    const char* end_str = json_get_string(root, "end_date");

    // Defensive check to avoid crash if fields are unexpectedly missing
    if (!start_str || !end_str) {
        return emit_error(err_buf, err_len, ERR_REQUIRED, "start_date/end_date");
    }

    if (strcmp(start_str, end_str) >= 0) {
        return emit_error(err_buf, err_len, ERR_START_AFTER_END, nullptr);
    }

    return true;
}

static const FieldValidator SalesSchema[] = {
    {.field_name = "start_date", .type = TYPE_DATE, .is_required = true, .custom_validator = nullptr},
    {.field_name = "end_date",   .type = TYPE_DATE, .is_required = true, .custom_validator = nullptr}
};

const ValidationContext SalesContext = {
    .schema = SalesSchema,
    .schema_count = sizeof(SalesSchema) / sizeof(SalesSchema[0]),
    .global_validator = validate_start_before_end
};

static void sales_bind_cb(struct json_object* body, SQLHSTMT hstmt) {
    const char* start_date = json_get_string(body, "start_date");
    const char* end_date = json_get_string(body, "end_date");
    
    size_t start_len = start_date ? strlen(start_date) : 0;
    size_t end_len = end_date ? strlen(end_date) : 0;
    SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, start_len, 0, (SQLPOINTER)start_date, start_len, &cb_nts);
    SQLBindParameter(hstmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, end_len, 0, (SQLPOINTER)end_date, end_len, &cb_nts);
}

void sales_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    [[maybe_unused]] const char** out_status_txt,
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    if (!odbcutil_get_json("{CALL sp_sales_by_category(?,?)}", sales_bind_cb, body, out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
        *out_status_txt = "Internal Server Error";
    }
}

// --- TOTP QR Handler ---

void getqr_handler(
    struct evhttp_request* req, 
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    const char** out_status_txt,
    struct evbuffer* out_buf
) {
    const char* user = get_user(req);
    
    totp_generate_svg(user, out_status, out_status_txt, out_buf);
    if (*out_status == HTTP_OK) {
        struct evkeyvalq* headers = evhttp_request_get_output_headers(req);
        evhttp_add_header(headers, "Content-Type", "image/svg+xml");
    }
}

void shippers_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    const char** out_status_txt,
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
        
    const char* user = get_user(req);
    const char* session = get_session_id(req);
    LOG_AUDIT("shippers_handler accessed by User: %s, SessionID: %s", 
              user ? user : "unknown", 
              session ? session : "unknown");
              
    if (!odbcutil_get_json("{CALL sp_shippers_view}", nullptr, nullptr, out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
        *out_status_txt = "Internal Server Error";
    }
}

void products_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    [[maybe_unused]] const char** out_status_txt,
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    odbcutil_get_json("{CALL sp_products_view}", nullptr, nullptr, out_buf, __func__);
}

void uuid_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    const char** out_status_txt,
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    char uuid_str[37];
    generate_uuidv4(uuid_str);
    
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "{\"uuid\":\"%s\"}", uuid_str);
    evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
}

// --- Shared Utilities ---

bool is_trusted_proxy(struct evhttp_request* req, const char** out_peer_ip) {
    struct evhttp_connection* evcon = evhttp_request_get_connection(req);
    char* peer_ip = nullptr;
    uint16_t port = 0;
    if (evcon) evhttp_connection_get_peer(evcon, &peer_ip, &port);

    if (out_peer_ip) *out_peer_ip = peer_ip;

    char trust_proxy_ip[MAX_CONFIG_STR] = {0};
    config_get_trust_proxy_ip(trust_proxy_ip, sizeof(trust_proxy_ip));

    if (peer_ip && trust_proxy_ip[0] != '\0' && strcmp(peer_ip, trust_proxy_ip) == 0) {
        return true;
    }
    return false;
}

const char* extract_client_ip(struct evhttp_request* req) {
    if (!req) return "unknown";
    
    const char* peer_ip = nullptr;
    if (is_trusted_proxy(req, &peer_ip)) {
        struct evkeyvalq* in_headers = evhttp_request_get_input_headers(req);
        const char* x_forwarded_for = evhttp_find_header(in_headers, "X-Forwarded-For");
        if (x_forwarded_for) {
            static _Thread_local char client_ip_buf[128];
            strncpy(client_ip_buf, x_forwarded_for, sizeof(client_ip_buf) - 1);
            client_ip_buf[sizeof(client_ip_buf) - 1] = '\0';
            
            // Take the first IP in the comma-separated list
            char *comma = strchr(client_ip_buf, ',');
            if (comma) *comma = '\0';
            
            // Trim right whitespace if any
            int len = strlen(client_ip_buf);
            while (len > 0 && isspace((unsigned char)client_ip_buf[len - 1])) {
                client_ip_buf[--len] = '\0';
            }
            return client_ip_buf;
        }
    }
    
    if (peer_ip) return peer_ip;
    
    return "unknown";
}

static _Thread_local char tl_username[33] = {0};
static _Thread_local char tl_session_id[37] = {0};

void handlers_set_identity(const char* user, const char* session) {
    if (user) strncpy(tl_username, user, sizeof(tl_username) - 1);
    else tl_username[0] = '\0';
    
    if (session) strncpy(tl_session_id, session, sizeof(tl_session_id) - 1);
    else tl_session_id[0] = '\0';
}

void handlers_clear_identity(void) {
    tl_username[0] = '\0';
    tl_session_id[0] = '\0';
}

const char* get_user(struct evhttp_request* req) {
    (void)req;
    return tl_username[0] != '\0' ? tl_username : nullptr;
}

const char* get_session_id(struct evhttp_request* req) {
    (void)req;
    return tl_session_id[0] != '\0' ? tl_session_id : nullptr;
}

// --- Login Handler & Schema ---

static const FieldValidator LoginSchema[] = {
    {.field_name = "username", .type = TYPE_STRING, .is_required = true, .custom_validator = nullptr},
    {.field_name = "password", .type = TYPE_STRING, .is_required = true, .custom_validator = nullptr}
};

static bool login_global_validator(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *root, 
    [[maybe_unused]] const char *name, 
    char *err_buf, 
    size_t err_len
) {
    const char* username = json_get_string(root, "username");
    const char* password = json_get_string(root, "password");
    
    if (username && strlen(username) > 32) {
        snprintf(err_buf, err_len, "username exceeds 32 characters");
        return false;
    }

    if (password && strlen(password) > 32) {
        snprintf(err_buf, err_len, "password exceeds 32 characters");
        return false;
    }
    return true;
}

const ValidationContext LoginContext = {
    .schema = LoginSchema,
    .schema_count = sizeof(LoginSchema) / sizeof(LoginSchema[0]),
    .global_validator = login_global_validator
};

static void handle_login_success(
    const char* username, 
    const char* remote_ip, 
    int* out_status, 
    const char** out_status_txt,
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    char session_id[37];
    generate_uuidv4(session_id);

    char jwt_secret[MAX_CONFIG_STR];
    config_get_jwt_secret(jwt_secret, sizeof(jwt_secret));
    long jwt_timeout = config_get_jwt_timeout_seconds();

    char* ticket = jwt_create(username, session_id, jwt_secret, jwt_timeout);

    if (ticket) {
        char buf[512];
        int len = snprintf(buf, sizeof(buf), "{\"token\":\"%s\"}", ticket);
        evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
        LOG_AUDIT("Login OK - Username: %s, SessionID: %s, RemoteIP: %s", username, session_id, remote_ip);
        free(ticket);
    } else {
        const char* msg = "{\"error\":\"Failed to generate ticket\"}";
        evbuffer_add(out_buf, msg, strlen(msg));
        *out_status = HTTP_INTERNAL;
        *out_status_txt = "Internal Server Error";
        LOG_WARN("Failed to generate ticket for Username: %s, RemoteIP: %s", username, remote_ip);
    }
}

static void handle_login_failure(
    const char* username, 
    const char* remote_ip, 
    long http_code, 
    struct json_object* remote_response,
    int* out_status, 
    const char** out_status_txt,
    struct evbuffer* out_buf
) {
    *out_status = (int)http_code;
    *out_status_txt = "Unauthorized";
    if (http_code == 0) {
        *out_status = HTTP_INTERNAL;
        *out_status_txt = "Internal Server Error";
    }

    LOG_WARN("Login failed - Username: %s, RemoteIP: %s, HTTP Code: %ld", username, remote_ip, http_code);

    if (remote_response) {
        const char* final_error = nullptr;
        const char* desc = json_get_string(remote_response, "description");
        const char* err_msg = json_get_string(remote_response, "error");
        
        if (desc) {
            final_error = desc;
        } else if (err_msg) {
            final_error = err_msg;
        } else {
            final_error = "Unknown provider error";
        }
        
        char buf[256];
        int len = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", final_error);
        evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
        json_object_put(remote_response);
    } else {
        const char* msg = "{\"error\":\"Provider unreachable\"}";
        evbuffer_add(out_buf, msg, strlen(msg));
    }
}

void login_handler(
    struct evhttp_request* req, 
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    const char** out_status_txt,
    struct evbuffer* out_buf
) {
    const char* username = json_get_string(body, "username");
    const char* password = json_get_string(body, "password");

    const char* remote_ip = extract_client_ip(req);

    long http_code = 0;
    struct json_object* remote_response = login_service_authenticate(username, password, &http_code);

    if (http_code == 200) {
        if (remote_response) json_object_put(remote_response);
        handle_login_success(username, remote_ip, out_status, out_status_txt, out_buf);
    } else {
        handle_login_failure(username, remote_ip, http_code, remote_response, out_status, out_status_txt, out_buf);
    }
}


