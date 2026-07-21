#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <json-c/json.h>
#include "http_client.h"
#include "customer.h"
#include "logger.h"
#include "config.h"

#include "jwt.h"

struct jwt_cache {
    char token[1024];
    time_t expires_at;
};

static struct jwt_cache global_jwt_cache = {0};
static pthread_rwlock_t jwt_cache_lock = PTHREAD_RWLOCK_INITIALIZER;

static struct jwt_cache* get_jwt_cache(void) {
    return &global_jwt_cache;
}

static struct json_object* execute_login_request(void) {
    char api_user[MAX_CONFIG_STR], api_pass[MAX_CONFIG_STR], api_url[MAX_CONFIG_STR];
    config_get_api_user(api_user, sizeof(api_user));
    config_get_api_pass(api_pass, sizeof(api_pass));
    config_get_api_url(api_url, sizeof(api_url));
    
    char body_str[512];
    int len = snprintf(body_str, sizeof(body_str), "{\"username\":\"%s\",\"password\":\"%s\"}", api_user, api_pass);
    if (len >= (int)sizeof(body_str)) {
        LOG_ERROR("Login payload truncated");
        return nullptr;
    }
    
    const char* headers[] = {"Content-Type: application/json"};
    long http_code = 0;
    
    struct json_object* login_response = http_client_post_json(api_url, "/api/login", body_str, headers, 1, &http_code);
    
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
                LOG_ERROR("Remote token exceeds %zu bytes", sizeof(cache->token) - 1);
                return;
            }
            snprintf(cache->token, sizeof(cache->token), "%s", id_token);
            time_t exp = jwt_get_expiration(id_token);
            cache->expires_at = exp > 0 ? exp : time(nullptr) + 180;
        }
    }
}

static bool login_and_get_token(char* out_token, size_t max_len) {
    time_t now = time(nullptr);
    struct jwt_cache* cache = get_jwt_cache();
    
    pthread_rwlock_rdlock(&jwt_cache_lock);
    if (cache->token[0] != '\0' && now < cache->expires_at) {
        snprintf(out_token, max_len, "%s", cache->token);
        pthread_rwlock_unlock(&jwt_cache_lock);
        return true;
    }
    pthread_rwlock_unlock(&jwt_cache_lock);
    
    // Cache miss - acquire write lock to prevent thundering herd
    pthread_rwlock_wrlock(&jwt_cache_lock);
    
    // Double-checked locking
    now = time(nullptr);
    if (cache->token[0] != '\0' && now < cache->expires_at) {
        snprintf(out_token, max_len, "%s", cache->token);
        pthread_rwlock_unlock(&jwt_cache_lock);
        return true;
    }
    
    LOG_WARN("[customer] JWT cache miss (token missing or expired). Requesting a fresh token...");
    
    cache->token[0] = '\0';
    
    struct json_object* login_response = execute_login_request();
    if (!login_response) {
        pthread_rwlock_unlock(&jwt_cache_lock);
        return false;
    }
    
    update_jwt_cache(cache, login_response);
    json_object_put(login_response);
    
    bool success = false;
    if (cache->token[0] != '\0') {
        snprintf(out_token, max_len, "%s", cache->token);
        success = true;
    }
    pthread_rwlock_unlock(&jwt_cache_lock);
    
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
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);
    
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
        pthread_rwlock_wrlock(&jwt_cache_lock);
        get_jwt_cache()->token[0] = '\0';
        pthread_rwlock_unlock(&jwt_cache_lock);
    }
    
    return remote_response;
}
