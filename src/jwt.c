#include <apiserver/jwt.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sodium.h>
#include <json-c/json.h>
#include <apiserver/json_util.h>
#include <apiserver/raii.h>

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
    
    size_t bin_len = 0;
    if (sodium_base642bin((unsigned char*)out, out_maxlen - 1, start, len, NULL, &bin_len, NULL, sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        return false;
    }
    
    out[bin_len] = '\0';
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
    [[gnu::cleanup(cleanup_json_object)]] struct json_object* jwt_obj = json_tokener_parse(payload_json);
    if (jwt_obj) {
        struct json_object* exp_obj;
        if (json_object_object_get_ex(jwt_obj, "exp", &exp_obj)) {
            exp = json_object_get_int64(exp_obj);
        }
    }
    sodium_memzero(payload_json, sizeof(payload_json));
    return exp;
}

static bool get_secret_bytes(const char* secret_hex, unsigned char* out_bytes) {
    size_t secret_bin_len = 0;
    if (sodium_hex2bin(out_bytes, crypto_auth_hmacsha256_KEYBYTES, secret_hex, strlen(secret_hex), nullptr, &secret_bin_len, nullptr) != 0) {
        return false;
    }
    return secret_bin_len == crypto_auth_hmacsha256_KEYBYTES;
}

static bool jwt_build_payload(const char* username, const char* session_id, long timeout_seconds, char* payload, size_t payload_max) {
    char escaped_user[128];
    json_encode_string(username, escaped_user, sizeof(escaped_user));
    time_t now = time(nullptr);
    int p_len = snprintf(payload, payload_max, "{\"username\":\"%s\",\"sessionId\":\"%s\",\"iat\":%lld,\"exp\":%lld}", 
                         escaped_user, session_id, (long long)now, (long long)(now + timeout_seconds));
    return p_len >= 0 && p_len < (int)payload_max;
}

static bool jwt_sign_msg(const char* msg, const unsigned char* secret_bytes, char* out_jwt, size_t out_jwt_size) {
    unsigned char mac[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256(mac, (const unsigned char*)msg, strlen(msg), secret_bytes);
    
    char mac_b64[128];
    size_t mac_b64_max = sodium_base64_ENCODED_LEN(sizeof(mac), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    if (mac_b64_max > sizeof(mac_b64)) return false;
    
    sodium_bin2base64(mac_b64, sizeof(mac_b64), mac, sizeof(mac), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    
    int final_len = snprintf(out_jwt, out_jwt_size, "%s.%s", msg, mac_b64);
    return final_len >= 0 && final_len < (int)out_jwt_size;
}

bool jwt_create(const char* username, const char* session_id, const char* secret_hex, long timeout_seconds, char* out_jwt, size_t out_jwt_size) {
    if (!username || !session_id || !secret_hex || !out_jwt || out_jwt_size == 0) return false;
    
    unsigned char secret_bytes[crypto_auth_hmacsha256_KEYBYTES];
    if (!get_secret_bytes(secret_hex, secret_bytes)) return false;
    
    const char* header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    char payload[512];
    if (!jwt_build_payload(username, session_id, timeout_seconds, payload, sizeof(payload))) {
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return false;
    }
    
    char header_b64[128];
    char payload_b64[512];
    if (sodium_base64_ENCODED_LEN(strlen(header), sodium_base64_VARIANT_URLSAFE_NO_PADDING) > sizeof(header_b64) ||
        sodium_base64_ENCODED_LEN(strlen(payload), sodium_base64_VARIANT_URLSAFE_NO_PADDING) > sizeof(payload_b64)) {
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return false;
    }

    sodium_bin2base64(header_b64, sizeof(header_b64), (const unsigned char*)header, strlen(header), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    sodium_bin2base64(payload_b64, sizeof(payload_b64), (const unsigned char*)payload, strlen(payload), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    
    char msg[768];
    int msg_len = snprintf(msg, sizeof(msg), "%s.%s", header_b64, payload_b64);
    if (msg_len >= (int)sizeof(msg) || !jwt_sign_msg(msg, secret_bytes, out_jwt, out_jwt_size)) {
        sodium_memzero(secret_bytes, sizeof(secret_bytes));
        return false;
    }
    
    sodium_memzero(secret_bytes, sizeof(secret_bytes));
    return true;
}

static bool jwt_check_header_alg(const char* token) {
    char header_json[1024];
    if (!jwt_decode_header(token, header_json, sizeof(header_json))) return false;
    
    [[gnu::cleanup(cleanup_json_object)]] struct json_object* header_obj = json_tokener_parse(header_json);
    if (!header_obj) return false;
    
    struct json_object* alg_obj;
    if (!json_object_object_get_ex(header_obj, "alg", &alg_obj)) return false;
    
    return (strcmp(json_object_get_string(alg_obj), "HS256") == 0);
}

static bool jwt_verify_mac(const char* msg, size_t msg_len, const char* secret_hex, const char* signature) {
    unsigned char secret_bytes[crypto_auth_hmacsha256_KEYBYTES];
    if (!get_secret_bytes(secret_hex, secret_bytes)) return false;

    unsigned char mac[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256(mac, (const unsigned char*)msg, msg_len, secret_bytes);
    sodium_memzero(secret_bytes, sizeof(secret_bytes));

    unsigned char provided_mac[crypto_auth_hmacsha256_BYTES];
    size_t provided_len = 0;
    if (sodium_base642bin(provided_mac, sizeof(provided_mac), signature, strlen(signature),
                          NULL, &provided_len, NULL,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        return false;
    }
    
    if (provided_len != crypto_auth_hmacsha256_BYTES) {
        return false;
    }
    
    int match = sodium_memcmp(mac, provided_mac, sizeof(mac));
    sodium_memzero(provided_mac, sizeof(provided_mac));
    
    return (match == 0);
}

static int jwt_parse_payload(const char* payload_json, char* out_username, size_t out_uname_size, char* out_session_id, size_t out_sess_size) {
    [[gnu::cleanup(cleanup_json_object)]] struct json_object* jwt_obj = json_tokener_parse(payload_json);
    if (!jwt_obj) return JWT_ERR_INVALID;

    struct json_object* exp_obj;
    if (!json_object_object_get_ex(jwt_obj, "exp", &exp_obj)) return JWT_ERR_INVALID;
    
    time_t exp = json_object_get_int64(exp_obj);
    if (time(nullptr) > exp) return JWT_ERR_EXPIRED;

    if (out_username) {
        struct json_object* sub_obj;
        if (!json_object_object_get_ex(jwt_obj, "username", &sub_obj)) return JWT_ERR_INVALID;
        const char* uname = json_object_get_string(sub_obj);
        if (!uname || strlen(uname) >= out_uname_size) return JWT_ERR_INVALID;
        (void)snprintf(out_username, out_uname_size, "%s", uname);
    }
    
    if (out_session_id) {
        struct json_object* jti_obj;
        if (!json_object_object_get_ex(jwt_obj, "sessionId", &jti_obj)) return JWT_ERR_INVALID;
        const char* sess = json_object_get_string(jti_obj);
        if (!sess || strlen(sess) >= out_sess_size) return JWT_ERR_INVALID;
        (void)snprintf(out_session_id, out_sess_size, "%s", sess);
    }

    return JWT_OK;
}

int jwt_verify(const char* token, const char* secret_hex, char* out_username, size_t out_uname_size, char* out_session_id, size_t out_sess_size) {
    if (!token || !secret_hex) return JWT_ERR_INVALID;

    const char* dot1 = strchr(token, '.');
    if (!dot1) return JWT_ERR_INVALID;
    const char* dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return JWT_ERR_INVALID;
    if (strchr(dot2 + 1, '.') != NULL) return JWT_ERR_INVALID;

    size_t msg_len = (size_t)(dot2 - token);
    
    if (!jwt_check_header_alg(token)) return JWT_ERR_INVALID;
    if (!jwt_verify_mac(token, msg_len, secret_hex, dot2 + 1)) return JWT_ERR_INVALID;

    char payload_json[8192];
    if (!jwt_decode_payload(token, payload_json, sizeof(payload_json))) return JWT_ERR_INVALID;
    
    int result = jwt_parse_payload(payload_json, out_username, out_uname_size, out_session_id, out_sess_size);
    sodium_memzero(payload_json, sizeof(payload_json));
    return result;
}
