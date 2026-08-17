#pragma once

#include <sql.h>
#include <sqlext.h>
#include <stdbool.h>
#include <stddef.h>

struct evbuffer;

/**
 * \file odbcutil.h
 * \brief ODBC database utility wrappers and abstractions.
 */

constexpr int MAX_ODBC_CONN_STR_LEN = 1024;
constexpr int ODBC_FETCH_CHUNK_SIZE = 4096;

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

/** \brief Structure for binding an ODBC query output parameter with name for JSON. */
typedef struct {
    const char* name;      // JSON key name
    void*     buffer;      // Pointer to the caller's stack/heap buffer
    SQLLEN    buffer_len;  // Maximum capacity of the buffer
    SQLLEN    ind;         // Will hold the returned length or SQL_NULL_DATA
    ParamType type;        // Expected C data type
    char      _padding[4];
} SpOutParam;

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


/** \brief Encapsulates the entire connect, execute, fetch, and disconnect flow with data binding callback. */
[[nodiscard("ODBC function return value must be evaluated")]]
bool odbcutil_get_json(DbConnectionId db_id, const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name);

/** \brief Executes query and converts traditional resultset to JSON array of objects. */
[[nodiscard("ODBC function return value must be evaluated")]]
bool odbcutil_get_rs2json(DbConnectionId db_id, const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name);

/** \brief Executes query returning multiple resultsets as a JSON object with array fields r1, r2, ... */
[[nodiscard("ODBC function return value must be evaluated")]]
bool odbcutil_get_jsonm(DbConnectionId db_id, const char* query, QueryParam* params, size_t param_count, struct evbuffer* out_buf, const char* func_name);


/** \brief Extracts and logs ODBC diagnostic records. */
void odbcutil_set_error(DbConnectionId db_id, SQLSMALLINT handle_type, SQLHANDLE handle, const char* context_msg);

/** \brief Executes a query and fetches a single row directly into the provided OutParam buffers. */
[[nodiscard("ODBC function return value must be evaluated")]]
bool odbcutil_query_single_row(DbConnectionId db_id, const char* query, QueryParam* in_params, size_t in_count, OutParam* out_params, size_t out_count, const char* func_name);

/** \brief Executes a stored procedure without a resultset, returning output parameters as a JSON object. */
[[nodiscard("ODBC function return value must be evaluated")]]
bool odbcutil_execute_sp_json(DbConnectionId db_id, const char* query, QueryParam* in_params, size_t in_count, SpOutParam* out_params, size_t out_count, struct evbuffer* out_buf, const char* func_name);

/** \brief Allocates a statement handle and handles cleanup/logging on failure. */
SQLHSTMT odbcutil_alloc_stmt(DbConnectionId db_id, SQLHDBC hdbc, const char* func_name);

/** \brief Safely cleans up a statement handle, verifying it hasn't been freed by a reset. */
void odbcutil_cleanup_stmt(const SQLHSTMT* stmt);

/** \brief Checks if a statement handle is still valid in the thread-local pool. */
bool odbcutil_is_valid_stmt(SQLHSTMT hstmt);



