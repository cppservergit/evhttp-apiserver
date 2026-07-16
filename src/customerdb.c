#include <sql.h>
#include <sqlext.h>
#include <stdlib.h>
#include "customerdb.h"
#include "odbcutil.h"
#include "server.h"

struct json_object* customerdb_get_data(const char* customer_id) {
    SQLHDBC hdbc = odbcutil_connect();
    if (hdbc == SQL_NULL_HDBC) return nullptr;
    
    SQLHSTMT hstmt = odbcutil_alloc_stmt(hdbc, __func__);
    if (!hstmt) return nullptr;
    
    struct json_object* result_json = nullptr;
    
    SQLLEN cbId = SQL_NTS;
    SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 0, 0, (SQLPOINTER)customer_id, 0, &cbId);
    
    SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)"{CALL sp_customer_get(?)}", SQL_NTS);
    
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        result_json = odbcutil_fetch_json_batch(hstmt, __func__);
    } else {
        odbcutil_log_error(SQL_HANDLE_STMT, hstmt, "Failed to execute SQLExecDirect for sp_customer_get");
    }
    
    odbcutil_disconnect(hdbc, hstmt);
    
    return result_json;
}
