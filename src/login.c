#include "login.h"
#include "http_client.h"
#include "config.h"
#include <stdio.h>

struct json_object* login_service_authenticate(const char* username, const char* password, long* out_http_code) {
    char provider[MAX_CONFIG_STR];
    char uri[MAX_CONFIG_STR];
    char payload_fmt[MAX_CONFIG_STR];

    config_get_login_provider(provider, sizeof(provider));
    config_get_login_uri(uri, sizeof(uri));
    config_get_login_payload(payload_fmt, sizeof(payload_fmt));

    char body_str[2048];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    snprintf(body_str, sizeof(body_str), payload_fmt, username, password);
#pragma GCC diagnostic pop

    const char* headers[] = {"Content-Type: application/json"};
    return http_client_post_json(provider, uri, body_str, headers, 1, out_http_code);
}
