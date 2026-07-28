#define ODBC_MAX_COL_SIZE 65536
#include "odbcutil.h"
#include "raii.h"
#include "thread_error.h"
#include "server.h"
#include "config.h"
#include <string.h>
#include <strings.h>

#include <pthread.h>

static _Thread_local SQLHDBC tl_hdbc[MAX_DB_CONNECTIONS] = {0};
static _Thread_local SQLHSTMT tl_hstmt[MAX_DB_CONNECTIONS] = {0};
static pthread_key_t tl_hdbc_key;
static pthread_once_t tl_hdbc_key_once = PTHREAD_ONCE_INIT;

static void tl_hdbc_destructor(void* arg) {
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
}

static void make_tl_hdbc_key(void) {
    pthread_key_create(&tl_hdbc_key, tl_hdbc_destructor);
}

void odbcutil_reset_connection(DbConnectionId db_id) {
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
}

void odbcutil_set_error(DbConnectionId db_id, SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg) {
    SQLCHAR sqlState[6];
    SQLCHAR msg[SQL_MAX_MESSAGE_LENGTH];
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
}

SQLHDBC odbcutil_connect(DbConnectionId db_id) {
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
}

SQLHSTMT odbcutil_alloc_stmt(DbConnectionId db_id, SQLHDBC hdbc, const char* func_name) {
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
}

void odbcutil_disconnect(SQLHDBC hdbc, SQLHSTMT hstmt) {
    (void)hdbc; // Connection remains pooled persistently in tl_hdbc
    if (hstmt != SQL_NULL_HSTMT) {
        SQLFreeStmt(hstmt, SQL_CLOSE);        // Close open cursors
        SQLFreeStmt(hstmt, SQL_RESET_PARAMS); // Clear bound parameters
        SQLFreeStmt(hstmt, SQL_UNBIND);       // Clear bound columns
    }
}

static bool fetch_json_native_col_loop(DbConnectionId db_id, SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf, bool* has_data_written) {
    char chunk[ODBC_FETCH_CHUNK_SIZE];
    SQLLEN indicator;
    SQLRETURN ret;
    while (true) {
        ret = SQLGetData(hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator);
        if (!SQL_SUCCEEDED(ret) && ret != SQL_NO_DATA) {
            char err_msg[256];
            (void)snprintf(err_msg, sizeof(err_msg), "SQLGetData failed (code %d) for %s", ret, func_name);
            odbcutil_set_error(db_id, SQL_HANDLE_STMT, hstmt, err_msg);
            return false;
        }
        if (indicator == SQL_NULL_DATA) {
            if (!*has_data_written) { evbuffer_add(out_buf, "null", 4); *has_data_written = true; }
            break;
        }
        if (ret == SQL_NO_DATA) break;
        
        size_t max_payload = sizeof(chunk) - 1;
        size_t bytes_to_write = (indicator == SQL_NO_TOTAL || indicator >= (SQLLEN)max_payload) ? max_payload : 
                                (indicator >= 0 ? (size_t)indicator : strlen(chunk));
        
        if (bytes_to_write > 0) {
            evbuffer_add(out_buf, chunk, bytes_to_write);
            *has_data_written = true;
        }
        if (ret == SQL_SUCCESS) break;
    }
    return true;
}

static bool odbcutil_fetch_json_native(DbConnectionId db_id, SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf) {
    SQLRETURN ret;
    bool has_rows = false, fetch_success = true, has_data_written = false;

    while (SQL_SUCCEEDED(ret = SQLFetch(hstmt))) {
        has_rows = true;
        if (!fetch_json_native_col_loop(db_id, hstmt, func_name, out_buf, &has_data_written)) {
            return false;
        }
    }
    
    if (ret != SQL_NO_DATA && !SQL_SUCCEEDED(ret)) {
        char err_msg[256];
        (void)snprintf(err_msg, sizeof(err_msg), "SQLFetch failed for %s", func_name);
        odbcutil_set_error(db_id, SQL_HANDLE_STMT, hstmt, err_msg);
        fetch_success = false;
    }
    
    if (!has_rows) evbuffer_add(out_buf, "[]", 2);
    return fetch_success;
}


typedef bool (*odbc_fetch_cb)(DbConnectionId db_id, SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf);

static bool odbcutil_bind_param(DbConnectionId db_id, SQLHSTMT hstmt, SQLUSMALLINT param_idx, QueryParam* param) {
    SQLRETURN bind_ret;
    switch (param->type) {
        case PARAM_STRING: {
            const char *str = (const char *)param->value;
            size_t len = str ? strlen(str) : 0;
            param->ind = (str && len > 0) ? SQL_NTS : SQL_NULL_DATA;
            bind_ret = SQLBindParameter(hstmt, param_idx, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                   len > 0 ? len : 1, 0, (SQLPOINTER)str, 0, &param->ind);
            break;
        }
        case PARAM_INT: {
            param->ind = param->value ? 0 : SQL_NULL_DATA;
            bind_ret = SQLBindParameter(hstmt, param_idx, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
                                   0, 0, (SQLPOINTER)param->value, 0, &param->ind);
            break;
        }
        case PARAM_DOUBLE: {
            param->ind = param->value ? 0 : SQL_NULL_DATA;
            bind_ret = SQLBindParameter(hstmt, param_idx, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE,
                                   15, 0, (SQLPOINTER)param->value, 0, &param->ind);
            break;
        }
        case PARAM_NULL: {
            param->ind = SQL_NULL_DATA;
            bind_ret = SQLBindParameter(hstmt, param_idx, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                   0, 0, nullptr, 0, &param->ind);
            break;
        }
        default:
            odbcutil_set_error(db_id, SQL_HANDLE_STMT, hstmt, "Unsupported parameter type");
            return false;
    }

    if (!SQL_SUCCEEDED(bind_ret)) {
        odbcutil_set_error(db_id, SQL_HANDLE_STMT, hstmt, "Failed to bind parameter");
        return false;
    }
    return true;
}

static bool odbcutil_execute_and_fetch(
    DbConnectionId db_id, const char* query, 
    QueryParam* params, 
    size_t param_count, 
    struct evbuffer* out_buf, 
    const char* func_name,
    odbc_fetch_cb fetch_cb
) {
    SQLHDBC hdbc = odbcutil_connect(db_id);
    if (hdbc == SQL_NULL_HDBC) return false;
    
    SQLHSTMT hstmt = odbcutil_alloc_stmt(db_id, hdbc, func_name);
    if (!hstmt) return false;
    
    for (size_t i = 0; i < param_count; ++i) {
        if (!odbcutil_bind_param(db_id, hstmt, (SQLUSMALLINT)(i + 1), &params[i])) {
            odbcutil_disconnect(hdbc, hstmt);
            return false;
        }
    }
    
    bool success = false;
    
    if (SQL_SUCCEEDED(SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS))) {
        success = fetch_cb(db_id, hstmt, func_name, out_buf);
    } else {
        char err_msg[256];
        (void)snprintf(err_msg, sizeof(err_msg), "Failed to execute SQLExecDirect in %s", func_name);
        odbcutil_set_error(db_id, SQL_HANDLE_STMT, hstmt, err_msg);
    }
    
    odbcutil_disconnect(hdbc, hstmt);
    return success;
}

bool odbcutil_get_json(DbConnectionId db_id, const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name) {
    return odbcutil_execute_and_fetch(db_id, query, params, param_count, out_buf, func_name, odbcutil_fetch_json_native);
}

typedef struct {
    SQLCHAR     name[128];
    SQLSMALLINT name_len;
    SQLSMALLINT sql_type;
    char       *buffer;
    size_t      alloc_size;
    SQLLEN      ind;
} ColumnDescriptor;

typedef struct {
    ColumnDescriptor *cols;
    char             *arena;
    SQLSMALLINT       count;
    SQLHSTMT          hstmt;
} ResultSetMetadata;

static void metadata_destroy(ResultSetMetadata *meta) {
    if (!meta) return;
    if (meta->hstmt != SQL_NULL_HSTMT) {
        SQLFreeStmt(meta->hstmt, SQL_UNBIND);
    }
    free(meta->cols);
    free(meta->arena);
    meta->cols = nullptr;
    meta->arena = nullptr;
    meta->count = 0;
    meta->hstmt = SQL_NULL_HSTMT;
}

static bool is_json_number_type(SQLSMALLINT sql_type) {
    switch (sql_type) {
        case SQL_INTEGER:    case SQL_SMALLINT:  case SQL_TINYINT:
        case SQL_BIGINT:     case SQL_FLOAT:     case SQL_REAL:
        case SQL_DOUBLE:     case SQL_DECIMAL:   case SQL_NUMERIC:
            return true;
        default:
            return false;
    }
}

static void evbuffer_append_escaped_str(struct evbuffer *buf, const char *str, size_t len) {
    evbuffer_add(buf, "\"", 1);
    const char *start = str;
    const char *end = str + len;
    const char *p;

    for (p = str; p < end; ++p) {
        if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) continue;

        size_t clean_len = (size_t)(p - start);
        if (clean_len > 0) {
            evbuffer_add(buf, start, clean_len);
        }

        switch (*p) {
            case '"':  evbuffer_add(buf, "\\\"", 2); break;
            case '\\': evbuffer_add(buf, "\\\\", 2); break;
            case '\b': evbuffer_add(buf, "\\b",  2); break;
            case '\f': evbuffer_add(buf, "\\f",  2); break;
            case '\n': evbuffer_add(buf, "\\n",  2); break;
            case '\r': evbuffer_add(buf, "\\r",  2); break;
            case '\t': evbuffer_add(buf, "\\t",  2); break;
            default:
                evbuffer_add_printf(buf, "\\u%04x", (unsigned char)*p);
                break;
        }
        start = p + 1;
    }

    size_t remaining_len = (size_t)(p - start);
    if (remaining_len > 0) {
        evbuffer_add(buf, start, remaining_len);
    }
    evbuffer_add(buf, "\"", 1);
}

static bool alloc_metadata_cols(DbConnectionId db_id, SQLHSTMT hstmt, ResultSetMetadata *meta, SQLSMALLINT num_cols) {
    meta->hstmt = hstmt;
    meta->cols = calloc((size_t)num_cols, sizeof(ColumnDescriptor));
    if (!meta->cols) return false;
    meta->count = num_cols;

    size_t total_arena_size = 0;
    for (SQLSMALLINT i = 0; i < num_cols; ++i) {
        SQLULEN col_size = 0;
        SQLSMALLINT digits = 0, nullable = 0;
        SQLRETURN ret = SQLDescribeCol(hstmt, (SQLUSMALLINT)(i + 1), meta->cols[i].name, sizeof(meta->cols[i].name),
                                       &meta->cols[i].name_len, &meta->cols[i].sql_type, &col_size, &digits, &nullable);
        if (!SQL_SUCCEEDED(ret)) {
            odbcutil_set_error(db_id, SQL_HANDLE_STMT, hstmt, "Failed to describe column.");
            return false;
        }
        meta->cols[i].alloc_size = (col_size == 0 || col_size > ODBC_MAX_COL_SIZE) ? ODBC_MAX_COL_SIZE : (col_size + 64);
        total_arena_size += meta->cols[i].alloc_size;
    }
    meta->arena = malloc(total_arena_size);
    return meta->arena != nullptr;
}

static bool odbc_bind_resultset_metadata(DbConnectionId db_id, SQLHSTMT hstmt, [[maybe_unused]] const char* func_name, ResultSetMetadata *meta) {
    SQLSMALLINT num_cols = 0;
    SQLRETURN ret = SQLNumResultCols(hstmt, &num_cols);
    if (!SQL_SUCCEEDED(ret)) {
        odbcutil_set_error(db_id, SQL_HANDLE_STMT, hstmt, "Failed to retrieve result set column count.");
        return false;
    }
    if (num_cols == 0) {
        meta->count = 0; meta->cols = nullptr; meta->arena = nullptr; meta->hstmt = SQL_NULL_HSTMT;
        return true;
    }
    if (!alloc_metadata_cols(db_id, hstmt, meta, num_cols)) return false;

    size_t current_offset = 0;
    for (SQLSMALLINT i = 0; i < num_cols; ++i) {
        meta->cols[i].buffer = meta->arena + current_offset;
        current_offset += meta->cols[i].alloc_size;
        ret = SQLBindCol(hstmt, (SQLUSMALLINT)(i + 1), SQL_C_CHAR, meta->cols[i].buffer, meta->cols[i].alloc_size, &meta->cols[i].ind);
        if (!SQL_SUCCEEDED(ret)) {
            odbcutil_set_error(db_id, SQL_HANDLE_STMT, hstmt, "Failed to bind column.");
            return false;
        }
    }
    return true;
}

static void append_col_value(struct evbuffer *buf, const ColumnDescriptor *col) {
    size_t val_len = 0;
    if (col->ind == SQL_NO_TOTAL) {
        val_len = strlen(col->buffer);
    } else if ((size_t)col->ind >= col->alloc_size) {
        val_len = col->alloc_size - 1;
    } else {
        val_len = (size_t)col->ind;
    }

    if (col->sql_type == SQL_BIT) {
        bool val = (col->buffer[0] == '1' || strcasecmp(col->buffer, "true") == 0);
        evbuffer_add(buf, val ? "true" : "false", val ? 4 : 5);
        return;
    }

    if (is_json_number_type(col->sql_type)) {
        if (val_len == 0) {
            evbuffer_add(buf, "null", 4);
        } else {
            evbuffer_add(buf, col->buffer, val_len);
        }
        return;
    }

    evbuffer_append_escaped_str(buf, col->buffer, val_len);
}

static void evbuffer_append_row_object(struct evbuffer *buf, const ResultSetMetadata *meta) {
    evbuffer_add(buf, "{", 1);
    for (SQLSMALLINT i = 0; i < meta->count; ++i) {
        const ColumnDescriptor *col = &meta->cols[i];

        evbuffer_add(buf, "\"", 1);
        evbuffer_add(buf, col->name, col->name_len);
        evbuffer_add(buf, "\":", 2);

        if (col->ind == SQL_NULL_DATA) {
            evbuffer_add(buf, "null", 4);
        } else {
            if (col->alloc_size > 0) {
                col->buffer[col->alloc_size - 1] = '\0';
            }
            append_col_value(buf, col);
        }

        if (i < meta->count - 1) {
            evbuffer_add(buf, ",", 1);
        }
    }
    evbuffer_add(buf, "}", 1);
}

bool odbcutil_fetch_rs2json(DbConnectionId db_id, SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf) {
    [[gnu::cleanup(metadata_destroy)]] ResultSetMetadata meta = {};

    if (!odbc_bind_resultset_metadata(db_id, hstmt, func_name, &meta)) {
        return false;
    }

    if (meta.count == 0) {
        evbuffer_add(out_buf, "[]", 2);
        return true;
    }

    evbuffer_add(out_buf, "[", 1);

    SQLRETURN ret;
    bool first_row = true;

    while (SQL_SUCCEEDED(ret = SQLFetch(hstmt))) {
        if (!first_row) {
            evbuffer_add(out_buf, ",", 1);
        }
        first_row = false;

        evbuffer_append_row_object(out_buf, &meta);
    }
    
    if (ret != SQL_NO_DATA && !SQL_SUCCEEDED(ret)) {
        char err_msg[256];
        (void)snprintf(err_msg, sizeof(err_msg), "SQLFetch failed for %s", func_name);
        odbcutil_set_error(db_id, SQL_HANDLE_STMT, hstmt, err_msg);
        return false;
    }

    evbuffer_add(out_buf, "]", 1);
    return true;
}

bool odbcutil_get_rs2json(DbConnectionId db_id, const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name) {
    return odbcutil_execute_and_fetch(db_id, query, params, param_count, out_buf, func_name, odbcutil_fetch_rs2json);
}
