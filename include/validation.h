#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <json-c/json.h>

/**
 * \file validation.h
 * \brief JSON schema validation framework.
 */

typedef enum {
    ERR_REQUIRED,
    ERR_NOT_INT,
    ERR_NOT_DOUBLE,
    ERR_NOT_STRING,
    ERR_NOT_DATE,
    ERR_INVALID_DATE,
    ERR_UNKNOWN_TYPE,
    ERR_MAX_ERRORS
} ErrorCode;

typedef enum {
    TYPE_INT,
    TYPE_DOUBLE,
    TYPE_STRING,
    TYPE_DATE,
    TYPE_MAX_TYPES
} FieldType;

typedef struct ValidationContext ValidationContext;
typedef bool (*CustomValidatorFunc)(const ValidationContext *ctx, const json_object *obj, const char *field_name, char *err_buf, size_t err_len);

/** \brief Defines a schema rule for a single JSON field. */
typedef struct {
    const char *field_name;
    CustomValidatorFunc custom_validator;
    FieldType type;
    bool is_required;
    char _padding[3];
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

/** \brief Retrieves a 64-bit integer property from a JSON object. */
int64_t json_get_int(const struct json_object* obj, const char* key);

/** \brief Retrieves a double-precision property from a JSON object. */
double json_get_double(const struct json_object* obj, const char* key);
