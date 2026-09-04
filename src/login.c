#include "login.h"
#include <apiserver/http_client.h>
#include <apiserver/config.h>
#include <apiserver/json_util.h>
#include <apiserver/thread_error.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

struct json_object* login_service_authenticate(const char* username, const char* password, long* out_http_code) {
    const char* provider = config_get_login_provider();
    const char* uri = config_get_login_uri();

    char escaped_user[JSON_ENCODE_BUFSIZE(96)];
    char escaped_pass[JSON_ENCODE_BUFSIZE(96)];
    
    json_encode_string(username, escaped_user, sizeof(escaped_user));
    json_encode_string(password, escaped_pass, sizeof(escaped_pass));

    char body_str[JSON_ENCODE_BUFSIZE(96) * 2 + 128];
    int len = snprintf(body_str, sizeof(body_str), "{\"username\":\"%s\",\"password\":\"%s\"}", escaped_user, escaped_pass);
    if (len >= (int)sizeof(body_str) || len < 0) {
        sodium_memzero(escaped_pass, sizeof(escaped_pass));
        sodium_memzero(body_str, sizeof(body_str));
        set_thread_error(TL_ERR_ERROR, "Login payload truncation error");
        return nullptr;
    }

    const char* headers[] = {"Content-Type: application/json"};
    struct json_object* result = http_client_post_json(provider, uri, body_str, headers, 1, out_http_code);

    sodium_memzero(escaped_pass, sizeof(escaped_pass));
    sodium_memzero(body_str, sizeof(body_str));

    return result;
}
