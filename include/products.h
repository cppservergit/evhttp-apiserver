#pragma once
#include <json-c/json.h>

/**
 * \file products.h
 * \brief Database integration for product catalog data.
 */

/** \brief Retrieves a list of products from the database. \return A json_object array (caller frees). */
struct json_object* products_get_data(void);
