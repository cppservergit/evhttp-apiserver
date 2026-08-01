#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <json-c/json.h>
#include "http_client.h"
#include "customer.h"
#include "thread_error.h"
#include "config.h"

#include "jwt.h"
#include <sodium.h>
#include "json_util.h"
struct jwt_cache {
    char token[1024];
    time_t expires_at;
};

static struct jwt_cache global_jwt_cache = {0};
static pthread_mutex_t jwt_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jwt_cache_cond = PTHREAD_COND_INITIALIZER;
static bool jwt_is_refreshing = false;

static struct jwt_cache* get_jwt_cache(void) {
    return &global_jwt_cache;
}

static struct json_object* execute_login_request(void) {
    char api_user[MAX_CONFIG_STR];
    char api_pass[MAX_CONFIG_STR];
    char api_url[MAX_CONFIG_STR];
    config_get_api_user(api_user, sizeof(api_user));
    config_get_api_pass(api_pass, sizeof(api_pass));
    config_get_api_url(api_url, sizeof(api_url));
    
    char escaped_user[256];
    char escaped_pass[1024];
    json_encode_string(api_user, escaped_user, sizeof(escaped_user));
    json_encode_string(api_pass, escaped_pass, sizeof(escaped_pass));

    char body_str[2048];
    int len = snprintf(body_str, sizeof(body_str), "{\"username\":\"%s\",\"password\":\"%s\"}", escaped_user, escaped_pass);
    if (len >= (int)sizeof(body_str)) {
        sodium_memzero(api_pass, sizeof(api_pass));
        sodium_memzero(escaped_pass, sizeof(escaped_pass));
        sodium_memzero(body_str, sizeof(body_str));
        set_thread_error(TL_ERR_ERROR, "Login payload truncated");
        return nullptr;
    }
    
    const char* headers[] = {"Content-Type: application/json"};
    long http_code = 0;
    
    struct json_object* login_response = http_client_post_json(api_url, "/api/login", body_str, headers, 1, &http_code);
    
    sodium_memzero(api_pass, sizeof(api_pass));
    sodium_memzero(escaped_pass, sizeof(escaped_pass));
    sodium_memzero(body_str, sizeof(body_str));
    
    if (http_code != 200 || !login_response) {
        if (login_response) json_object_put(login_response);
        return nullptr;
    }
    return login_response;
}

static void update_jwt_cache(struct jwt_cache* cache, struct json_object* login_response) {
    struct json_object* token_obj;
    if (json_object_object_get_ex(login_response, "id_token", &token_obj)) {
        const char* id_token = json_object_get_string(token_obj);
        if (id_token) {
            if (strlen(id_token) >= sizeof(cache->token)) {
                set_thread_error(TL_ERR_ERROR, "Remote token exceeds %zu bytes", sizeof(cache->token) - 1);
                return;
            }
            (void)snprintf(cache->token, sizeof(cache->token), "%s", id_token);
            time_t exp = jwt_get_expiration(id_token);
            cache->expires_at = exp > 0 ? exp : time(nullptr) + 180;
        }
    }
}

static void copy_token_safely(char* out_token, size_t max_len, const char* src_token) {
    if (max_len == 0) return;
    size_t cpy_len = strlen(src_token);
    if (cpy_len >= max_len) cpy_len = max_len - 1;
    memcpy(out_token, src_token, cpy_len);
    out_token[cpy_len] = '\0';
}

static bool login_and_get_token(char* out_token, size_t max_len) {
    struct jwt_cache* cache = get_jwt_cache();
    
    pthread_mutex_lock(&jwt_cache_lock);
    
    while (1) {
        time_t now = time(nullptr);
        if (cache->token[0] != '\0' && now < cache->expires_at) {
            copy_token_safely(out_token, max_len, cache->token);
            pthread_mutex_unlock(&jwt_cache_lock);
            return true;
        }
        
        if (jwt_is_refreshing) {
            pthread_cond_wait(&jwt_cache_cond, &jwt_cache_lock);
            continue;
        }
        
        jwt_is_refreshing = true;
        break;
    }
    
    pthread_mutex_unlock(&jwt_cache_lock);
    
    set_thread_error(TL_ERR_WARN, "[customer] JWT cache miss (token missing or expired). Requesting a fresh token...");
    
    struct json_object* login_response = execute_login_request();
    
    pthread_mutex_lock(&jwt_cache_lock);
    
    if (login_response) {
        cache->token[0] = '\0';
        update_jwt_cache(cache, login_response);
        json_object_put(login_response);
    }
    
    jwt_is_refreshing = false;
    pthread_cond_broadcast(&jwt_cache_cond);
    
    bool success = false;
    if (cache->token[0] != '\0') {
        copy_token_safely(out_token, max_len, cache->token);
        success = true;
    }
    pthread_mutex_unlock(&jwt_cache_lock);
    
    return success;
}

struct json_object* customer_service_get_info(const char* customer_id, long* out_http_code) {
    char token[1024];
    if (!login_and_get_token(token, sizeof(token))) {
        if (out_http_code) *out_http_code = 401;
        return nullptr;
    }
    
    struct json_object* req_payload = json_object_new_object();
    json_object_object_add(req_payload, "id", json_object_new_string(customer_id));
    const char* body = json_object_to_json_string(req_payload);
    
    char auth_header[1100];
    (void)snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);
    
    const char* headers[] = {
        auth_header,
        "Content-Type: application/json"
    };
    
    char api_url[MAX_CONFIG_STR];
    config_get_api_url(api_url, sizeof(api_url));
    
    struct json_object* remote_response = http_client_post_json(api_url, "/api/customer", body, headers, 2, out_http_code);
    json_object_put(req_payload);
    
    // Invalidate token cache on auth failure
    if (out_http_code && *out_http_code == 401) {
        pthread_mutex_lock(&jwt_cache_lock);
        get_jwt_cache()->token[0] = '\0';
        pthread_mutex_unlock(&jwt_cache_lock);
    }
    
    return remote_response;
}
