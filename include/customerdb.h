#pragma once
#include <json-c/json.h>

/**
 * \file customerdb.h
 * \brief Database integration for customer data.
 */

/**
 * \brief Retrieves customer data from the database using a stored procedure.
 * \param customer_id The customer identifier.
 * \return A newly allocated json_object array of results (caller must free).
 */
struct json_object* customerdb_get_data(const char* customer_id);
