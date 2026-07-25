#include "login.h"
#include "http_client.h"
#include "config.h"
#include "json_util.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>

struct json_object* login_service_authenticate(const char* username, const char* password, long* out_http_code) {
    char provider[MAX_CONFIG_STR];
    char uri[MAX_CONFIG_STR];

    config_get_login_provider(provider, sizeof(provider));
    config_get_login_uri(uri, sizeof(uri));

    char escaped_user[256];
    char escaped_pass[1024];
    
    json_encode_string(username, escaped_user, sizeof(escaped_user));
    json_encode_string(password, escaped_pass, sizeof(escaped_pass));

    char body_str[2048];
    (void)snprintf(body_str, sizeof(body_str), "{\"username\":\"%s\",\"password\":\"%s\"}", escaped_user, escaped_pass);

    const char* headers[] = {"Content-Type: application/json"};
    struct json_object* result = http_client_post_json(provider, uri, body_str, headers, 1, out_http_code);

    sodium_memzero(escaped_pass, sizeof(escaped_pass));
    sodium_memzero(body_str, sizeof(body_str));

    return result;
}
