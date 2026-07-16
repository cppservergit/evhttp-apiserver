#include <sql.h>
#include <sqlext.h>
#include <stdlib.h>
#include "odbcutil.h"
#include <json-c/json.h>
#include "sales.h"
#include "server.h"

struct json_object* sales_service_get_data(const char* start_date, const char* end_date) {
    SQLHDBC hdbc = odbcutil_connect();
    if (hdbc == SQL_NULL_HDBC) return nullptr;
    
    SQLHSTMT hstmt = odbcutil_alloc_stmt(hdbc, __func__);
    if (!hstmt) return nullptr;
    
    struct json_object* result_json = nullptr;
    
    SQLLEN cbStart = SQL_NTS, cbEnd = SQL_NTS;
    SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 0, 0, (SQLPOINTER)start_date, 0, &cbStart);
    SQLBindParameter(hstmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 0, 0, (SQLPOINTER)end_date, 0, &cbEnd);
    
    SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)"{CALL sp_sales_by_category(?,?)}", SQL_NTS);
    
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        result_json = odbcutil_fetch_json_batch(hstmt, __func__);
    } else {
        odbcutil_log_error(SQL_HANDLE_STMT, hstmt, "Failed to execute SQLExecDirect for sp_sales_by_category");
    }
    
    odbcutil_disconnect(hdbc, hstmt);
    
    return result_json;
}
