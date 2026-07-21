#pragma once

#include <sql.h>
#include <sqlext.h>
#include <json-c/json.h>

struct evbuffer;

/**
 * \file odbcutil.h
 * \brief ODBC database utility wrappers and abstractions.
 */

#define MAX_ODBC_CONN_STR_LEN 1024
#define ODBC_FETCH_CHUNK_SIZE 8192

typedef void (*odbc_bind_fn)(struct json_object* body, SQLHSTMT hstmt);

/** \brief Retrieves a new database connection from the driver pool. \return The ODBC connection handle. */
SQLHDBC odbcutil_connect(void);

#include <stdbool.h>

/** \brief Encapsulates the entire connect, execute, fetch, and disconnect flow with data binding callback. */
bool odbcutil_get_json(const char* query, odbc_bind_fn binder, struct json_object* body, struct evbuffer* out_buf, const char* func_name);

/** \brief Extracts JSON data using batch fetching (4 rows at a time). \param hstmt The executed statement. */
bool odbcutil_fetch_json_batch(SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf);

/** \brief Extracts and logs ODBC diagnostic records. */
void odbcutil_log_error(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg);

/** \brief Allocates a statement handle and handles cleanup/logging on failure. */
SQLHSTMT odbcutil_alloc_stmt(SQLHDBC hdbc, const char* func_name);

/** \brief Frees the statement and returns the connection to the thread-local pool. The connection remains open by design. */
void odbcutil_disconnect(SQLHDBC hdbc, SQLHSTMT hstmt);

