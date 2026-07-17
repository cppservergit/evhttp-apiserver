#include "totp.h"
#include "odbcutil.h"
#include "logger.h"
#include <qrencode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int get_secret(const char* user, char* out_secret, size_t max_len) {
    SQLHDBC hdbc = odbcutil_connect();
    if (hdbc == SQL_NULL_HDBC) return 500;
    
    SQLHSTMT hstmt = odbcutil_alloc_stmt(hdbc, __func__);
    if (!hstmt) {
        odbcutil_disconnect(hdbc, NULL);
        return 500;
    }
    
    SQLLEN cbUser = SQL_NTS;
    SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 0, 0, (SQLPOINTER)user, 0, &cbUser);
    
    SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)"{CALL cpp_get_secret(?)}", SQL_NTS);
    
    int status = 404;
    
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        if (SQLFetch(hstmt) == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
            SQLLEN len = 0;
            SQLGetData(hstmt, 1, SQL_C_CHAR, out_secret, max_len, &len);
            if (len != SQL_NULL_DATA && len > 0) {
                status = 200;
            }
        }
    } else {
        odbcutil_log_error(SQL_HANDLE_STMT, hstmt, "Failed to execute cpp_get_secret");
        status = 500;
    }
    
    odbcutil_disconnect(hdbc, hstmt);
    return status;
}

struct evbuffer* totp_generate_svg(const char* user, int* out_status, const char** out_status_txt) {
    if (!user) {
        *out_status = 400;
        *out_status_txt = "Bad Request";
        return NULL;
    }

    char secret[128] = {0};
    int db_status = get_secret(user, secret, sizeof(secret));
    
    if (db_status != 200) {
        *out_status = db_status;
        *out_status_txt = (db_status == 404) ? "Not Found" : "Internal Server Error";
        return NULL;
    }

    char uri[1024];
    snprintf(uri, sizeof(uri), "otpauth://totp/APIServer2:%s?secret=%s&issuer=APIServer2", user, secret);

    QRcode *qrcode = QRcode_encodeString(uri, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (!qrcode) {
        *out_status = 500;
        *out_status_txt = "Internal Server Error";
        return NULL;
    }

    struct evbuffer* buf = evbuffer_new();
    
    evbuffer_add_printf(buf, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    evbuffer_add_printf(buf, "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" viewBox=\"0 0 %d %d\">\n", qrcode->width + 2, qrcode->width + 2);
    evbuffer_add_printf(buf, "<rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", qrcode->width + 2, qrcode->width + 2);
    evbuffer_add_printf(buf, "<path d=\"");
    
    for (int y = 0; y < qrcode->width; y++) {
        for (int x = 0; x < qrcode->width; x++) {
            if (qrcode->data[y * qrcode->width + x] & 1) {
                evbuffer_add_printf(buf, "M%d,%dh1v1h-1z ", x + 1, y + 1);
            }
        }
    }
    
    evbuffer_add_printf(buf, "\" fill=\"black\"/>\n");
    evbuffer_add_printf(buf, "</svg>\n");

    QRcode_free(qrcode);

    *out_status = 200;
    *out_status_txt = "OK";
    return buf;
}
