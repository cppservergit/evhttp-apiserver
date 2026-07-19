#include "odbcutil.h"
#include "raii.h"
#include "logger.h"
#include "server.h"
#include "config.h"
#include <string.h>

static _Thread_local SQLHDBC tl_hdbc = SQL_NULL_HDBC;

void odbcutil_reset_connection(void) {
    if (tl_hdbc != SQL_NULL_HDBC) {
        SQLDisconnect(tl_hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, tl_hdbc);
        tl_hdbc = SQL_NULL_HDBC;
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
        SQLFreeHandle(SQL_HANDLE_DBC, tl_hdbc);
        tl_hdbc = SQL_NULL_HDBC;
        return SQL_NULL_HDBC;
    }
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
                            if (result_json) {
                                json_object_put(result_json);
                            }
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

struct json_object* odbcutil_fetch_json_batch(SQLHSTMT hstmt, const char* func_name) {
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
                        if (result_json) {
                            json_object_put(result_json);
                        }
                        result_json = parsed_obj;
                    }
                }
            }
        }
    }
    
    if (ret != SQL_NO_DATA && ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "SQLFetchScroll failed while iterating rowset in batch mode in %s", func_name);
        odbcutil_log_error(SQL_HANDLE_STMT, hstmt, err_msg);
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
        result_json = odbcutil_fetch_json_batch(hstmt, func_name);
    } else {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Failed to execute SQLExecDirect in %s", func_name);
        odbcutil_log_error(SQL_HANDLE_STMT, hstmt, err_msg);
    }
    
    odbcutil_disconnect(hdbc, hstmt);
    return result_json;
}
