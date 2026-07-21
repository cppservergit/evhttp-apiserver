#include "odbcutil.h"
#include "raii.h"
#include "logger.h"
#include "server.h"
#include "config.h"
#include <string.h>

#include <pthread.h>

static _Thread_local SQLHDBC tl_hdbc = SQL_NULL_HDBC;
static pthread_key_t tl_hdbc_key;
static pthread_once_t tl_hdbc_key_once = PTHREAD_ONCE_INIT;

static void tl_hdbc_destructor(void* arg) {
    SQLHDBC hdbc = (SQLHDBC)arg;
    if (hdbc != SQL_NULL_HDBC) {
        SQLDisconnect(hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    }
}

static void make_tl_hdbc_key(void) {
    pthread_key_create(&tl_hdbc_key, tl_hdbc_destructor);
}

void odbcutil_reset_connection(void) {
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

    if (SQLGetDiagRec(handle_type, handle, 1, sqlState, &nativeError, msg, sizeof(msg), &msgLen) == SQL_SUCCESS) {
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
        SQLUINTEGER dead = SQL_CD_FALSE;
        SQLGetConnectAttr(tl_hdbc, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr);
        if (dead == SQL_CD_FALSE) {
            return tl_hdbc;
        }
        odbcutil_reset_connection();
    }
    
    if (SQLAllocHandle(SQL_HANDLE_DBC, server_get_odbc_env(), &tl_hdbc) != SQL_SUCCESS) {
        odbcutil_log_error(SQL_HANDLE_ENV, server_get_odbc_env(), "Failed to allocate ODBC connection handle");
        return SQL_NULL_HDBC;
    }
    
    // Set a 5-second login timeout
    SQLSetConnectAttr(tl_hdbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);
    
    char conn_str[MAX_CONFIG_STR];
    config_get_odbc_conn_str(conn_str, sizeof(conn_str));
    
    SQLCHAR out_conn_str[MAX_ODBC_CONN_STR_LEN];
    SQLSMALLINT out_conn_len;
    SQLRETURN ret = SQLDriverConnect(tl_hdbc, nullptr, (SQLCHAR*)conn_str, SQL_NTS, out_conn_str, sizeof(out_conn_str), &out_conn_len, SQL_DRIVER_NOPROMPT);
    
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        odbcutil_log_error(SQL_HANDLE_DBC, tl_hdbc, "Failed to connect to database");
        odbcutil_reset_connection();
        return SQL_NULL_HDBC;
    }
    
    pthread_once(&tl_hdbc_key_once, make_tl_hdbc_key);
    pthread_setspecific(tl_hdbc_key, (void*)tl_hdbc);
    
    return tl_hdbc;
}

SQLHSTMT odbcutil_alloc_stmt(SQLHDBC hdbc, const char* func_name) {
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt) != SQL_SUCCESS) {
        char context_msg[256];
        snprintf(context_msg, sizeof(context_msg), "Failed to allocate ODBC statement handle in %s", func_name);
        odbcutil_log_error(SQL_HANDLE_DBC, hdbc, context_msg);
        odbcutil_reset_connection();
        return SQL_NULL_HSTMT;
    }
    
    // Set a 5-second timeout on all database queries so worker threads never hang indefinitely
    SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)5, 0);
    
    return hstmt;
}

void odbcutil_disconnect(SQLHDBC hdbc, SQLHSTMT hstmt) {
    (void)hdbc; // Connection remains pooled persistently in tl_hdbc
    if (hstmt != SQL_NULL_HSTMT) {
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    }
}

bool odbcutil_fetch_json_batch(SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf) {
    SQLRETURN ret;
    char chunk[ODBC_FETCH_CHUNK_SIZE];
    SQLLEN indicator;
    
    bool has_rows = false;
    bool fetch_success = true;

    while ((ret = SQLFetch(hstmt)) == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        has_rows = true;
        
        while (true) {
            ret = SQLGetData(hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator);
            
            if (ret == SQL_NO_DATA || indicator == SQL_NULL_DATA) {
                break;
            }
            
            if (ret == SQL_ERROR) {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), "SQLGetData failed for %s", func_name);
                odbcutil_log_error(SQL_HANDLE_STMT, hstmt, err_msg);
                return false;
            }
            
            size_t chunk_len = strnlen(chunk, sizeof(chunk) - 1);
            if (chunk_len > 0) {
                evbuffer_add(out_buf, chunk, chunk_len);
            }
            
            if (ret == SQL_SUCCESS) {
                break;
            }
        }
    }
    
    if (ret != SQL_NO_DATA && ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
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


bool odbcutil_get_json(const char* query, odbc_bind_fn binder, struct json_object* body, struct evbuffer* out_buf, const char* func_name) {
    SQLHDBC hdbc = odbcutil_connect();
    if (hdbc == SQL_NULL_HDBC) return false;
    
    SQLHSTMT hstmt = odbcutil_alloc_stmt(hdbc, func_name);
    if (!hstmt) return false;
    
    if (binder) {
        binder(body, hstmt);
    }
    
    SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS);
    bool success = false;
    
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        success = odbcutil_fetch_json_batch(hstmt, func_name, out_buf);
    } else {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Failed to execute SQLExecDirect in %s", func_name);
        odbcutil_log_error(SQL_HANDLE_STMT, hstmt, err_msg);
    }
    
    odbcutil_disconnect(hdbc, hstmt);
    return success;
}
