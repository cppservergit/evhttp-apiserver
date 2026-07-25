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
#define ODBC_FETCH_CHUNK_SIZE 4096

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

typedef enum {
    PARAM_STRING,
    PARAM_INT,
    PARAM_DOUBLE,
    PARAM_NULL
} ParamType;

typedef struct {
    ParamType   type;
    const void *value;
    SQLLEN      ind; // Preserves stack liveness during SQLExecute
} QueryParam;

/** \brief Retrieves a new database connection from the driver pool. \return The ODBC connection handle. */
SQLHDBC odbcutil_connect(void);

#include <stdbool.h>

/** \brief Encapsulates the entire connect, execute, fetch, and disconnect flow with data binding callback. */
bool odbcutil_get_json(const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name);

/** \brief Extracts JSON data using native JSON streaming. \param hstmt The executed statement. */
bool odbcutil_fetch_json_native(SQLHSTMT hstmt, const char* func_name, struct evbuffer* out_buf);

/** \brief Executes query and converts traditional resultset to JSON array of objects. */
bool odbcutil_get_rs2json(const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name);

/** \brief Extracts and logs ODBC diagnostic records. */
void odbcutil_set_error(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg);

/** \brief Allocates a statement handle and handles cleanup/logging on failure. */
SQLHSTMT odbcutil_alloc_stmt(SQLHDBC hdbc, const char* func_name);

/** \brief Frees the statement and returns the connection to the thread-local pool. The connection remains open by design. */
void odbcutil_disconnect(SQLHDBC hdbc, SQLHSTMT hstmt);

