#include "jwt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sodium.h>
#include <json-c/json.h>

static unsigned char b64_lookup(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return 255;
}

void generate_uuidv4(char out[37]) {
    unsigned char bytes[16];
    
    // 1. Generate 16 cryptographically secure random bytes
    randombytes_buf(bytes, sizeof(bytes));

    // 2. Set the version to 4 (0b0100xxxx)
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    
    // 3. Set the variant to RFC 4122 (0b10xxxxxx)
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    // 4. Format into the standard 36-character UUID string (plus null terminator)
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

char* jwt_decode_payload(const char* jwt) {
    const char* dot1 = strchr(jwt, '.');
    if (!dot1) return nullptr;
    const char* payload_start = dot1 + 1;
    const char* dot2 = strchr(payload_start, '.');
    if (!dot2) return nullptr;
    
    size_t len = (size_t)(dot2 - payload_start);
    size_t out_maxlen = (len * 3) / 4 + 3;
    char* out = malloc(out_maxlen);
    if (!out) return nullptr;
    
    size_t i = 0, j = 0;
    while (i < len) {
        unsigned int n = 0;
        int chars = 0;
        for (int k = 0; k < 4; ++k) {
            n <<= 6;
            if (i < len && payload_start[i] != '=') {
                unsigned char val = b64_lookup((unsigned char)payload_start[i]);
                if (val == 255) {
                    free(out);
                    return nullptr;
                }
                n |= val;
                chars++;
                i++;
            } else if (i < len && payload_start[i] == '=') {
                i++; // Skip padding to prevent infinite loop
            }
        }
        if (chars > 1 && j < out_maxlen - 1) out[j++] = (char)((n >> 16) & 0xFF);
        if (chars > 2 && j < out_maxlen - 1) out[j++] = (char)((n >> 8) & 0xFF);
        if (chars > 3 && j < out_maxlen - 1) out[j++] = (char)(n & 0xFF);
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

char* jwt_create(const char* username, const char* session_id, const char* secret_hex, long timeout_seconds) {
    if (!username || !session_id || !secret_hex) return nullptr;
    
    unsigned char secret_bytes[crypto_auth_hmacsha256_KEYBYTES];
    size_t secret_bin_len = 0;
    if (sodium_hex2bin(secret_bytes, sizeof(secret_bytes), secret_hex, strlen(secret_hex), nullptr, &secret_bin_len, nullptr) != 0) {
        return nullptr; // Invalid hex or wrong length
    }
    
    const char* header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    
    struct json_object* payload_obj = json_object_new_object();
    json_object_object_add(payload_obj, "username", json_object_new_string(username));
    json_object_object_add(payload_obj, "sessionId", json_object_new_string(session_id));
    json_object_object_add(payload_obj, "exp", json_object_new_int64((int64_t)(time(nullptr) + timeout_seconds)));
    const char* payload = json_object_to_json_string_ext(payload_obj, JSON_C_TO_STRING_PLAIN);
    
    size_t header_b64_max = sodium_base64_ENCODED_LEN(strlen(header), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    size_t payload_b64_max = sodium_base64_ENCODED_LEN(strlen(payload), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    
    char* header_b64 = malloc(header_b64_max);
    char* payload_b64 = malloc(payload_b64_max);
    
    if (!header_b64 || !payload_b64) {
        free(header_b64); free(payload_b64); json_object_put(payload_obj); 
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return nullptr;
    }
    
    sodium_bin2base64(header_b64, header_b64_max, (const unsigned char*)header, strlen(header), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    sodium_bin2base64(payload_b64, payload_b64_max, (const unsigned char*)payload, strlen(payload), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    
    size_t msg_len = strlen(header_b64) + 1 + strlen(payload_b64) + 1;
    char* msg = malloc(msg_len);
    if (!msg) {
        free(header_b64); free(payload_b64); json_object_put(payload_obj); 
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return nullptr;
    }
    snprintf(msg, msg_len, "%s.%s", header_b64, payload_b64);
    
    unsigned char mac[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256(mac, (const unsigned char*)msg, strlen(msg), secret_bytes);
    
    size_t mac_b64_max = sodium_base64_ENCODED_LEN(sizeof(mac), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    char* mac_b64 = malloc(mac_b64_max);
    if (!mac_b64) {
        free(header_b64); free(payload_b64); free(msg); json_object_put(payload_obj); 
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return nullptr;
    }
    sodium_bin2base64(mac_b64, mac_b64_max, mac, sizeof(mac), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    
    size_t jwt_len = strlen(msg) + 1 + strlen(mac_b64) + 1;
    char* jwt = malloc(jwt_len);
    if (jwt) {
        snprintf(jwt, jwt_len, "%s.%s", msg, mac_b64);
    }
    
    free(header_b64);
    free(payload_b64);
    free(msg);
    free(mac_b64);
    json_object_put(payload_obj);
    
    sodium_memzero(secret_bytes, sizeof(secret_bytes));
    return jwt;
}

int jwt_verify(const char* token, const char* secret_hex, char* out_username, size_t out_uname_size, char* out_session_id, size_t out_sess_size) {
    if (!token || !secret_hex) return JWT_ERR_INVALID;

    const char* dot1 = strchr(token, '.');
    if (!dot1) return JWT_ERR_INVALID;
    const char* dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return JWT_ERR_INVALID;

    size_t msg_len = (size_t)(dot2 - token);
    
    unsigned char secret_bytes[crypto_auth_hmacsha256_KEYBYTES];
    size_t secret_bin_len = 0;
    if (sodium_hex2bin(secret_bytes, sizeof(secret_bytes), secret_hex, strlen(secret_hex), nullptr, &secret_bin_len, nullptr) != 0) {
        return JWT_ERR_INVALID; 
    }

    unsigned char mac[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256(mac, (const unsigned char*)token, msg_len, secret_bytes);
    sodium_memzero(secret_bytes, sizeof(secret_bytes));

    size_t mac_b64_max = sodium_base64_ENCODED_LEN(sizeof(mac), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    char mac_b64[128];
    sodium_bin2base64(mac_b64, mac_b64_max, mac, sizeof(mac), sodium_base64_VARIANT_URLSAFE_NO_PADDING);

    const char* signature = dot2 + 1;
    if (strlen(signature) != strlen(mac_b64) || sodium_memcmp(signature, mac_b64, strlen(mac_b64)) != 0) {
        return JWT_ERR_INVALID;
    }

    char* payload_json = jwt_decode_payload(token);
    if (!payload_json) return JWT_ERR_INVALID;

    int ret = JWT_ERR_INVALID;
    struct json_object* jwt_obj = json_tokener_parse(payload_json);
    if (jwt_obj) {
        struct json_object* exp_obj;
        if (json_object_object_get_ex(jwt_obj, "exp", &exp_obj)) {
            time_t exp = (time_t)json_object_get_int64(exp_obj);
            if (time(nullptr) > exp) {
                ret = JWT_ERR_EXPIRED;
            } else {
                ret = JWT_OK;
            }
        }
        
        if (ret == JWT_OK) {
            struct json_object* sub_obj;
            if (json_object_object_get_ex(jwt_obj, "username", &sub_obj) && out_username) {
                snprintf(out_username, out_uname_size, "%s", json_object_get_string(sub_obj));
            }
            struct json_object* jti_obj;
            if (json_object_object_get_ex(jwt_obj, "sessionId", &jti_obj) && out_session_id) {
                snprintf(out_session_id, out_sess_size, "%s", json_object_get_string(jti_obj));
            }
        }
        json_object_put(jwt_obj);
    }
    free(payload_json);
    return ret;
}
