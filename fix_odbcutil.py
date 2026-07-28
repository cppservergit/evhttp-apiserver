import re

with open('src/odbcutil.c', 'r') as f:
    c = f.read()

# Fix odbcutil_set_error missing db_id
c = re.sub(r'odbcutil_set_error\(SQL_HANDLE_STMT, hstmt, (.*?)\);', r'odbcutil_set_error(db_id, SQL_HANDLE_STMT, hstmt, \1);', c)

# Update helper signatures to take db_id
c = c.replace('static bool odbcutil_execute_and_bind(SQLHSTMT hstmt, const char* query, QueryParam* params, size_t param_count, const char* func_name)', 'static bool odbcutil_execute_and_bind(DbConnectionId db_id, SQLHSTMT hstmt, const char* query, QueryParam* params, size_t param_count, const char* func_name)')
c = c.replace('static bool odbc_bind_resultset_metadata(SQLHSTMT hstmt, ColumnData** out_columns, SQLSMALLINT* out_num_cols)', 'static bool odbc_bind_resultset_metadata(DbConnectionId db_id, SQLHSTMT hstmt, ColumnData** out_columns, SQLSMALLINT* out_num_cols)')
c = c.replace('static bool odbcutil_fetch_rs2json(SQLHSTMT hstmt, struct evbuffer* out_buf)', 'static bool odbcutil_fetch_rs2json(DbConnectionId db_id, SQLHSTMT hstmt, struct evbuffer* out_buf)')
c = c.replace('static bool odbcutil_fetch_json_native(SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf)', 'static bool odbcutil_fetch_json_native(DbConnectionId db_id, SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf)')

# Update internal calls to helpers
c = c.replace('odbcutil_execute_and_bind(hstmt,', 'odbcutil_execute_and_bind(db_id, hstmt,')
c = c.replace('odbc_bind_resultset_metadata(hstmt,', 'odbc_bind_resultset_metadata(db_id, hstmt,')
c = c.replace('odbcutil_fetch_rs2json(hstmt,', 'odbcutil_fetch_rs2json(db_id, hstmt,')
c = c.replace('odbcutil_fetch_json_native(hstmt,', 'odbcutil_fetch_json_native(db_id, hstmt,')

# Fix odbcutil_get_json implementation signature
c = re.sub(
    r'bool odbcutil_get_json\(DbConnectionId db_id, const char\* query, QueryParam\* params, size_t param_count, struct evbuffer\* out_buf, const char\* func_name\) \{',
    r'bool odbcutil_get_json(DbConnectionId db_id, const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name) {\n    (void)db_id; (void)query; (void)params; (void)param_count; (void)out_buf; (void)func_name; return false; // REMOVE ME\n}\n\nstruct json_object* odbcutil_get_json_old(DbConnectionId db_id, const char* query, QueryParam* params, size_t param_count, const char* func_name) {',
    c
)

with open('src/odbcutil.c', 'w') as f:
    f.write(c)
