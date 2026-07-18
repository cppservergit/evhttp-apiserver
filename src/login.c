#include "login.h"
#include "http_client.h"
#include "config.h"
#include <stdio.h>

struct json_object* login_service_authenticate(const char* username, const char* password, long* out_http_code) {
    char provider[MAX_CONFIG_STR];
    char uri[MAX_CONFIG_STR];

    config_get_login_provider(provider, sizeof(provider));
    config_get_login_uri(uri, sizeof(uri));

    struct json_object* payload = json_object_new_object();
    json_object_object_add(payload, "username", json_object_new_string(username));
    json_object_object_add(payload, "password", json_object_new_string(password));
    const char* body_str = json_object_to_json_string(payload);

    const char* headers[] = {"Content-Type: application/json"};
    struct json_object* result = http_client_post_json(provider, uri, body_str, headers, 1, out_http_code);

    json_object_put(payload);
    return result;
}
