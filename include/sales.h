#pragma once
#include <json-c/json.h>

/**
 * \file sales.h
 * \brief Database integration for sales analytics data.
 */

/** 
 * \brief Fetch sales data from ODBC backend between a date range. 
 * \param start_date Beginning of the range (YYYY-MM-DD).
 * \param end_date End of the range (YYYY-MM-DD).
 * \return A newly allocated json_object array (caller must free).
 */
struct json_object* sales_service_get_data(const char* start_date, const char* end_date);
