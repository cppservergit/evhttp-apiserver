#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <json-c/json.h>

/**
 * \file validation.h
 * \brief JSON schema validation framework.
 */

/** \brief Enumerates standard validation error codes. */
typedef enum {
    ERR_REQUIRED,
    ERR_NOT_INT,
    ERR_NOT_DOUBLE,
    ERR_NOT_STRING,
    ERR_NOT_DATE,
    ERR_INVALID_DATE,
    ERR_UNKNOWN_TYPE,
    ERR_TOO_LONG,
    ERR_TOO_SMALL,
    ERR_TOO_LARGE,
    ERR_MAX_ERRORS
} ErrorCode;

/** \brief Enumerates supported primitive JSON data types. */
typedef enum {
    TYPE_INT,
    TYPE_DOUBLE,
    TYPE_STRING,
    TYPE_DATE,
    TYPE_MAX_TYPES
} FieldType;

/** \brief Forward declaration for the schema validation context. */
typedef struct ValidationContext ValidationContext;

/** \brief Function pointer definition for a custom field validator. */
typedef bool (*CustomValidatorFunc)(const json_object *obj, char *err_buf, size_t err_len);

typedef struct {
    const char *field_name;
    CustomValidatorFunc custom_validator;
    
    // C11/C23 anonymous unions to share memory between types
    union {
        int min_int;
        double min_dbl;
    };
    union {
        size_t max_len;
        int max_int;
        double max_dbl;
    };
    
    FieldType type;
    
    // 1-bit flags tightly packed into 4 bytes
    uint32_t is_required : 1;
    uint32_t has_min     : 1;
    uint32_t has_max     : 1;
    uint32_t _pad_bits   : 29;
} FieldValidator;

/** \brief Holds an entire JSON validation schema. */
struct ValidationContext {
    const FieldValidator *schema;
    size_t schema_count;
    CustomValidatorFunc global_validator;
};

/** \brief Formats and copies a validation error message into a buffer. */
bool emit_error(char *err_buf, size_t err_len, ErrorCode code, const char *arg);

/** \brief Validates a parsed JSON object against a ValidationContext schema. */
bool validate_json(const ValidationContext *ctx, const json_object *root, char *err_buf, size_t err_len);

// JSON extraction utility functions
/** \brief Retrieves a string property from a JSON object. */
const char* json_get_string(const struct json_object* obj, const char* key);

/** \brief Retrieves a string property and its length from a JSON object. */
const char* json_get_string_ex(const struct json_object* obj, const char* key, int* len);

/** \brief Retrieves a 64-bit integer property from a JSON object. */
int64_t json_get_int(const struct json_object* obj, const char* key);

/** \brief Retrieves a double-precision property from a JSON object. */
double json_get_double(const struct json_object* obj, const char* key);
