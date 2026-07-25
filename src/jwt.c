#include "jwt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sodium.h>
#include <json-c/json.h>
#include "json_util.h"

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
    (void)snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

static bool b64_decode_segment(const char* start, size_t len, char* out, size_t out_maxlen) {
    if (!out || out_maxlen == 0) return false;
    
    size_t i = 0, j = 0;
    while (i < len) {
        unsigned int n = 0;
        int chars = 0;
        for (int k = 0; k < 4; ++k) {
            n <<= 6;
            if (i < len && start[i] != '=') {
                unsigned char val = b64_lookup((unsigned char)start[i]);
                if (val == 255) {
                    return false;
                }
                n |= val;
                chars++;
                i++;
            } else if (i < len && start[i] == '=') {
                i++; // Skip padding to prevent infinite loop
            }
        }
        if (chars > 1 && j < out_maxlen - 1) out[j++] = (char)((n >> 16) & 0xFF);
        if (chars > 2 && j < out_maxlen - 1) out[j++] = (char)((n >> 8) & 0xFF);
        if (chars > 3 && j < out_maxlen - 1) out[j++] = (char)(n & 0xFF);
    }
    out[j] = '\0';
    return true;
}

static bool jwt_decode_header(const char* jwt, char* out, size_t out_maxlen) {
    const char* dot1 = strchr(jwt, '.');
    if (!dot1) return false;
    return b64_decode_segment(jwt, (size_t)(dot1 - jwt), out, out_maxlen);
}

bool jwt_decode_payload(const char* jwt, char* out, size_t out_maxlen) {
    const char* dot1 = strchr(jwt, '.');
    if (!dot1) return false;
    const char* payload_start = dot1 + 1;
    const char* dot2 = strchr(payload_start, '.');
    if (!dot2) return false;
    
    return b64_decode_segment(payload_start, (size_t)(dot2 - payload_start), out, out_maxlen);
}

time_t jwt_get_expiration(const char* jwt) {
    char payload_json[8192];
    if (!jwt_decode_payload(jwt, payload_json, sizeof(payload_json))) return 0;
    
    time_t exp = 0;
    struct json_object* jwt_obj = json_tokener_parse(payload_json);
    if (jwt_obj) {
        struct json_object* exp_obj;
        if (json_object_object_get_ex(jwt_obj, "exp", &exp_obj)) {
            exp = (time_t)json_object_get_int64(exp_obj);
        }
        json_object_put(jwt_obj);
    }
    return exp;
}

bool jwt_create(const char* username, const char* session_id, const char* secret_hex, long timeout_seconds, char* out_jwt, size_t out_jwt_size) {
    if (!username || !session_id || !secret_hex || !out_jwt || out_jwt_size == 0) return false;
    
    unsigned char secret_bytes[crypto_auth_hmacsha256_KEYBYTES];
    size_t secret_bin_len = 0;
    if (sodium_hex2bin(secret_bytes, sizeof(secret_bytes), secret_hex, strlen(secret_hex), nullptr, &secret_bin_len, nullptr) != 0) {
        return false; // Invalid hex or wrong length
    }
    if (secret_bin_len != crypto_auth_hmacsha256_KEYBYTES) {
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return false;
    }
    
    const char* header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    
    char escaped_user[128];
    json_encode_string(username, escaped_user, sizeof(escaped_user));
    
    char payload[512];
    int p_len = snprintf(payload, sizeof(payload), "{\"username\":\"%s\",\"sessionId\":\"%s\",\"exp\":%lld}", 
                         escaped_user, session_id, (long long)(time(nullptr) + timeout_seconds));
    if (p_len >= (int)sizeof(payload)) {
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return false;
    }
    
    size_t header_b64_max = sodium_base64_ENCODED_LEN(strlen(header), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    size_t payload_b64_max = sodium_base64_ENCODED_LEN(strlen(payload), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    
    char header_b64[128];
    char payload_b64[512];
    char mac_b64[128];

    if (header_b64_max > sizeof(header_b64) || payload_b64_max > sizeof(payload_b64)) {
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return false;
    }

    sodium_bin2base64(header_b64, sizeof(header_b64), (const unsigned char*)header, strlen(header), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    sodium_bin2base64(payload_b64, sizeof(payload_b64), (const unsigned char*)payload, strlen(payload), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    
    char msg[768];
    int msg_len = snprintf(msg, sizeof(msg), "%s.%s", header_b64, payload_b64);
    if (msg_len >= (int)sizeof(msg)) {
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return false;
    }
    
    unsigned char mac[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256(mac, (const unsigned char*)msg, strlen(msg), secret_bytes);
    
    size_t mac_b64_max = sodium_base64_ENCODED_LEN(sizeof(mac), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    if (mac_b64_max > sizeof(mac_b64)) {
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return false;
    }
    sodium_bin2base64(mac_b64, sizeof(mac_b64), mac, sizeof(mac), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    
    int final_len = snprintf(out_jwt, out_jwt_size, "%s.%s", msg, mac_b64);
    if (final_len >= (int)out_jwt_size) {
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return false;
    }
    
    sodium_memzero(secret_bytes, sizeof(secret_bytes));
    return true;
}

int jwt_verify(const char* token, const char* secret_hex, char* out_username, size_t out_uname_size, char* out_session_id, size_t out_sess_size) {
    if (!token || !secret_hex) return JWT_ERR_INVALID;

    const char* dot1 = strchr(token, '.');
    if (!dot1) return JWT_ERR_INVALID;
    const char* dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return JWT_ERR_INVALID;

    size_t msg_len = (size_t)(dot2 - token);
    
    // Verify the Header explicitly to reject non-HS256 algorithms early and save CPU cycles
    char header_json[1024];
    if (!jwt_decode_header(token, header_json, sizeof(header_json))) return JWT_ERR_INVALID;
    
    struct json_object* header_obj = json_tokener_parse(header_json);
    if (!header_obj) return JWT_ERR_INVALID;
    
    struct json_object* alg_obj;
    if (!json_object_object_get_ex(header_obj, "alg", &alg_obj) || 
        strcmp(json_object_get_string(alg_obj), "HS256") != 0) {
        json_object_put(header_obj);
        return JWT_ERR_INVALID;
    }
    json_object_put(header_obj);

    // Compute HMAC
    unsigned char secret_bytes[crypto_auth_hmacsha256_KEYBYTES];
    size_t secret_bin_len = 0;
    if (sodium_hex2bin(secret_bytes, sizeof(secret_bytes), secret_hex, strlen(secret_hex), nullptr, &secret_bin_len, nullptr) != 0) {
        return JWT_ERR_INVALID; 
    }
    if (secret_bin_len != crypto_auth_hmacsha256_KEYBYTES) {
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
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

    char payload_json[8192];
    if (!jwt_decode_payload(token, payload_json, sizeof(payload_json))) return JWT_ERR_INVALID;

    // Tokens without an 'exp' claim will explicitly fail because ret initializes to JWT_ERR_INVALID
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
                (void)snprintf(out_username, out_uname_size, "%s", json_object_get_string(sub_obj));
            }
            struct json_object* jti_obj;
            if (json_object_object_get_ex(jwt_obj, "sessionId", &jti_obj) && out_session_id) {
                (void)snprintf(out_session_id, out_sess_size, "%s", json_object_get_string(jti_obj));
            }
        }
        json_object_put(jwt_obj);
    }
    return ret;
}
