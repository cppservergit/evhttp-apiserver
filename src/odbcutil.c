#include "odbcutil.h"
#include "raii.h"
#include "logger.h"
#include "server.h"
#include "config.h"
#include <string.h>

void odbcutil_log_error(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg) {
    SQLCHAR sqlState[6], msg[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER nativeError;
    SQLSMALLINT msgLen;

    if (SQLGetDiagRec(handle_type, handle, 1, sqlState, &nativeError, msg, sizeof(msg), &msgLen) == SQL_SUCCESS) {
        LOG_ERROR("%s | ODBC Error [%s]: %s", context_msg, sqlState, msg);
    } else {
        LOG_ERROR("%s | Unknown ODBC Error", context_msg);
    }
}

SQLHDBC odbcutil_connect(void) {
    SQLHDBC hdbc = SQL_NULL_HDBC;
    if (SQLAllocHandle(SQL_HANDLE_DBC, server_get_odbc_env(), &hdbc) != SQL_SUCCESS) {
        odbcutil_log_error(SQL_HANDLE_ENV, server_get_odbc_env(), "Failed to allocate ODBC connection handle");
        return SQL_NULL_HDBC;
    }
    
    // Set a 5-second login timeout
    SQLSetConnectAttr(hdbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);
    
    char conn_str[MAX_CONFIG_STR];
    config_get_odbc_conn_str(conn_str, sizeof(conn_str));
    
    SQLCHAR out_conn_str[MAX_ODBC_CONN_STR_LEN];
    SQLSMALLINT out_conn_len;
    SQLRETURN ret = SQLDriverConnect(hdbc, NULL, (SQLCHAR*)conn_str, SQL_NTS, out_conn_str, sizeof(out_conn_str), &out_conn_len, SQL_DRIVER_NOPROMPT);
    
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        odbcutil_log_error(SQL_HANDLE_DBC, hdbc, "Failed to connect to database");
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        return SQL_NULL_HDBC;
    }
    return hdbc;
}

SQLHSTMT odbcutil_alloc_stmt(SQLHDBC hdbc, const char* func_name) {
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt) != SQL_SUCCESS) {
        char context_msg[256];
        snprintf(context_msg, sizeof(context_msg), "Failed to allocate ODBC statement handle in %s", func_name);
        odbcutil_log_error(SQL_HANDLE_DBC, hdbc, context_msg);
        SQLDisconnect(hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        return SQL_NULL_HSTMT;
    }
    
    // Set a 5-second timeout on all database queries so worker threads never hang indefinitely
    SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)5, 0);
    
    return hstmt;
}

void odbcutil_disconnect(SQLHDBC hdbc, SQLHSTMT hstmt) {
    if (hstmt != SQL_NULL_HSTMT) {
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    }
    if (hdbc != SQL_NULL_HDBC) {
        SQLDisconnect(hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    }
}

struct json_object* odbcutil_fetch_json(SQLHSTMT hstmt) {
    struct json_object* result_json = nullptr;
    SQLRETURN ret;
    
    raii_json_tokener tok = json_tokener_new();
    if (tok) {
        bool has_rows = false;
        while ((ret = SQLFetch(hstmt)) == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
            has_rows = true;
            char chunk[ODBC_FETCH_CHUNK_SIZE];
            bool has_more_chunks = true;
            while (has_more_chunks) {
                SQLLEN indicator = 0;
                ret = SQLGetData(hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator);
                
                if (ret == SQL_NO_DATA) {
                    has_more_chunks = false;
                } else if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
                    if (indicator != SQL_NULL_DATA) {
                        // Stream chunk directly into the JSON state machine, no intermediate buffers
                        struct json_object* parsed_obj = json_tokener_parse_ex(tok, chunk, (int)strlen(chunk));
                        if (parsed_obj) {
                            result_json = parsed_obj;
                        }
                        has_more_chunks = (ret == SQL_SUCCESS_WITH_INFO);
                    } else {
                        has_more_chunks = false;
                    }
                } else {
                    odbcutil_log_error(SQL_HANDLE_STMT, hstmt, "SQLGetData failed during chunk streaming");
                    has_more_chunks = false;
                }
            }
        }
        
        if (ret != SQL_NO_DATA && ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
            odbcutil_log_error(SQL_HANDLE_STMT, hstmt, "SQLFetch failed while iterating rowset");
        }
        
        if (!has_rows || !result_json) {
            result_json = json_tokener_parse("[]");
        }
    }
    
    return result_json;
}

#define BATCH_SIZE 4

struct json_object* odbcutil_fetch_json_batch(SQLHSTMT hstmt) {
    struct json_object* result_json = nullptr;
    SQLRETURN ret;
    
    raii_json_tokener tok = json_tokener_new();
    if (!tok) return nullptr;
    
    char chunks[BATCH_SIZE][ODBC_FETCH_CHUNK_SIZE];
    SQLLEN indicators[BATCH_SIZE];
    SQLUSMALLINT row_status[BATCH_SIZE];
    SQLULEN rows_fetched = 0;
    
    SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_BIND_TYPE, (SQLPOINTER)SQL_BIND_BY_COLUMN, 0);
    SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)BATCH_SIZE, 0);
    SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_STATUS_PTR, row_status, 0);
    SQLSetStmtAttr(hstmt, SQL_ATTR_ROWS_FETCHED_PTR, &rows_fetched, 0);
    
    SQLBindCol(hstmt, 1, SQL_C_CHAR, chunks, ODBC_FETCH_CHUNK_SIZE, indicators);
    
    bool has_rows = false;
    while ((ret = SQLFetchScroll(hstmt, SQL_FETCH_NEXT, 0)) == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        has_rows = true;
        for (SQLULEN i = 0; i < rows_fetched; ++i) {
            if (row_status[i] != SQL_ROW_DELETED && row_status[i] != SQL_ROW_ERROR) {
                if (indicators[i] != SQL_NULL_DATA) {
                    struct json_object* parsed_obj = json_tokener_parse_ex(tok, chunks[i], (int)strlen(chunks[i]));
                    if (parsed_obj) {
                        result_json = parsed_obj;
                    }
                }
            }
        }
    }
    
    if (ret != SQL_NO_DATA && ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        odbcutil_log_error(SQL_HANDLE_STMT, hstmt, "SQLFetchScroll failed while iterating rowset in batch mode");
    }
    
    if (!has_rows || !result_json) {
        result_json = json_tokener_parse("[]");
    }
    
    return result_json;
}


struct json_object* odbcutil_get_json(const char* sp_call, const char* func_name) {
    SQLHDBC hdbc = odbcutil_connect();
    if (hdbc == SQL_NULL_HDBC) return nullptr;
    
    SQLHSTMT hstmt = odbcutil_alloc_stmt(hdbc, func_name);
    if (!hstmt) return nullptr;
    
    struct json_object* result_json = nullptr;
    
    SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)sp_call, SQL_NTS);
    
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        result_json = odbcutil_fetch_json(hstmt);
    } else {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Failed to execute SQLExecDirect in %s", func_name);
        odbcutil_log_error(SQL_HANDLE_STMT, hstmt, err_msg);
    }
    
    odbcutil_disconnect(hdbc, hstmt);
    return result_json;
}
