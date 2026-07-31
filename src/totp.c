#include "totp.h"
#include "odbcutil.h"
#include "thread_error.h"
#include <qrencode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>
#include <stdbool.h>
#include <liboath/oath.h>
#include <time.h>
#include <event2/http.h>

static int get_secret(const char* user, char* out_secret, size_t max_len) {
    SQLHDBC hdbc = odbcutil_connect(DB_0);
    if (hdbc == SQL_NULL_HDBC) return HTTP_INTERNAL;
    
    SQLHSTMT hstmt = odbcutil_alloc_stmt(DB_0, hdbc, __func__);
    if (!hstmt) {
        odbcutil_disconnect(hdbc, nullptr);
        return HTTP_INTERNAL;
    }
    
    SQLLEN cbUser = SQL_NTS;
    SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 0, 0, (SQLPOINTER)user, 0, &cbUser);
    
    SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)"{CALL cpp_get_secret(?)}", SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        odbcutil_set_error(DB_0, SQL_HANDLE_STMT, hstmt, "Failed to execute cpp_get_secret");
        odbcutil_disconnect(hdbc, hstmt);
        return HTTP_INTERNAL;
    }
    
    SQLRETURN fetch_ret = SQLFetch(hstmt);
    if (fetch_ret != SQL_SUCCESS && fetch_ret != SQL_SUCCESS_WITH_INFO) {
        odbcutil_disconnect(hdbc, hstmt);
        return HTTP_NOTFOUND;
    }
    
    SQLLEN len = 0;
    SQLRETURN get_ret = SQLGetData(hstmt, 1, SQL_C_CHAR, out_secret, (SQLLEN)max_len, &len);
    
    int status = HTTP_NOTFOUND;
    if (get_ret == SQL_SUCCESS) {
        if (len != SQL_NULL_DATA && len > 0) status = HTTP_OK;
    } else if (get_ret == SQL_SUCCESS_WITH_INFO) {
        odbcutil_set_error(DB_0, SQL_HANDLE_STMT, hstmt, "Secret fetch truncated or warning");
    }
    
    odbcutil_disconnect(hdbc, hstmt);
    return status;
}

static void totp_append_qr_svg_path(struct evbuffer* out_buf, QRcode *qrcode) {
    char path_buf[128];
    for (int y = 0; y < qrcode->width; y++) {
        for (int x = 0; x < qrcode->width; x++) {
            if (qrcode->data[y * qrcode->width + x] & 1) {
                int len = snprintf(path_buf, sizeof(path_buf), "M%d,%dh1v1h-1z ", x + 1, y + 1);
                evbuffer_add(out_buf, path_buf, len < (int)sizeof(path_buf) ? (size_t)len : sizeof(path_buf) - 1);
            }
        }
    }
}



void totp_generate_svg(const char* user, int* out_status, struct evbuffer* out_buf) {
    if (!user) {
        *out_status = HTTP_BADREQUEST;
        return;
    }

    char secret[128] = {0};
    int db_status = get_secret(user, secret, sizeof(secret));
    
    if (db_status != HTTP_OK) {
        *out_status = db_status;
        return;
    }

    char* escaped_user = evhttp_uriencode(user, -1, 0);
    if (!escaped_user) {
        *out_status = HTTP_INTERNAL;
        sodium_memzero(secret, sizeof(secret));
        return;
    }

    char uri[512];
    (void)snprintf(uri, sizeof(uri), "otpauth://totp/apiserver:%s?secret=%s&issuer=apiserver&algorithm=SHA256&digits=6&period=30", escaped_user, secret);

    QRcode *qrcode = QRcode_encodeString(uri, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    
    sodium_memzero(secret, sizeof(secret));
    sodium_memzero(escaped_user, strlen(escaped_user));
    free(escaped_user);
    sodium_memzero(uri, sizeof(uri));

    if (!qrcode) {
        *out_status = HTTP_INTERNAL;
        return;
    }

    char svg_start[256];
    int len = snprintf(svg_start, sizeof(svg_start), 
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" viewBox=\"0 0 %d %d\">\n"
        "<rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n"
        "<path d=\"", 
        qrcode->width + 2, qrcode->width + 2,
        qrcode->width + 2, qrcode->width + 2);
    evbuffer_add(out_buf, svg_start, len < (int)sizeof(svg_start) ? (size_t)len : sizeof(svg_start) - 1);
    
    totp_append_qr_svg_path(out_buf, qrcode);
    
    const char* svg_end = "\" fill=\"black\"/>\n</svg>\n";
    evbuffer_add(out_buf, svg_end, strlen(svg_end));

    QRcode_free(qrcode);

    *out_status = HTTP_OK;
}

bool is_valid_totp(const char* username, const char* totp_code) {
    if (!username || !totp_code) return false;

    char secret_b32[128] = {0};
    if (get_secret(username, secret_b32, sizeof(secret_b32)) != HTTP_OK) {
        return false;
    }

    char* secret_bin = NULL;
    size_t secret_bin_len = 0;
    
    int decode_ret = oath_base32_decode(secret_b32, strlen(secret_b32), &secret_bin, &secret_bin_len);
    if (decode_ret != OATH_OK) {
        sodium_memzero(secret_b32, sizeof(secret_b32));
        return false;
    }

    int validate_ret = oath_totp_validate4(secret_bin, secret_bin_len, time(NULL), 30, 0, 2, NULL, NULL, OATH_TOTP_HMAC_SHA256, totp_code);
    
    sodium_memzero(secret_b32, sizeof(secret_b32));
    sodium_memzero(secret_bin, secret_bin_len);
    free(secret_bin);
    
    return (validate_ret >= 0);
}

bool totp_generate_base32_secret(char* out_secret, size_t out_maxlen) {
    if (!out_secret || out_maxlen != 33) return false;

    unsigned char random_bytes[20];
    randombytes_buf(random_bytes, sizeof(random_bytes));

    char* b32 = NULL;
    size_t b32_len = 0;
    int enc_ret = oath_base32_encode((const char*)random_bytes, sizeof(random_bytes), &b32, &b32_len);
    
    sodium_memzero(random_bytes, sizeof(random_bytes));

    if (enc_ret != OATH_OK || !b32) {
        return false;
    }

    if (b32_len >= out_maxlen) {
        sodium_memzero(b32, b32_len);
        free(b32);
        return false;
    }

    strncpy(out_secret, b32, out_maxlen);
    out_secret[out_maxlen - 1] = '\0';

    sodium_memzero(b32, b32_len);
    free(b32);

    return true;
}
