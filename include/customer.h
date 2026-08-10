#pragma once
#include <json-c/json.h>

/**
 * \file customer.h
 * \brief External API integration for customer data.
 */

/** 
 * \brief Fetches customer info from the remote REST API.
 * \param customer_id The ID of the customer.
 * \param out_http_code Pointer to store the HTTP status code.
 * \return A newly allocated json_object, or nullptr on failure.
 * \note Caller must assign the return value to a variable marked with [[gnu::cleanup(cleanup_json_object)]] to prevent memory leaks.
 */
struct json_object* customer_service_get_info(const char* customer_id, long* out_http_code);
