#include "handlers.h"
#include <apiserver/server.h>
#include <apiserver/http_client.h>
#include <apiserver/validation.h>
#include <apiserver/config.h>
#include <apiserver/logger.h>
#include <apiserver/jwt.h>
#include <apiserver/odbcutil.h>
#include <apiserver/raii.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <ctype.h>
#include <sodium.h>
#include <apiserver/json_util.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <stdckdint.h>
#include <curl/curl.h>
#include <stdint.h>
#include <gnu/libc-version.h>
#include "customer.h"
#include "login.h"
#include "totp.h"


// ==============================================================================
// UTILITY FUNCTIONS & SHARED CONTEXT
// ==============================================================================

#include <apiserver/context.h>
void build_sysinfo_json_string(char* buf, size_t max_len) {
    server_request_stats_t stats = {0};
    server_get_request_stats(&stats);
    
    uint64_t total_ram_kb = 0;
    uint64_t mem_usage_kb = 0;
    server_get_memory_stats(&total_ram_kb, &mem_usage_kb);
    double mem_usage_pct = total_ram_kb > 0 ? ((double)mem_usage_kb / (double)total_ram_kb) * 100.0 : 0.0;
    
    char esc_start[128];
    char esc_hostname[256];
    json_encode_string(server_get_start_time(), esc_start, sizeof(esc_start));
    json_encode_string(server_get_hostname(), esc_hostname, sizeof(esc_hostname));

    (void)snprintf(buf, max_len,
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
}

static void append_remote_json_response(struct evbuffer* out_buf, struct json_object* remote_json, long http_code) {
    [[gnu::cleanup(cleanup_json_object)]] struct json_object* scoped_json = remote_json;
    char prefix[128];
    int len = snprintf(prefix, sizeof(prefix), "{\"remote_status\":%ld,\"remote_data\":", http_code);
    if (len >= 0) {
        evbuffer_add(out_buf, prefix, len < (int)sizeof(prefix) ? (size_t)len : sizeof(prefix) - 1);
    }
    if (scoped_json) {
        const char* json_str = json_object_to_json_string_ext(scoped_json, JSON_C_TO_STRING_PLAIN);
        evbuffer_add(out_buf, json_str, strlen(json_str));
    } else {
        evbuffer_add(out_buf, "null", 4);
    }
    evbuffer_add(out_buf, "}", 1);
}

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
        char esc_token[2048];
        json_encode_string(token, esc_token, sizeof(esc_token));
        
        char buf[2500];
        int len = snprintf(buf, sizeof(buf), "{\"token\":\"%s\"}", esc_token);
        if (len >= 0) {
            evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
        }
        LOG_AUDIT("Login OK - Username: %s, SessionID: %s, RemoteIP: %s", username, session_id, remote_ip);
    } else {
        *out_status = HTTP_INTERNAL;
        LOG_WARN("Failed to generate token for Username: %s, RemoteIP: %s", username, remote_ip);
    }
    
    sodium_memzero(jwt_secret, sizeof(jwt_secret));
    sodium_memzero(token, sizeof(token));
}

static void handle_login_failure(
    const char* username, 
    const char* remote_ip, 
    long http_code, 
    const struct json_object* remote_response,
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
        
        char escaped_error[256];
        json_encode_string(final_error, escaped_error, sizeof(escaped_error));
        char buf[512];
        int len = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", escaped_error);
        evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
    } else {
        const char* msg = "{\"error\":\"Provider unreachable\"}";
        evbuffer_add(out_buf, msg, strlen(msg));
    }
}

static bool upload_is_valid_dir(const char* uploads_dir) {
    if (uploads_dir[0] == '\0') {
        LOG_ERROR("UPLOADS_DIR config variable is empty. Uploads are disabled.");
        return false;
    }
    
    struct stat st;
    if (stat(uploads_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        LOG_ERROR("Uploads directory '%s' does not exist or is not a directory.", uploads_dir);
        return false;
    }
    
    return true;
}


// ==============================================================================
// HTTP HANDLERS
// ==============================================================================


void ping_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    (void)body; 
    
    *out_status = HTTP_OK;
    
    const char* msg = "{\"status\":\"OK\"}";
    evbuffer_add(out_buf, msg, strlen(msg));
}

void version_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    (void)body; 
    
    *out_status = HTTP_OK;
    
    char esc_version[64];
    char esc_compiler[256];
    char esc_date[128];
    char esc_hostname[256];
    char esc_os[256];
    char esc_libevent[128];
    char esc_libcurl[256];
    char esc_libc[64];
    json_encode_string(get_server_version(), esc_version, sizeof(esc_version));
    json_encode_string(__VERSION__, esc_compiler, sizeof(esc_compiler));
    json_encode_string(__DATE__ " " __TIME__, esc_date, sizeof(esc_date));
    json_encode_string(server_get_hostname(), esc_hostname, sizeof(esc_hostname));
    json_encode_string(server_get_os_version(), esc_os, sizeof(esc_os));
    json_encode_string(event_get_version(), esc_libevent, sizeof(esc_libevent));
    json_encode_string(curl_version(), esc_libcurl, sizeof(esc_libcurl));
    json_encode_string(gnu_get_libc_version(), esc_libc, sizeof(esc_libc));

    char buf[1024];
    int len = snprintf(buf, sizeof(buf),
        "{\"version\":\"%s\",\"compiler\":\"%s\",\"compile_date\":\"%s\",\"hostname\":\"%s\",\"os_version\":\"%s\",\"libevent_version\":\"%s\",\"libcurl_version\":\"%s\",\"libc_version\":\"%s\"}",
        esc_version,
        esc_compiler,
        esc_date,
        esc_hostname,
        esc_os,
        esc_libevent,
        esc_libcurl,
        esc_libc
    );
    evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
}


void sysinfo_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    (void)body; 
    
    *out_status = HTTP_OK;
    
    char buf[1024];
    build_sysinfo_json_string(buf, sizeof(buf));
    
    evbuffer_add(out_buf, buf, strlen(buf));
}

void metrics_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    (void)body; 
    
    *out_status = HTTP_OK;
    
    server_request_stats_t stats = {0};
    server_get_request_stats(&stats);
    
    uint64_t total_ram_kb = 0;
    uint64_t mem_usage_kb = 0;
    server_get_memory_stats(&total_ram_kb, &mem_usage_kb);
    const char* hname = server_get_hostname();
    
    char buf[2048];
    int len = snprintf(buf, sizeof(buf),
        "# HELP microservice_requests_total Total number of processed requests\n"
        "# TYPE microservice_requests_total counter\n"
        "microservice_requests_total{hostname=\"%s\"} %" PRIu64 "\n\n"
        "# HELP microservice_requests_fast_total Total number of processed requests in fast pool\n"
        "# TYPE microservice_requests_fast_total counter\n"
        "microservice_requests_fast_total{hostname=\"%s\"} %" PRIu64 "\n\n"
        "# HELP microservice_requests_slow_total Total number of processed requests in slow pool\n"
        "# TYPE microservice_requests_slow_total counter\n"
        "microservice_requests_slow_total{hostname=\"%s\"} %" PRIu64 "\n\n"
        "# HELP microservice_processing_time_milliseconds_total Total processing time across all requests\n"
        "# TYPE microservice_processing_time_milliseconds_total counter\n"
        "microservice_processing_time_milliseconds_total{hostname=\"%s\"} %" PRIu64 "\n\n"
        "# HELP microservice_processing_time_fast_milliseconds_total Total processing time for fast pool\n"
        "# TYPE microservice_processing_time_fast_milliseconds_total counter\n"
        "microservice_processing_time_fast_milliseconds_total{hostname=\"%s\"} %" PRIu64 "\n\n"
        "# HELP microservice_processing_time_slow_milliseconds_total Total processing time for slow pool\n"
        "# TYPE microservice_processing_time_slow_milliseconds_total counter\n"
        "microservice_processing_time_slow_milliseconds_total{hostname=\"%s\"} %" PRIu64 "\n\n"
        "# HELP microservice_memory_usage_bytes Current resident memory size in bytes\n"
        "# TYPE microservice_memory_usage_bytes gauge\n"
        "microservice_memory_usage_bytes{hostname=\"%s\"} %" PRIu64 "\n\n"
        "# HELP microservice_memory_total_bytes Total physical memory in bytes\n"
        "# TYPE microservice_memory_total_bytes gauge\n"
        "microservice_memory_total_bytes{hostname=\"%s\"} %" PRIu64 "\n",
        hname, stats.total_requests,
        hname, stats.total_requests_fast,
        hname, stats.total_requests_slow,
        hname, stats.total_time_ms,
        hname, stats.total_time_fast_ms,
        hname, stats.total_time_slow_ms,
        hname, mem_usage_kb * 1024,
        hname, total_ram_kb * 1024
    );
    evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
    
    context_set_content_type("text/plain; version=0.0.4");
}


void rsysinfo_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    (void)body; 
    
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
    
    sodium_memzero(api_key, sizeof(api_key));
    sodium_memzero(auth_header, sizeof(auth_header));

    append_remote_json_response(out_buf, remote_json, http_code);
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
    const json_object *obj, 
    char *err_buf, 
    size_t err_len
) {
    const char *id = json_object_get_string((struct json_object*)(uintptr_t)obj);
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
    {.field_name = "id", .type = TYPE_STRING, .is_required = true, .max_len = 5, .custom_validator = customer_id_validator}
};

const ValidationContext CustomerContext = {
    .schema = CustomerSchema,
    .schema_count = sizeof(CustomerSchema) / sizeof(CustomerSchema[0]),
    .global_validator = nullptr
};

void rcustomer_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    const char *customer_id = json_get_string(body, "id");
    
    *out_status = HTTP_OK;
    
    if (!customer_id) {
        *out_status = HTTP_BADREQUEST;
        return;
    }
    
    long http_code = 0;
    struct json_object* remote_json = customer_service_get_info(customer_id, &http_code);
    
    append_remote_json_response(out_buf, remote_json, http_code);
}


void customer_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    
    const char* customer_id = json_get_string(body, "id");
    QueryParam params[] = {
        { .type = PARAM_STRING, .value = customer_id }
    };
    
    if (!odbcutil_get_json(DB_0, "{CALL sp_customer_get(?)}", params, ARRAY_SIZE(params), out_buf, __func__)) {
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
    const json_object *root, 
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
    const json_object *obj, 
    char *err_buf, 
    size_t err_len
) {
    const char *date_str = json_object_get_string((struct json_object*)(uintptr_t)obj);
    int year = (int)strtol(date_str, nullptr, 10);
    if (year <= 1993) {
        (void)snprintf(err_buf, err_len, "Start date is too early (min 1994)");
        return false;
    }
    return true;
}

static bool validate_sales_end_date(
    const json_object *obj, 
    char *err_buf, 
    size_t err_len
) {
    const char *date_str = json_object_get_string((struct json_object*)(uintptr_t)obj);
    int year = (int)strtol(date_str, nullptr, 10);
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
    .global_validator = &sales_invariant_validator
};

void sales_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    
    const char* start_date = json_get_string(body, "start_date");
    const char* end_date = json_get_string(body, "end_date");
    QueryParam params[] = {
        { .type = PARAM_STRING, .value = start_date },
        { .type = PARAM_STRING, .value = end_date }
    };
    
    if (!odbcutil_get_json(DB_0, "{CALL sp_sales_by_category(?,?)}", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

// --- Shippers & Products Handlers ---

void shippers_handler([[maybe_unused]] struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
        
    const char* user = context_get_user();
    const char* session = context_get_session_id();
    LOG_AUDIT("shippers_handler accessed by User: %s, SessionID: %s", 
              user ? user : "unknown", 
              session ? session : "unknown");
              
    if (!odbcutil_get_json(DB_0, "{CALL sp_shippers_view}", nullptr, 0, out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

void products_handler([[maybe_unused]] struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    if (!odbcutil_get_json(DB_0, "{CALL sp_products_view}", nullptr, 0, out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

void getqr_handler([[maybe_unused]] struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    const char* user = context_get_user();
    
    totp_generate_svg(user, out_status, out_buf);
    if (*out_status == HTTP_OK) {
        context_set_content_type("image/svg+xml");
    }
}

// --- Verify TOTP Handler & Schema ---

static bool totp_custom_validator(
    const json_object *obj, 
    char *err_buf, 
    size_t err_len
) {
    const char *totp = json_object_get_string((struct json_object*)(uintptr_t)obj);
    if (!totp || strlen(totp) != 6) {
        (void)snprintf(err_buf, err_len, "Field '%s' must be exactly 6 characters.", "totp");
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (!isdigit((unsigned char)totp[i])) {
            (void)snprintf(err_buf, err_len, "Field '%s' must contain only digits.", "totp");
            return false;
        }
    }
    return true;
}

static const FieldValidator VerifyTotpSchema[] = {
    {.field_name = "totp", .type = TYPE_STRING, .is_required = true, .max_len = 6, .custom_validator = totp_custom_validator}
};

const ValidationContext VerifyTotpContext = {
    .schema = VerifyTotpSchema,
    .schema_count = sizeof(VerifyTotpSchema) / sizeof(VerifyTotpSchema[0]),
    .global_validator = nullptr
};

void verifytotp_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    const char* user = context_get_user();
    const char* session = context_get_session_id();
    const char* totp_str = json_get_string(body, "totp");
    const char* client_ip = context_get_client_ip();
    
    if (totp_str && is_valid_totp(user, totp_str)) {
        *out_status = HTTP_OK;
        const char* msg = "{\"status\":\"OK\"}";
        evbuffer_add(out_buf, msg, strlen(msg));
    } else {
        LOG_WARN("TOTP validation failed for User: %s, SessionID: %s from IP: %s", 
            user ? user : "unknown", 
            session ? session : "unknown", 
            client_ip ? client_ip : "unknown");
        *out_status = 401;
    }
}

void uuid_handler([[maybe_unused]] struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    
    char uuid_str[37];
    generate_uuidv4(uuid_str);
    
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "{\"uuid\":\"%s\"}", uuid_str);
    evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
}

void secretb32_handler([[maybe_unused]] struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    char secret[33];
    if (totp_generate_base32_secret(secret, sizeof(secret))) {
        char buf[128];
        int len = snprintf(buf, sizeof(buf), "{\"secret\":\"%s\"}", secret);
        evbuffer_add(out_buf, buf, len < (int)sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
    } else {
        *out_status = HTTP_INTERNAL;
    }
}


// --- Login Handler & Schema ---

static const FieldValidator LoginSchema[] = {
    {.field_name = "username", .type = TYPE_STRING, .is_required = true, .max_len = 32},
    {.field_name = "password", .type = TYPE_STRING, .is_required = true, .max_len = 32}
};

const ValidationContext LoginContext = {
    .schema = LoginSchema,
    .schema_count = sizeof(LoginSchema) / sizeof(LoginSchema[0]),
    .global_validator = nullptr
};


void login_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    const char* username = json_get_string(body, "username");
    const char* password = json_get_string(body, "password");
    const char* remote_ip = context_get_client_ip();

    long http_code = 0;
    [[gnu::cleanup(cleanup_json_object)]] struct json_object* remote_response = login_service_authenticate(username, password, &http_code);

    if (http_code == 200) {
        handle_login_success(username, remote_ip, out_status, out_buf);
    } else {
        handle_login_failure(username, remote_ip, http_code, remote_response, out_status, out_buf);
    }
}

// --- Employee Handler & Schema ---

static const FieldValidator EmployeeSchema[] = {
    {.field_name = "id", .type = TYPE_INT, .is_required = true, .has_min = true, .min_int = 1, .has_max = true, .max_int = 9}
};

const ValidationContext EmployeeContext = {
    .schema = EmployeeSchema,
    .schema_count = sizeof(EmployeeSchema) / sizeof(EmployeeSchema[0]),
    .global_validator = nullptr
};

void employee_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    
    int emp_id = json_get_int(body, "id");
    
    QueryParam params[] = {
        { .type = PARAM_INT, .value = &emp_id }
    };
    
    if (!odbcutil_get_rs2json(DB_0, "{CALL emp_get(?)}", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

// --- Prodget Handler & Schema ---

static const FieldValidator ProdgetSchema[] = {
    {.field_name = "id", .type = TYPE_INT, .is_required = true, .has_min = true, .min_int = 1}
};

const ValidationContext ProdgetContext = {
    .schema = ProdgetSchema,
    .schema_count = sizeof(ProdgetSchema) / sizeof(ProdgetSchema[0]),
    .global_validator = nullptr
};

void prodget_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    
    int id = json_get_int(body, "id");
    
    QueryParam params[] = {
        { .type = PARAM_INT, .value = &id }
    };
    
    if (!odbcutil_get_rs2json(DB_0, "{CALL product_get(?)}", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

// --- Supplier Handler & Schema ---

static const FieldValidator SupplierSchema[] = {
    {.field_name = "id", .type = TYPE_INT, .is_required = true, .has_min = true, .min_int = 1}
};

const ValidationContext SupplierContext = {
    .schema = SupplierSchema,
    .schema_count = sizeof(SupplierSchema) / sizeof(SupplierSchema[0]),
    .global_validator = nullptr
};

void supplier_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    
    int id = json_get_int(body, "id");
    
    QueryParam params[] = {
        { .type = PARAM_INT, .value = &id }
    };
    
    if (!odbcutil_get_rs2json(DB_0, "{CALL supplier_get(?)}", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

// --- Customers Handler & Schema ---

static const FieldValidator CustomersSchema[] = {
    {.field_name = "filter", .type = TYPE_STRING, .is_required = false, .max_len = 10}
};

const ValidationContext CustomersContext = {
    .schema = CustomersSchema,
    .schema_count = sizeof(CustomersSchema) / sizeof(CustomersSchema[0]),
    .global_validator = nullptr
};

void customers_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    
    const char* filter = json_get_string(body, "filter");
    
    QueryParam params[] = {
        { .type = PARAM_STRING, .value = filter }
    };
    
    if (!odbcutil_get_json(DB_0, "{CALL sp_customers_like(?)}", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

void sales_pgsql_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    
    const char* start_date = json_get_string(body, "start_date");
    const char* end_date = json_get_string(body, "end_date");
    QueryParam params[] = {
        { .type = PARAM_STRING, .value = start_date },
        { .type = PARAM_STRING, .value = end_date }
    };
    
    if (!odbcutil_get_json(DB_1, "select get_sales_by_category(?, ?)", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

// --- Upload Handler & Schema ---



static const FieldValidator UploadSchema[] = {
    {.field_name = "filename", .type = TYPE_STRING, .is_required = true, .max_len = 250},
    {.field_name = "content_type", .type = TYPE_STRING, .is_required = true, .max_len = 250},
    {.field_name = "title", .type = TYPE_STRING, .is_required = true, .max_len = 250},
    {.field_name = "blob", .type = TYPE_STRING, .is_required = true}
};

const ValidationContext UploadContext = {
    .schema = UploadSchema,
    .schema_count = sizeof(UploadSchema) / sizeof(UploadSchema[0]),
    .global_validator = nullptr
};


void upload_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    const char* filename = json_get_string(body, "filename");
    const char* content_type = json_get_string(body, "content_type");
    const char* title = json_get_string(body, "title");
    const char* blob = json_get_string(body, "blob");
    const char* user = context_get_user();

    *out_status = HTTP_INTERNAL;
    
    char uuid_str[37];
    generate_uuidv4(uuid_str);
    
    char uploads_dir[MAX_CONFIG_STR];
    config_get_uploads_dir(uploads_dir, sizeof(uploads_dir));
    
    if (!upload_is_valid_dir(uploads_dir)) {
        return;
    }
    
    char out_path[MAX_CONFIG_STR + 64];
    (void)snprintf(out_path, sizeof(out_path), "%s/%s", uploads_dir, uuid_str);
    
    size_t b64_len = strlen(blob);
    size_t bin_maxlen;
    if (ckd_mul(&bin_maxlen, b64_len / 4, 3) || ckd_add(&bin_maxlen, bin_maxlen, 4)) {
        LOG_ERROR("Upload blob length overflow");
        return;
    }
    [[gnu::cleanup(cleanup_free)]] unsigned char* bin_buf = malloc(bin_maxlen);
    if (!bin_buf) {
        LOG_ERROR("Out of memory allocating %zu bytes for upload blob", bin_maxlen);
        return;
    }
    
    size_t bin_len = 0;
    if (sodium_base642bin(bin_buf, bin_maxlen, blob, b64_len, nullptr, &bin_len, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0) {
        LOG_WARN("User %s provided invalid base64 encoding for upload", user ? user : "anonymous");
        const char* msg = "{\"error\":\"Invalid base64 encoding\"}";
        *out_status = HTTP_BADREQUEST;
        evbuffer_add(out_buf, msg, strlen(msg));
        return;
    }
    
    [[gnu::cleanup(cleanup_file)]] FILE* fp = fopen(out_path, "wb");
    if (!fp) {
        char errbuf[256];
        LOG_ERROR("Failed to open upload destination %s for writing: %s", out_path, strerror_r(errno, errbuf, sizeof(errbuf)));
        return;
    }
    
    size_t written = fwrite(bin_buf, 1, bin_len, fp);
    if (written != bin_len) {
        char errbuf[256];
        LOG_ERROR("Failed to write full %zu bytes to %s: %s", bin_len, out_path, strerror_r(errno, errbuf, sizeof(errbuf)));
        return;
    }
    
    LOG_AUDIT("User %s uploaded file %s (Title: %s, Content-Type: %s, Size: %zu bytes) saved as %s", 
              user ? user : "anonymous", filename, title, content_type, bin_len, uuid_str);
              
    *out_status = HTTP_OK;
    char response[256];
    int len = snprintf(response, sizeof(response), "{\"status\":\"ok\",\"uuid\":\"%s\",\"size\":%zu}", uuid_str, bin_len);
    evbuffer_add(out_buf, response, len < (int)sizeof(response) ? (size_t)len : sizeof(response) - 1);
}

// --- Gasto Handler & Schema ---

static const FieldValidator GastoSchema[] = {
    {.field_name = "id", .type = TYPE_INT, .is_required = true, .has_min = true, .min_int = 1}
};

const ValidationContext GastoContext = {
    .schema = GastoSchema,
    .schema_count = sizeof(GastoSchema) / sizeof(GastoSchema[0]),
    .global_validator = nullptr
};

void gasto_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    int id = json_get_int(body, "id");
    
    QueryParam in_params[1];
    in_params[0].type = PARAM_INT;
    in_params[0].value = &id;

    OutParam out_params[4];
    
    char fecha[11];
    out_params[0].name = "fecha";
    out_params[0].buffer = fecha;
    out_params[0].buffer_len = sizeof(fecha);
    out_params[0].type = PARAM_STRING;
    
    int categ_id;
    out_params[1].name = "categ_id";
    out_params[1].buffer = &categ_id;
    out_params[1].buffer_len = sizeof(int);
    out_params[1].type = PARAM_INT;
    
    double monto;
    out_params[2].name = "monto";
    out_params[2].buffer = &monto;
    out_params[2].buffer_len = sizeof(double);
    out_params[2].type = PARAM_DOUBLE;
    
    char motivo[151];
    out_params[3].name = "motivo";
    out_params[3].buffer = motivo;
    out_params[3].buffer_len = sizeof(motivo);
    out_params[3].type = PARAM_STRING;

    SpParams sp_params = {
        .in_params = in_params,
        .in_count = ARRAY_SIZE(in_params),
        .out_params = out_params,
        .out_count = ARRAY_SIZE(out_params)
    };

    if (!odbcutil_execute_sp_json(DB_0, "{CALL dbo.ObtenerGasto(?, ?, ?, ?, ?)}", 
                                  &sp_params, out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

// --- Customer Get Handler ---

void customerget_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    const char* id = json_get_string(body, "id");
    
    QueryParam in_params[1];
    in_params[0].type = PARAM_STRING;
    in_params[0].value = id;

    if (!odbcutil_get_jsonm(DB_0, "{CALL dbo.get_customer_info(?)}", in_params, ARRAY_SIZE(in_params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}
// --- Employee Orders Handler & Schema ---



void emp_orders_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    
    int id = json_get_int(body, "id");
    
    QueryParam params[] = {
        { .type = PARAM_INT, .value = &id }
    };
    
    if (!odbcutil_get_rs2json(DB_1, "SELECT * FROM demo.get_employee_orders(?)", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}

void emp_ordersj_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    
    int id = json_get_int(body, "id");
    
    QueryParam params[] = {
        { .type = PARAM_INT, .value = &id }
    };
    
    if (!odbcutil_get_json(DB_1, "SELECT demo.get_employee_orders_j(?)", params, ARRAY_SIZE(params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}


// --- Dummy Handler & Schema ---

static const FieldValidator DummySchema[] = {
    {.field_name = "id", .type = TYPE_INT, .is_required = true}
};

const ValidationContext DummyContext = {
    .schema = DummySchema,
    .schema_count = sizeof(DummySchema) / sizeof(DummySchema[0]),
    .global_validator = nullptr
};

void dummy_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK;
    
    int in_param1 = json_get_int(body, "id");

    QueryParam in_params[1];
    in_params[0].type = PARAM_INT;
    in_params[0].value = &in_param1;

    int my_out1 = 0;
    char my_out3[64] = {0};

    OutParam out_params[2] = {
        { .name = "CapturedOut1", .buffer = &my_out1, .buffer_len = sizeof(my_out1), .type = PARAM_INT },
        { .name = "CapturedOut3", .buffer = my_out3, .buffer_len = sizeof(my_out3), .type = PARAM_STRING }
    };

    const char* query = 
        "DECLARE @Out1 INT;\n"
        "DECLARE @Out3 DATETIME;\n"
        "EXEC dbo.TestDummySP @InParam1=?, @OutParam1=@Out1 OUTPUT, @OutParam3=@Out3 OUTPUT;\n"
        "SELECT @Out1 AS OutParam1, @Out3 AS OutParam3;";

    if (!odbcutil_query_single_row_j(DB_0, query, in_params, ARRAY_SIZE(in_params), out_params, ARRAY_SIZE(out_params), out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
    }
}
