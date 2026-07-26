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
#include <string.h>

static const char* get_client_ip(void);
static const char* get_session_id(void);
static void set_content_type(const char* ctype);

#include <time.h>
#include <ctype.h>
#include <sodium.h>
#include "json_util.h"
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>

void ping_handler(struct json_object* body, void* arg, int* out_status,  struct evbuffer* out_buf) {
    (void)body; (void)arg;
    
    *out_status = HTTP_OK;
    
    const char* msg = "{\"status\":\"OK\"}";
    evbuffer_add(out_buf, msg, strlen(msg));
}

void version_handler(struct json_object* body, void* arg, int* out_status,  struct evbuffer* out_buf) {
    (void)body; (void)arg;
    
    *out_status = HTTP_OK;
    
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

void sysinfo_handler(struct json_object* body, void* arg, int* out_status,  struct evbuffer* out_buf) {
    (void)body; (void)arg;
    
    *out_status = HTTP_OK;
    
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

void metrics_handler(struct json_object* body, void* arg, int* out_status,  struct evbuffer* out_buf) {
    (void)body; (void)arg;
    
    *out_status = HTTP_OK;
    
    server_request_stats_t stats = {0};
    server_get_request_stats(&stats);
    
    uint64_t total_ram_kb = 0;
    uint64_t mem_usage_kb = 0;
    server_get_memory_stats(&total_ram_kb, &mem_usage_kb);
    
    char buf[2048];
    int len = snprintf(buf, sizeof(buf),
        "# HELP microservice_requests_total Total number of processed requests\n"
        "# TYPE microservice_requests_total counter\n"
        "microservice_requests_total %" PRIu64 "\n\n"
        "# HELP microservice_requests_fast_total Total number of processed requests in fast pool\n"
        "# TYPE microservice_requests_fast_total counter\n"
        "microservice_requests_fast_total %" PRIu64 "\n\n"
        "# HELP microservice_requests_slow_total Total number of processed requests in slow pool\n"
        "# TYPE microservice_requests_slow_total counter\n"
        "microservice_requests_slow_total %" PRIu64 "\n\n"
        "# HELP microservice_processing_time_milliseconds_total Total processing time across all requests\n"
        "# TYPE microservice_processing_time_milliseconds_total counter\n"
        "microservice_processing_time_milliseconds_total %" PRIu64 "\n\n"
        "# HELP microservice_processing_time_fast_milliseconds_total Total processing time for fast pool\n"
        "# TYPE microservice_processing_time_fast_milliseconds_total counter\n"
        "microservice_processing_time_fast_milliseconds_total %" PRIu64 "\n\n"
        "# HELP microservice_processing_time_slow_milliseconds_total Total processing time for slow pool\n"
        "# TYPE microservice_processing_time_slow_milliseconds_total counter\n"
        "microservice_processing_time_slow_milliseconds_total %" PRIu64 "\n\n"
        "# HELP microservice_memory_usage_bytes Current resident memory size in bytes\n"
        "# TYPE microservice_memory_usage_bytes gauge\n"
        "microservice_memory_usage_bytes %" PRIu64 "\n\n"
        "# HELP microservice_memory_total_bytes Total physical memory in bytes\n"
        "# TYPE microservice_memory_total_bytes gauge\n"
        "microservice_memory_total_bytes %" PRIu64 "\n",
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

void rsysinfo_handler(struct json_object* body, void* arg, int* out_status,  struct evbuffer* out_buf) {
    (void)body; (void)arg;
    
    *out_status = HTTP_OK;

    char api_key[256] = {0};
    config_get_remote_api_key(api_key, sizeof(api_key));
    char auth_header[512];
    (void)snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

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

/**
 * customer_id_validator: Custom field validator for customer IDs.
 * 
 * Enforces business logic requiring customer IDs to be exactly 5 alphabetical 
 * characters long (e.g. "ALFKI"). This guarantees database query safety and 
 * data integrity before invoking the stored procedure.
 */
static bool customer_id_validator(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *obj, 
    [[maybe_unused]] const char *name, 
    char *err_buf, 
    size_t err_len
) {
    const char *id = json_object_get_string((json_object *)obj);
    if (!id || strlen(id) != 5) {
        (void)snprintf(err_buf, err_len, "Invalid customer ID format: %s", id ? id : "null");
        return false;
    }
    for (int i = 0; i < 5; ++i) {
        if (!isalpha((unsigned char)id[i])) {
            (void)snprintf(err_buf, err_len, "Invalid customer ID character: %s", id);
            return false;
        }
    }
    return true;
}

static const FieldValidator CustomerSchema[] = {
    {.field_name = "id", .type = TYPE_STRING, .is_required = true, .custom_validator = customer_id_validator}
};

const ValidationContext CustomerContext = {
    .schema = CustomerSchema,
    .schema_count = sizeof(CustomerSchema) / sizeof(CustomerSchema[0]),
    .global_validator = nullptr
};

void rcustomer_handler(
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    struct evbuffer* out_buf
) {
    const char *customer_id = json_get_string(body, "id");
    
    *out_status = HTTP_OK;
    
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
    
    evbuffer_add(out_buf, "}", 1);
}


void customer_handler(
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    
    const char* customer_id = json_get_string(body, "id");
    QueryParam params[] = {
        { .type = PARAM_STRING, .value = customer_id }
    };
    
    if (!odbcutil_get_json("{CALL sp_customer_get(?)}", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

// --- Sales Handler & Schema ---

/**
 * sales_invariant_validator: Global validator for the Sales endpoint schema.
 * 
 * Enforces business logic invariants across multiple fields in the sales request.
 * Specifically, it ensures that the queried 'start_date' chronologically 
 * precedes the 'end_date', preventing invalid or negative time range queries 
 * from hitting the database.
 */
static bool sales_invariant_validator(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *root, 
    [[maybe_unused]] const char *name, 
    char *err_buf, 
    size_t err_len
) {
    const char* start_str = json_get_string(root, "start_date");
    const char* end_str = json_get_string(root, "end_date");

    // NOTE: This relies exclusively on the TYPE_DATE field validators (in SalesSchema)
    // guaranteeing that the strings are strictly formatted as zero-padded ISO-8601 (YYYY-MM-DD).
    // Because of this fixed-width ISO-8601 format invariant, lexical string order is mathematically 
    // identical to chronological order, making strcmp highly optimized and perfectly safe here.
    // (Also note that >= 0 correctly rejects identical dates, forcing start to be strictly before end).
    if (strcmp(start_str, end_str) >= 0) {
        (void)snprintf(err_buf, err_len, "Start date must strictly precede end date");
        return false;
    }

    return true;
}

static bool validate_sales_start_date(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *obj, 
    [[maybe_unused]] const char *name, 
    char *err_buf, 
    size_t err_len
) {
    const char *date_str = json_object_get_string((json_object *)obj);
    int year = (int)strtol(date_str, NULL, 10);
    if (year <= 1993) {
        (void)snprintf(err_buf, err_len, "Start date is too early (min 1994)");
        return false;
    }
    return true;
}

static bool validate_sales_end_date(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *obj, 
    [[maybe_unused]] const char *name, 
    char *err_buf, 
    size_t err_len
) {
    const char *date_str = json_object_get_string((json_object *)obj);
    int year = (int)strtol(date_str, NULL, 10);
    if (year >= 1997) {
        (void)snprintf(err_buf, err_len, "End date is too late (max 1996)");
        return false;
    }
    return true;
}

static const FieldValidator SalesSchema[] = {
    {.field_name = "start_date", .type = TYPE_DATE, .is_required = true, .custom_validator = validate_sales_start_date},
    {.field_name = "end_date",   .type = TYPE_DATE, .is_required = true, .custom_validator = validate_sales_end_date}
};

const ValidationContext SalesContext = {
    .schema = SalesSchema,
    .schema_count = sizeof(SalesSchema) / sizeof(SalesSchema[0]),
    .global_validator = sales_invariant_validator
};

void sales_handler(
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    
    const char* start_date = json_get_string(body, "start_date");
    const char* end_date = json_get_string(body, "end_date");
    QueryParam params[] = {
        { .type = PARAM_STRING, .value = start_date },
        { .type = PARAM_STRING, .value = end_date }
    };
    
    if (!odbcutil_get_json("{CALL sp_sales_by_category(?,?)}", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

// --- TOTP QR Handler ---

void shippers_handler(
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
        
    const char* user = get_user();
    const char* session = get_session_id();
    LOG_AUDIT("shippers_handler accessed by User: %s, SessionID: %s", 
              user ? user : "unknown", 
              session ? session : "unknown");
              
    if (!odbcutil_get_json("{CALL sp_shippers_view}", nullptr, 0, out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

void products_handler(
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    struct evbuffer* out_buf
) {
    if (!odbcutil_get_json("{CALL sp_products_view}", nullptr, 0, out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
        const char* err = "{\"error\":\"Database error\"}";
        evbuffer_add(out_buf, err, strlen(err));
    } else {
        *out_status = HTTP_OK;
    }
}

void getqr_handler(
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    struct evbuffer* out_buf
) {
    const char* user = get_user();
    
    totp_generate_svg(user, out_status, out_buf);
    if (*out_status == HTTP_OK) {
        set_content_type("image/svg+xml");
    }
}

void uuid_handler(
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    
    char uuid_str[37];
    generate_uuidv4(uuid_str);
    
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "{\"uuid\":\"%s\"}", uuid_str);
    evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
}

// --- Shared Utilities ---

static _Thread_local char tl_user[33] = {0};
static _Thread_local char tl_session[37] = {0};
static _Thread_local char tl_client_ip[64] = {0};
static _Thread_local char tl_uri[1024] = {0};
static _Thread_local char tl_content_type[128] = {0};

void handlers_set_context(const char* user, const char* session, const char* client_ip, const char* uri) {
    if (user) (void)snprintf(tl_user, sizeof(tl_user), "%s", user);
    else tl_user[0] = '\0';
    if (session) (void)snprintf(tl_session, sizeof(tl_session), "%s", session);
    else tl_session[0] = '\0';
    if (client_ip) (void)snprintf(tl_client_ip, sizeof(tl_client_ip), "%s", client_ip);
    else tl_client_ip[0] = '\0';
    if (uri) (void)snprintf(tl_uri, sizeof(tl_uri), "%s", uri);
    else tl_uri[0] = '\0';
}

void handlers_clear_context(void) {
    tl_user[0] = '\0';
    tl_session[0] = '\0';
    tl_client_ip[0] = '\0';
    tl_uri[0] = '\0';
    tl_content_type[0] = '\0';
}

const char* get_user(void) { return tl_user[0] ? tl_user : nullptr; }
static const char* get_session_id(void) { return tl_session[0] ? tl_session : nullptr; }
static const char* get_client_ip(void) { return tl_client_ip[0] ? tl_client_ip : nullptr; }

static void set_content_type(const char* ctype) {
    if (ctype) (void)snprintf(tl_content_type, sizeof(tl_content_type), "%s", ctype);
    else tl_content_type[0] = '\0';
}

const char* get_content_type(void) {
    return tl_content_type[0] ? tl_content_type : nullptr;
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
        (void)snprintf(err_buf, err_len, "username exceeds 32 characters");
        return false;
    }

    if (password && strlen(password) > 32) {
        (void)snprintf(err_buf, err_len, "password exceeds 32 characters");
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
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    
    char session_id[37];
    generate_uuidv4(session_id);

    char jwt_secret[MAX_CONFIG_STR];
    config_get_jwt_secret(jwt_secret, sizeof(jwt_secret));
    long jwt_timeout = config_get_jwt_timeout_seconds();

    char token[1024];
    if (jwt_create(username, session_id, jwt_secret, jwt_timeout, token, sizeof(token))) {
        char buf[1200];
        int len = snprintf(buf, sizeof(buf), "{\"token\":\"%s\"}", token);
        evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
        LOG_AUDIT("Login OK - Username: %s, SessionID: %s, RemoteIP: %s", username, session_id, remote_ip);
    } else {
        *out_status = HTTP_INTERNAL;
        LOG_WARN("Failed to generate token for Username: %s, RemoteIP: %s", username, remote_ip);
    }
}

static void handle_login_failure(
    const char* username, 
    const char* remote_ip, 
    long http_code, 
    struct json_object* remote_response,
    int* out_status, 
    struct evbuffer* out_buf
) {
    *out_status = (int)http_code;
    if (http_code == 0) {
        *out_status = HTTP_INTERNAL;
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
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    struct evbuffer* out_buf
) {
    const char* username = json_get_string(body, "username");
    const char* password = json_get_string(body, "password");

    const char* remote_ip = get_client_ip();

    long http_code = 0;
    struct json_object* remote_response = login_service_authenticate(username, password, &http_code);

    if (http_code == 200) {
        if (remote_response) json_object_put(remote_response);
        handle_login_success(username, remote_ip, out_status, out_buf);
    } else {
        handle_login_failure(username, remote_ip, http_code, remote_response, out_status, out_buf);
    }
}

// --- Employee Handler & Schema ---

static bool employee_id_validator(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *obj, 
    [[maybe_unused]] const char *name, 
    char *err_buf, 
    size_t err_len
) {
    int id = json_object_get_int((json_object *)obj);
    if (id <= 0 || id >= 10) {
        (void)snprintf(err_buf, err_len, "Employee ID must be greater than 0 and less than 10");
        return false;
    }
    return true;
}

static const FieldValidator EmployeeSchema[] = {
    {.field_name = "id", .type = TYPE_INT, .is_required = true, .custom_validator = employee_id_validator}
};

const ValidationContext EmployeeContext = {
    .schema = EmployeeSchema,
    .schema_count = sizeof(EmployeeSchema) / sizeof(EmployeeSchema[0]),
    .global_validator = nullptr
};

void employee_handler(
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    
    int emp_id = json_get_int(body, "id");
    
    QueryParam params[] = {
        { .type = PARAM_INT, .value = &emp_id }
    };
    
    if (!odbcutil_get_rs2json("{CALL emp_get(?)}", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

// --- Prodget Handler & Schema ---

static bool prodget_id_validator(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *obj, 
    [[maybe_unused]] const char *name, 
    char *err_buf, 
    size_t err_len
) {
    int id = (int)json_object_get_int((struct json_object *)obj);
    if (id <= 0 || id >= 30) {
        (void)snprintf(err_buf, err_len, "Invalid id: %d (must be > 0 and < 30)", id);
        return false;
    }
    return true;
}

static const FieldValidator ProdgetSchema[] = {
    {.field_name = "id", .type = TYPE_INT, .is_required = true, .custom_validator = prodget_id_validator}
};

const ValidationContext ProdgetContext = {
    .schema = ProdgetSchema,
    .schema_count = sizeof(ProdgetSchema) / sizeof(ProdgetSchema[0]),
    .global_validator = nullptr
};

void prodget_handler(
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    
    int id = json_get_int(body, "id");
    
    QueryParam params[] = {
        { .type = PARAM_INT, .value = &id }
    };
    
    if (!odbcutil_get_rs2json("{CALL product_get(?)}", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}
