#pragma once

#include <sql.h>
#include <sqlext.h>
#include <json-c/json.h>

/**
 * \file odbcutil.h
 * \brief ODBC database utility wrappers and abstractions.
 */

#define MAX_ODBC_CONN_STR_LEN 1024
#define ODBC_FETCH_CHUNK_SIZE 8192

/** \brief Retrieves a new database connection from the driver pool. \return The ODBC connection handle. */
SQLHDBC odbcutil_connect(void);

/** \brief Extracts JSON data natively from an executed SQL statement. \param hstmt The executed statement. \return A newly allocated json_object array. */
struct json_object* odbcutil_fetch_json(SQLHSTMT hstmt);

/** \brief Extracts JSON data using batch fetching (4 rows at a time). \param hstmt The executed statement. \return A newly allocated json_object array. */
struct json_object* odbcutil_fetch_json_batch(SQLHSTMT hstmt);

/** \brief Extracts and logs ODBC diagnostic records. */
void odbcutil_log_error(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg);

/** \brief Allocates a statement handle and handles cleanup/logging on failure. */
SQLHSTMT odbcutil_alloc_stmt(SQLHDBC hdbc, const char* func_name);

/** \brief Frees the statement and disconnects/frees the database connection. */
void odbcutil_disconnect(SQLHDBC hdbc, SQLHSTMT hstmt);

/** \brief Encapsulates the entire connect, execute, fetch, and disconnect flow for parameter-less queries. */
struct json_object* odbcutil_get_json(const char* sp_call, const char* func_name);
