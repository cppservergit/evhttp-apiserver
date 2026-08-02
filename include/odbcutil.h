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

/** \brief ODBC query parameter types. */
typedef enum {
    PARAM_STRING,
    PARAM_INT,
    PARAM_DOUBLE,
    PARAM_NULL
} ParamType;

/** \brief Structure for binding an ODBC query parameter. */
typedef struct {
    const void *value;
    SQLLEN      ind; // Preserves stack liveness during SQLExecute
    ParamType   type;
    char        _padding[4];
} QueryParam;

/** \brief Structure for binding an ODBC query output column. */
typedef struct {
    void*     buffer;      // Pointer to the caller's stack/heap buffer
    SQLLEN    buffer_len;  // Maximum capacity of the buffer
    SQLLEN    ind;         // Will hold the returned length or SQL_NULL_DATA
    ParamType type;        // Expected C data type
    char      _padding[4];
} OutParam;

/** \brief Supported database connection identifiers. */
typedef enum {
    DB_0 = 0,
    DB_1,
    DB_2,
    DB_3,
    MAX_DB_CONNECTIONS
} DbConnectionId;

/** \brief Retrieves a new database connection from the driver pool. 
 * \param db_id The database connection ID.
 * \return The ODBC connection handle.
 */
SQLHDBC odbcutil_connect(DbConnectionId db_id);

#include <stdbool.h>

/** \brief Encapsulates the entire connect, execute, fetch, and disconnect flow with data binding callback. */
bool odbcutil_get_json(DbConnectionId db_id, const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name);

/** \brief Executes query and converts traditional resultset to JSON array of objects. */
bool odbcutil_get_rs2json(DbConnectionId db_id, const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name);

/** \brief Extracts and logs ODBC diagnostic records. */
void odbcutil_set_error(DbConnectionId db_id, SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg);

/** \brief Executes a query and fetches a single row directly into the provided OutParam buffers. */
bool odbcutil_query_single_row(DbConnectionId db_id, const char* query, QueryParam* in_params, size_t in_count, OutParam* out_params, size_t out_count, const char* func_name);

/** \brief Allocates a statement handle and handles cleanup/logging on failure. */
SQLHSTMT odbcutil_alloc_stmt(DbConnectionId db_id, SQLHDBC hdbc, const char* func_name);

/** \brief Safely cleans up a statement handle, verifying it hasn't been freed by a reset. */
void odbcutil_cleanup_stmt(SQLHSTMT* stmt);

/** \brief Checks if a statement handle is still valid in the thread-local pool. */
bool odbcutil_is_valid_stmt(SQLHSTMT hstmt);

/** \brief Scoped auto-cleanup for ODBC statement handle. */
#define raii_odbc_stmt [[gnu::cleanup(odbcutil_cleanup_stmt)]] SQLHSTMT

