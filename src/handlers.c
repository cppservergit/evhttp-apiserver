#include "handlers.h"
#include <assert.h>
#include "server.h"
#include "http_client.h"
#include "customer.h"
#include "customerdb.h"
#include "sales.h"
#include "shippers.h"
#include "products.h"
#include "validation.h"
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>

struct json_object* ping_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt) {
    (void)req; (void)body; (void)arg;
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    struct json_object* root = json_object_new_object();
    if (root != nullptr) {
        json_object_object_add(root, "status", json_object_new_string("OK"));
    }
    return root;
}

struct json_object* version_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt) {
    (void)req; (void)body; (void)arg;
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    struct json_object* root = json_object_new_object();
    if (root != nullptr) {
        json_object_object_add(root, "version", json_object_new_string(get_server_version()));
        json_object_object_add(root, "compiler", json_object_new_string(__VERSION__));
        json_object_object_add(root, "compile_date", json_object_new_string(__DATE__ " " __TIME__));
        json_object_object_add(root, "hostname", json_object_new_string(server_get_hostname()));
        json_object_object_add(root, "os_version", json_object_new_string(server_get_os_version()));
    }
    return root;
}

struct json_object* sysinfo_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt) {
    (void)req; (void)body; (void)arg;
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    uint64_t total_requests = 0;
    uint64_t avg_time_ms = 0;
    server_get_request_stats(&total_requests, nullptr, &avg_time_ms);
    
    uint64_t total_ram_kb = 0;
    uint64_t mem_usage_kb = 0;
    server_get_memory_stats(&total_ram_kb, &mem_usage_kb);
    double mem_usage_pct = total_ram_kb > 0 ? ((double)mem_usage_kb / (double)total_ram_kb) * 100.0 : 0.0;
    
    struct json_object* root = json_object_new_object();
    if (root != nullptr) {
        json_object_object_add(root, "start_time", json_object_new_string(server_get_start_time()));
        json_object_object_add(root, "total_requests", json_object_new_int64((int64_t)total_requests));
        json_object_object_add(root, "average_processing_time_ms", json_object_new_int64((int64_t)avg_time_ms));
        json_object_object_add(root, "total_ram_kb", json_object_new_int64((int64_t)total_ram_kb));
        json_object_object_add(root, "memory_usage_kb", json_object_new_int64((int64_t)mem_usage_kb));
        
        char pct_str[32];
        snprintf(pct_str, sizeof(pct_str), "%.2f", mem_usage_pct);
        json_object_object_add(root, "memory_usage_percentage", json_object_new_double_s(mem_usage_pct, pct_str));
        
        json_object_object_add(root, "hostname", json_object_new_string(server_get_hostname()));
    }
    return root;
}

struct evbuffer* metrics_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt) {
    (void)req; (void)body; (void)arg;
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    uint64_t total_requests = 0;
    uint64_t total_time_ms = 0;
    server_get_request_stats(&total_requests, &total_time_ms, nullptr);
    
    uint64_t total_ram_kb = 0;
    uint64_t mem_usage_kb = 0;
    server_get_memory_stats(&total_ram_kb, &mem_usage_kb);
    
    struct evbuffer* buf = evbuffer_new();
    if (buf == nullptr) return nullptr;
    
    evbuffer_add_printf(buf,
        "# HELP microservice_requests_total Total number of processed requests\n"
        "# TYPE microservice_requests_total counter\n"
        "microservice_requests_total %lu\n\n"
        "# HELP microservice_processing_time_milliseconds_total Total processing time across all requests\n"
        "# TYPE microservice_processing_time_milliseconds_total counter\n"
        "microservice_processing_time_milliseconds_total %lu\n\n"
        "# HELP microservice_memory_usage_bytes Current resident memory size in bytes\n"
        "# TYPE microservice_memory_usage_bytes gauge\n"
        "microservice_memory_usage_bytes %lu\n\n"
        "# HELP microservice_memory_total_bytes Total physical memory in bytes\n"
        "# TYPE microservice_memory_total_bytes gauge\n"
        "microservice_memory_total_bytes %lu\n",
        total_requests,
        total_time_ms,
        mem_usage_kb * 1024,
        total_ram_kb * 1024
    );
    
    return buf;
}

struct json_object* rsysinfo_handler(struct evhttp_request* req, struct json_object* body, void* arg, int* out_status, const char** out_status_txt) {
    (void)req; (void)body; (void)arg;
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";

    const char* headers[] = {
        "Authorization: Bearer 6976f434-d9c1-11f0-93b8-5254000f64af"
    };

    long http_code = 0;
    struct json_object* remote_json = http_client_get_json("https://cppserver.com/api/metrics", headers, 1, &http_code);

    struct json_object* root = json_object_new_object();
    json_object_object_add(root, "remote_status", json_object_new_int((int32_t)http_code));
    
    if (remote_json) {
        json_object_object_add(root, "remote_data", remote_json);
    } else {
        json_object_object_add(root, "remote_data", json_object_new_null());
    }

    return root;
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
    .global_validator = NULL
};

struct json_object* customer_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    [[maybe_unused]] const char** out_status_txt
) {
    const char *customer_id = json_get_string(body, "id");
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    long http_code = 0;
    struct json_object* remote_json = customer_service_get_info(customer_id, &http_code);
    
    struct json_object* root = json_object_new_object();
    json_object_object_add(root, "remote_status", json_object_new_int((int32_t)http_code));
    
    if (remote_json) {
        json_object_object_add(root, "remote_data", remote_json);
    } else {
        json_object_object_add(root, "remote_data", json_object_new_null());
    }
    
    return root;
}

struct json_object* customer_get_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    [[maybe_unused]] const char** out_status_txt
) {
    const char *customer_id = json_get_string(body, "id");
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    return customerdb_get_data(customer_id);
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

    // Defensive Assertions: Document and enforce engine invariants explicitly
    assert(start_str != nullptr);
    assert(end_str != nullptr);

    if (strcmp(start_str, end_str) >= 0) {
        return emit_error(err_buf, err_len, ERR_START_AFTER_END, nullptr);
    }

    return true;
}

static const FieldValidator SalesSchema[] = {
    {.field_name = "start_date", .type = TYPE_DATE, .is_required = true, .custom_validator = NULL},
    {.field_name = "end_date",   .type = TYPE_DATE, .is_required = true, .custom_validator = NULL}
};

const ValidationContext SalesContext = {
    .schema = SalesSchema,
    .schema_count = sizeof(SalesSchema) / sizeof(SalesSchema[0]),
    .global_validator = validate_start_before_end
};

struct json_object* sales_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    [[maybe_unused]] const char** out_status_txt
) {
    const char* start_date = json_get_string(body, "start_date");
    const char* end_date = json_get_string(body, "end_date");
    
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    return sales_service_get_data(start_date, end_date);
}

struct json_object* shippers_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    [[maybe_unused]] const char** out_status_txt
) {
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    return shippers_get_data();
}

struct json_object* products_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    [[maybe_unused]] int* out_status, 
    [[maybe_unused]] const char** out_status_txt
) {
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    return products_get_data();
}
