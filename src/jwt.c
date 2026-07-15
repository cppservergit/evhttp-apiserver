#include "jwt.h"
#include <string.h>
#include <stdlib.h>
#include <json-c/json.h>

static unsigned char b64_lookup(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return 255;
}

char* jwt_decode_payload(const char* jwt) {
    const char* dot1 = strchr(jwt, '.');
    if (!dot1) return NULL;
    const char* payload_start = dot1 + 1;
    const char* dot2 = strchr(payload_start, '.');
    if (!dot2) return NULL;
    
    size_t len = (size_t)(dot2 - payload_start);
    size_t out_len = (len * 3) / 4 + 1;
    char* out = malloc(out_len);
    if (!out) return NULL;
    
    size_t i = 0, j = 0;
    while (i < len) {
        unsigned int n = 0;
        int chars = 0;
        for (int k = 0; k < 4; ++k) {
            n <<= 6;
            if (i < len && payload_start[i] != '=') {
                n |= b64_lookup((unsigned char)payload_start[i]);
                chars++;
                i++;
            } else if (i < len && payload_start[i] == '=') {
                i++; // Skip padding to prevent infinite loop
            }
        }
        if (chars > 1) out[j++] = (char)((n >> 16) & 0xFF);
        if (chars > 2) out[j++] = (char)((n >> 8) & 0xFF);
        if (chars > 3) out[j++] = (char)(n & 0xFF);
    }
    out[j] = '\0';
    return out;
}

time_t jwt_get_expiration(const char* jwt) {
    char* payload_json = jwt_decode_payload(jwt);
    if (!payload_json) return 0;
    
    time_t exp = 0;
    struct json_object* jwt_obj = json_tokener_parse(payload_json);
    if (jwt_obj) {
        struct json_object* exp_obj;
        if (json_object_object_get_ex(jwt_obj, "exp", &exp_obj)) {
            exp = (time_t)json_object_get_int64(exp_obj);
        }
        json_object_put(jwt_obj);
    }
    free(payload_json);
    return exp;
}
