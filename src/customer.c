#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <json-c/json.h>
#include "http_client.h"
#include "http_client.h"
#include "customer.h"
#include "logger.h"
#include "config.h"

#include "jwt.h"

struct jwt_cache {
    char* token;
    time_t expires_at;
};

static pthread_key_t g_jwt_tls_key;
static pthread_once_t g_jwt_tls_once = PTHREAD_ONCE_INIT;

static void jwt_cache_destructor(void* val) {
    struct jwt_cache* cache = (struct jwt_cache*)val;
    if (cache) {
        if (cache->token) free(cache->token);
        free(cache);
    }
}

static void jwt_init_tls_key(void) {
    pthread_key_create(&g_jwt_tls_key, jwt_cache_destructor);
}

static struct jwt_cache* get_jwt_cache(void) {
    pthread_once(&g_jwt_tls_once, jwt_init_tls_key);
    struct jwt_cache* cache = (struct jwt_cache*)pthread_getspecific(g_jwt_tls_key);
    if (!cache) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
        cache = calloc(1, sizeof(struct jwt_cache));
        if (!cache) {
            LOG_FATAL("Out of memory allocating jwt_cache");
            return nullptr;
        }
        pthread_setspecific(g_jwt_tls_key, cache);
#pragma GCC diagnostic pop
    }
    return cache;
}

static const char* login_and_get_token(void) {
    time_t now = time(nullptr);
    struct jwt_cache* cache = get_jwt_cache();
    if (!cache) return nullptr;
    
    // Return cached token if valid
    if (cache->token && now < cache->expires_at) {
        return cache->token;
    }
    
    LOG_WARN("[customer] JWT cache miss (token missing or expired). Requesting a fresh token...");
    
    if (cache->token) {
        free(cache->token);
        cache->token = nullptr;
    }
    
    char api_user[MAX_CONFIG_STR];
    char api_pass[MAX_CONFIG_STR];
    char api_url[MAX_CONFIG_STR];
    
    config_get_api_user(api_user, sizeof(api_user));
    config_get_api_pass(api_pass, sizeof(api_pass));
    config_get_api_url(api_url, sizeof(api_url));
    
    struct json_object* login_payload = json_object_new_object();
    json_object_object_add(login_payload, "username", json_object_new_string(api_user));
    json_object_object_add(login_payload, "password", json_object_new_string(api_pass));
    
    const char* body = json_object_to_json_string(login_payload);
    const char* headers[] = {"Content-Type: application/json"};
    long http_code = 0;
    
    char url[MAX_CONFIG_STR + 64];
    snprintf(url, sizeof(url), "%s/api/login", api_url);
    
    struct json_object* login_response = http_client_post_json(url, body, headers, 1, &http_code);
    json_object_put(login_payload);
    
    if (http_code != 200 || !login_response) {
        if (login_response) json_object_put(login_response);
        return nullptr;
    }
    
    struct json_object* token_obj;
    if (json_object_object_get_ex(login_response, "id_token", &token_obj)) {
        const char* id_token = json_object_get_string(token_obj);
        if (id_token) {
            cache->token = strdup(id_token);
            if (cache->token) {
                time_t exp = jwt_get_expiration(id_token);
                if (exp > 0) {
                    cache->expires_at = exp;
                } else {
                    cache->expires_at = now + 180;
                }
            } else {
                LOG_FATAL("Out of memory in strdup for id_token");
            }
        }
    }
    
    json_object_put(login_response);
    return cache->token;
}

struct json_object* customer_service_get_info(const char* customer_id, long* out_http_code) {
    const char* token = login_and_get_token();
    if (!token) {
        if (out_http_code) *out_http_code = 401;
        return nullptr;
    }
    
    struct json_object* req_payload = json_object_new_object();
    json_object_object_add(req_payload, "id", json_object_new_string(customer_id));
    const char* body = json_object_to_json_string(req_payload);
    
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);
    
    const char* headers[] = {
        auth_header,
        "Content-Type: application/json"
    };
    
    char api_url[MAX_CONFIG_STR];
    config_get_api_url(api_url, sizeof(api_url));
    
    char url[MAX_CONFIG_STR + 64];
    snprintf(url, sizeof(url), "%s/api/customer", api_url);
    
    struct json_object* remote_response = http_client_post_json(url, body, headers, 2, out_http_code);
    json_object_put(req_payload);
    
    // Invalidate token cache on auth failure
    if (out_http_code && *out_http_code == 401) {
        struct jwt_cache* cache = get_jwt_cache();
        if (cache && cache->token) {
            free(cache->token);
            cache->token = nullptr;
        }
    }
    
    return remote_response;
}
