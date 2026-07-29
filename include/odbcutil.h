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

/** \brief Allocates a statement handle and handles cleanup/logging on failure. */
SQLHSTMT odbcutil_alloc_stmt(DbConnectionId db_id, SQLHDBC hdbc, const char* func_name);

/** \brief Frees the statement and returns the connection to the thread-local pool. The connection remains open by design. */
void odbcutil_disconnect(SQLHDBC hdbc, SQLHSTMT hstmt);

