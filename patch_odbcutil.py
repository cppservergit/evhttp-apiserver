import re

with open('src/odbcutil.c', 'r') as f:
    c = f.read()

# 1. Update globals
c = c.replace(
    'static _Thread_local SQLHDBC tl_hdbc = SQL_NULL_HDBC;\nstatic _Thread_local SQLHSTMT tl_hstmt = SQL_NULL_HSTMT;',
    'static _Thread_local SQLHDBC tl_hdbc[MAX_DB_CONNECTIONS] = {0};\nstatic _Thread_local SQLHSTMT tl_hstmt[MAX_DB_CONNECTIONS] = {0};'
)

# 2. Update tl_hdbc_destructor
c = re.sub(
    r'static void tl_hdbc_destructor\(void\* arg\) \{.*?\n\}',
    r'''static void tl_hdbc_destructor(void* arg) {
    (void)arg;
    for (int i = 0; i < MAX_DB_CONNECTIONS; i++) {
        if (tl_hstmt[i] != SQL_NULL_HSTMT) {
            SQLFreeHandle(SQL_HANDLE_STMT, tl_hstmt[i]);
            tl_hstmt[i] = SQL_NULL_HSTMT;
        }
        if (tl_hdbc[i] != SQL_NULL_HDBC) {
            SQLDisconnect(tl_hdbc[i]);
            SQLFreeHandle(SQL_HANDLE_DBC, tl_hdbc[i]);
            tl_hdbc[i] = SQL_NULL_HDBC;
        }
    }
}''', c, flags=re.DOTALL
)

# 3. Update odbcutil_reset_connection
c = re.sub(
    r'void odbcutil_reset_connection\(void\) \{.*?\n\}',
    r'''void odbcutil_reset_connection(DbConnectionId db_id) {
    if (db_id < 0 || db_id >= MAX_DB_CONNECTIONS) return;
    if (tl_hstmt[db_id] != SQL_NULL_HSTMT) {
        SQLFreeHandle(SQL_HANDLE_STMT, tl_hstmt[db_id]);
        tl_hstmt[db_id] = SQL_NULL_HSTMT;
    }
    if (tl_hdbc[db_id] != SQL_NULL_HDBC) {
        SQLDisconnect(tl_hdbc[db_id]);
        SQLFreeHandle(SQL_HANDLE_DBC, tl_hdbc[db_id]);
        tl_hdbc[db_id] = SQL_NULL_HDBC;
    }
}''', c, flags=re.DOTALL
)

# 4. Update odbcutil_set_error
c = re.sub(
    r'void odbcutil_set_error\(SQLSMALLINT handle_type, SQLHANDLE handle, const char\* context_msg\) \{.*?\n\}',
    r'''void odbcutil_set_error(DbConnectionId db_id, SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg) {
    SQLCHAR sqlState[6], msg[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER nativeError;
    SQLSMALLINT msgLen;

    if (SQL_SUCCEEDED(SQLGetDiagRec(handle_type, handle, 1, sqlState, &nativeError, msg, sizeof(msg), &msgLen))) {
        set_thread_error(TL_ERR_ERROR, "%s | ODBC Error [%s]: %s", context_msg, sqlState, msg);
        if (strncmp((char*)sqlState, "08S01", 5) == 0 || strncmp((char*)sqlState, "08003", 5) == 0) {
            set_thread_error(TL_ERR_ERROR, "Database connection lost. Resetting thread-local connection pool.");
            odbcutil_reset_connection(db_id);
        }
    } else {
        set_thread_error(TL_ERR_ERROR, "%s | Unknown ODBC Error", context_msg);
    }
}''', c, flags=re.DOTALL
)

# 5. Update odbcutil_connect
c = re.sub(
    r'SQLHDBC odbcutil_connect\(void\) \{.*?\n\}',
    r'''SQLHDBC odbcutil_connect(DbConnectionId db_id) {
    if (db_id < 0 || db_id >= MAX_DB_CONNECTIONS) return SQL_NULL_HDBC;
    if (tl_hdbc[db_id] != SQL_NULL_HDBC) {
        return tl_hdbc[db_id];
    }
    
    char conn_str[MAX_CONFIG_STR];
    config_get_odbc_conn_str(db_id, conn_str, sizeof(conn_str));
    if (conn_str[0] == '\0') {
        set_thread_error(TL_ERR_ERROR, "Attempted to connect to unconfigured database DB_%d", db_id);
        return SQL_NULL_HDBC;
    }
    
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, server_get_odbc_env(), &tl_hdbc[db_id]))) {
        odbcutil_set_error(db_id, SQL_HANDLE_ENV, server_get_odbc_env(), "Failed to allocate ODBC connection handle");
        return SQL_NULL_HDBC;
    }
    
    SQLSetConnectAttr(tl_hdbc[db_id], SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);
    
    SQLCHAR out_conn_str[MAX_ODBC_CONN_STR_LEN];
    SQLSMALLINT out_conn_len;
    if (!SQL_SUCCEEDED(SQLDriverConnect(tl_hdbc[db_id], nullptr, (SQLCHAR*)conn_str, SQL_NTS, out_conn_str, sizeof(out_conn_str), &out_conn_len, SQL_DRIVER_NOPROMPT))) {
        odbcutil_set_error(db_id, SQL_HANDLE_DBC, tl_hdbc[db_id], "Failed to connect to database");
        odbcutil_reset_connection(db_id);
        return SQL_NULL_HDBC;
    }
    
    pthread_once(&tl_hdbc_key_once, make_tl_hdbc_key);
    pthread_setspecific(tl_hdbc_key, (void*)1);
    
    return tl_hdbc[db_id];
}''', c, flags=re.DOTALL
)

# 6. Update odbcutil_alloc_stmt
c = re.sub(
    r'SQLHSTMT odbcutil_alloc_stmt\(SQLHDBC hdbc, const char\* func_name\) \{.*?\n\}',
    r'''SQLHSTMT odbcutil_alloc_stmt(DbConnectionId db_id, SQLHDBC hdbc, const char* func_name) {
    if (db_id < 0 || db_id >= MAX_DB_CONNECTIONS) return SQL_NULL_HSTMT;
    if (tl_hstmt[db_id] != SQL_NULL_HSTMT) {
        return tl_hstmt[db_id];
    }

    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) {
        char context_msg[256];
        (void)snprintf(context_msg, sizeof(context_msg), "Failed to allocate ODBC statement handle in %s", func_name);
        odbcutil_set_error(db_id, SQL_HANDLE_DBC, hdbc, context_msg);
        odbcutil_reset_connection(db_id);
        return SQL_NULL_HSTMT;
    }
    
    SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)5, 0);
    
    tl_hstmt[db_id] = hstmt;
    return hstmt;
}''', c, flags=re.DOTALL
)

# Update internal helpers
c = c.replace('odbcutil_set_error(SQL_HANDLE_STMT, hstmt', 'odbcutil_set_error(db_id, SQL_HANDLE_STMT, hstmt')
c = c.replace('odbcutil_set_error(SQL_HANDLE_DBC, hdbc', 'odbcutil_set_error(db_id, SQL_HANDLE_DBC, hdbc')

c = c.replace('static bool odbcutil_fetch_json_native(SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf)', 'static bool odbcutil_fetch_json_native(DbConnectionId db_id, SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf)')
c = c.replace('static bool odbcutil_execute_and_bind(SQLHSTMT hstmt, const char* query, QueryParam* params, size_t param_count, const char* func_name)', 'static bool odbcutil_execute_and_bind(DbConnectionId db_id, SQLHSTMT hstmt, const char* query, QueryParam* params, size_t param_count, const char* func_name)')
c = c.replace('static bool odbc_bind_resultset_metadata(SQLHSTMT hstmt, ColumnData** out_columns, SQLSMALLINT* out_num_cols)', 'static bool odbc_bind_resultset_metadata(DbConnectionId db_id, SQLHSTMT hstmt, ColumnData** out_columns, SQLSMALLINT* out_num_cols)')
c = c.replace('static bool odbcutil_fetch_rs2json(SQLHSTMT hstmt, struct evbuffer* out_buf)', 'static bool odbcutil_fetch_rs2json(DbConnectionId db_id, SQLHSTMT hstmt, struct evbuffer* out_buf)')

c = c.replace('odbcutil_execute_and_bind(hstmt,', 'odbcutil_execute_and_bind(db_id, hstmt,')
c = c.replace('odbc_bind_resultset_metadata(hstmt,', 'odbc_bind_resultset_metadata(db_id, hstmt,')
c = c.replace('odbcutil_fetch_rs2json(hstmt,', 'odbcutil_fetch_rs2json(db_id, hstmt,')
c = c.replace('odbcutil_fetch_json_native(hstmt,', 'odbcutil_fetch_json_native(db_id, hstmt,')

# Finally update the public APIs
c = c.replace('bool odbcutil_get_rs2json(const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name)', 'bool odbcutil_get_rs2json(DbConnectionId db_id, const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name)')
c = c.replace('struct json_object* odbcutil_get_json(const char* query, QueryParam* params, size_t param_count, const char* func_name)', 'struct json_object* odbcutil_get_json(DbConnectionId db_id, const char* query, QueryParam* params, size_t param_count, const char* func_name)')

c = c.replace('SQLHDBC hdbc = odbcutil_connect();', 'SQLHDBC hdbc = odbcutil_connect(db_id);')
c = c.replace('SQLHSTMT hstmt = odbcutil_alloc_stmt(hdbc, func_name);', 'SQLHSTMT hstmt = odbcutil_alloc_stmt(db_id, hdbc, func_name);')

with open('src/odbcutil.c', 'w') as f:
    f.write(c)
