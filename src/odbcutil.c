#include "odbcutil.h"
#include "raii.h"
#include "logger.h"
#include "server.h"
#include "config.h"
#include <string.h>

#include <pthread.h>

static _Thread_local SQLHDBC tl_hdbc = SQL_NULL_HDBC;
static _Thread_local SQLHSTMT tl_hstmt = SQL_NULL_HSTMT;
static pthread_key_t tl_hdbc_key;
static pthread_once_t tl_hdbc_key_once = PTHREAD_ONCE_INIT;

static void tl_hdbc_destructor(void* arg) {
    SQLHDBC hdbc = (SQLHDBC)arg;
    if (tl_hstmt != SQL_NULL_HSTMT) {
        SQLFreeHandle(SQL_HANDLE_STMT, tl_hstmt);
        tl_hstmt = SQL_NULL_HSTMT;
    }
    if (hdbc != SQL_NULL_HDBC) {
        SQLDisconnect(hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    }
}

static void make_tl_hdbc_key(void) {
    pthread_key_create(&tl_hdbc_key, tl_hdbc_destructor);
}

void odbcutil_reset_connection(void) {
    if (tl_hstmt != SQL_NULL_HSTMT) {
        SQLFreeHandle(SQL_HANDLE_STMT, tl_hstmt);
        tl_hstmt = SQL_NULL_HSTMT;
    }
    if (tl_hdbc != SQL_NULL_HDBC) {
        SQLDisconnect(tl_hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, tl_hdbc);
        tl_hdbc = SQL_NULL_HDBC;
        pthread_setspecific(tl_hdbc_key, NULL);
    }
}

void odbcutil_log_error(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg) {
    SQLCHAR sqlState[6], msg[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER nativeError;
    SQLSMALLINT msgLen;

    if (SQL_SUCCEEDED(SQLGetDiagRec(handle_type, handle, 1, sqlState, &nativeError, msg, sizeof(msg), &msgLen))) {
        LOG_ERROR("%s | ODBC Error [%s]: %s", context_msg, sqlState, msg);
        if (strncmp((char*)sqlState, "08S01", 5) == 0 || strncmp((char*)sqlState, "08003", 5) == 0) {
            LOG_ERROR("Database connection lost. Resetting thread-local connection pool.");
            odbcutil_reset_connection();
        }
    } else {
        LOG_ERROR("%s | Unknown ODBC Error", context_msg);
    }
}

SQLHDBC odbcutil_connect(void) {
    if (tl_hdbc != SQL_NULL_HDBC) {
        return tl_hdbc;
    }
    
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, server_get_odbc_env(), &tl_hdbc))) {
        odbcutil_log_error(SQL_HANDLE_ENV, server_get_odbc_env(), "Failed to allocate ODBC connection handle");
        return SQL_NULL_HDBC;
    }
    
    // Set a 5-second login timeout
    SQLSetConnectAttr(tl_hdbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);
    
    char conn_str[MAX_CONFIG_STR];
    config_get_odbc_conn_str(conn_str, sizeof(conn_str));
    
    SQLCHAR out_conn_str[MAX_ODBC_CONN_STR_LEN];
    SQLSMALLINT out_conn_len;
    if (!SQL_SUCCEEDED(SQLDriverConnect(tl_hdbc, nullptr, (SQLCHAR*)conn_str, SQL_NTS, out_conn_str, sizeof(out_conn_str), &out_conn_len, SQL_DRIVER_NOPROMPT))) {
        odbcutil_log_error(SQL_HANDLE_DBC, tl_hdbc, "Failed to connect to database");
        odbcutil_reset_connection();
        return SQL_NULL_HDBC;
    }
    
    pthread_once(&tl_hdbc_key_once, make_tl_hdbc_key);
    pthread_setspecific(tl_hdbc_key, (void*)tl_hdbc);
    
    return tl_hdbc;
}

SQLHSTMT odbcutil_alloc_stmt(SQLHDBC hdbc, const char* func_name) {
    if (tl_hstmt != SQL_NULL_HSTMT) {
        return tl_hstmt;
    }

    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) {
        char context_msg[256];
        snprintf(context_msg, sizeof(context_msg), "Failed to allocate ODBC statement handle in %s", func_name);
        odbcutil_log_error(SQL_HANDLE_DBC, hdbc, context_msg);
        odbcutil_reset_connection();
        return SQL_NULL_HSTMT;
    }
    
    // Set a 5-second timeout on all database queries so worker threads never hang indefinitely
    SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)5, 0);
    
    tl_hstmt = hstmt;
    return hstmt;
}

void odbcutil_disconnect(SQLHDBC hdbc, SQLHSTMT hstmt) {
    (void)hdbc; // Connection remains pooled persistently in tl_hdbc
    if (hstmt != SQL_NULL_HSTMT) {
        SQLFreeStmt(hstmt, SQL_CLOSE);        // Close open cursors
        SQLFreeStmt(hstmt, SQL_RESET_PARAMS); // Clear bound parameters
        SQLFreeStmt(hstmt, SQL_UNBIND);       // Clear bound columns
    }
}

bool odbcutil_fetch_json_native(SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf) {
    SQLRETURN ret;
    char chunk[ODBC_FETCH_CHUNK_SIZE];
    SQLLEN indicator;
    
    bool has_rows = false;
    bool has_data_written = false;
    bool fetch_success = true;

    while (SQL_SUCCEEDED(ret = SQLFetch(hstmt))) {
        has_rows = true;
                
        while (true) {
            ret = SQLGetData(hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator);
            
            if (ret == SQL_ERROR) {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), "SQLGetData failed for %s", func_name);
                odbcutil_log_error(SQL_HANDLE_STMT, hstmt, err_msg);
                return false;
            }
            
            if (indicator == SQL_NULL_DATA) {
                if (!has_data_written) {
                    evbuffer_add(out_buf, "null", 4);
                    has_data_written = true;
                }
                break;
            }

            if (ret == SQL_NO_DATA) {
                break;
            }
            
            size_t bytes_to_write = 0;
            size_t max_payload = sizeof(chunk) - 1;

            if (indicator == SQL_NO_TOTAL || indicator >= (SQLLEN)max_payload) {
                bytes_to_write = max_payload;
            } else if (indicator >= 0) {
                bytes_to_write = (size_t)indicator;
            } else {
                bytes_to_write = strlen(chunk);
            }
            
            if (bytes_to_write > 0) {
                evbuffer_add(out_buf, chunk, bytes_to_write);
                has_data_written = true;
            }
            
            if (ret == SQL_SUCCESS) {
                break;
            }
        }
    }
    
    if (ret != SQL_NO_DATA && !SQL_SUCCEEDED(ret)) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "SQLFetch failed for %s", func_name);
        odbcutil_log_error(SQL_HANDLE_STMT, hstmt, err_msg);
        fetch_success = false;
    }
    
    if (!has_rows) {
        evbuffer_add(out_buf, "[]", 2);
    }
    
    return fetch_success;
}


bool odbcutil_get_json(const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name) {
    SQLHDBC hdbc = odbcutil_connect();
    if (hdbc == SQL_NULL_HDBC) return false;
    
    SQLHSTMT hstmt = odbcutil_alloc_stmt(hdbc, func_name);
    if (!hstmt) return false;
    
    for (size_t i = 0; i < param_count; ++i) {
        SQLRETURN bind_ret;
        SQLUSMALLINT param_idx = (SQLUSMALLINT)(i + 1);

        switch (params[i].type) {
            case PARAM_STRING: {
                const char *str = (const char *)params[i].value;
                params[i].ind = str ? SQL_NTS : SQL_NULL_DATA;
                bind_ret = SQLBindParameter(hstmt, param_idx, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                       str ? strlen(str) : 0, 0, (SQLPOINTER)str, 0, &params[i].ind);
                break;
            }
            case PARAM_INT: {
                params[i].ind = params[i].value ? 0 : SQL_NULL_DATA;
                bind_ret = SQLBindParameter(hstmt, param_idx, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
                                       0, 0, (SQLPOINTER)params[i].value, 0, &params[i].ind);
                break;
            }
            case PARAM_DOUBLE: {
                params[i].ind = params[i].value ? 0 : SQL_NULL_DATA;
                bind_ret = SQLBindParameter(hstmt, param_idx, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE,
                                       0, 0, (SQLPOINTER)params[i].value, 0, &params[i].ind);
                break;
            }
            case PARAM_NULL: {
                params[i].ind = SQL_NULL_DATA;
                bind_ret = SQLBindParameter(hstmt, param_idx, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                       0, 0, nullptr, 0, &params[i].ind);
                break;
            }
            default:
                odbcutil_log_error(SQL_HANDLE_STMT, hstmt, "Unsupported parameter type");
                odbcutil_disconnect(hdbc, hstmt);
                return false;
        }

        if (!SQL_SUCCEEDED(bind_ret)) {
            odbcutil_log_error(SQL_HANDLE_STMT, hstmt, "Failed to bind parameter");
            odbcutil_disconnect(hdbc, hstmt);
            return false;
        }
    }
    
    bool success = false;
    
    if (SQL_SUCCEEDED(SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS))) {
        success = odbcutil_fetch_json_native(hstmt, func_name, out_buf);
    } else {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Failed to execute SQLExecDirect in %s", func_name);
        odbcutil_log_error(SQL_HANDLE_STMT, hstmt, err_msg);
    }
    
    odbcutil_disconnect(hdbc, hstmt);
    return success;
}
