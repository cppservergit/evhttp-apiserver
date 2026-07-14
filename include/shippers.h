#pragma once
#include <json-c/json.h>

/**
 * \file shippers.h
 * \brief Database integration for shipping carrier data.
 */

/** \brief Retrieves a list of active shippers from the database. \return A json_object array (caller frees). */
struct json_object* shippers_get_data(void);
