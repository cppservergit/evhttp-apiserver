#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <json-c/json.h>
#include "validation.h"

// --- Centralized Error Code Registry ---


static const char *const ERROR_TEMPLATES[ERR_MAX_ERRORS] = {
    [ERR_REQUIRED]        = "Field '%s' is required.",
    [ERR_NOT_INT]         = "Field '%s' must be an integer.",
    [ERR_NOT_DOUBLE]      = "Field '%s' must be a numeric decimal (double).",
    [ERR_NOT_STRING]      = "Field '%s' must be a string.",
    [ERR_NOT_DATE]        = "Field '%s' must be a date string.",
    [ERR_INVALID_DATE]    = "Field '%s' contains an invalid date format or calendar lie.",
    [ERR_UNKNOWN_TYPE]    = "Internal error: Unknown validation type configured.",
    [ERR_NEGATIVE_AMOUNT] = "Field error: Amount on '%s' cannot be negative.",
    [ERR_START_AFTER_END] = "Business rule violation: 'start_date' must be strictly before 'end_date'.",
    [ERR_INVALID_CUSTOMER_ID] = "Field error: Customer ID '%s' is invalid."
};

// --- Safe Format Utility ---

bool emit_error(char *err_buf, size_t err_len, ErrorCode code, const char *arg) {
    if (!err_buf || err_len == 0) return false;
    
    if (code >= ERR_MAX_ERRORS || !ERROR_TEMPLATES[code]) {
        snprintf(err_buf, err_len, "Unknown error code: %d", code);
        return false;
    }

    if (arg) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wformat-nonliteral"
        snprintf(err_buf, err_len, ERROR_TEMPLATES[code], arg);
        #pragma GCC diagnostic pop
    } else {
        snprintf(err_buf, err_len, "%s", ERROR_TEMPLATES[code]);
    }
    return false; 
}

// --- Framework Types ---

typedef bool (*TypeValidatorFunc)(const json_object *obj, const char *field_name, char *err_buf, size_t err_len);




// --- High-Performance Optimized Date Validator ---

static bool validate_date_string_fast(const char *date_str) {
    if (!date_str) return false;

    // Explicitly guarantee we don't scan past 10 characters
    for (int i = 0; i < 10; i++) {
        if (date_str[i] == '\0') return false;
    }
    if (date_str[10] != '\0') return false; 

    // Hard boundary mapping to ensure numbers are strictly within 0-9 range
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (date_str[i] != '-') return false;
        } else {
            if (date_str[i] < '0' || date_str[i] > '9') return false; // This catches 'xx' cleanly!
        }
    }

    // Now this vector math is 100% mathematically bounded and safe from overflows
    int year  = (date_str[0] - '0') * 1000 + (date_str[1] - '0') * 100 + (date_str[2] - '0') * 10 + (date_str[3] - '0');
    int month = (date_str[5] - '0') * 10   + (date_str[6] - '0');
    int day   = (date_str[8] - '0') * 10   + (date_str[9] - '0');

    if (month < 1 || month > 12 || day < 1 || year < 0) return false;

    static const int days_in_month[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int max_days = days_in_month[month];

    if (month == 2) {
        bool is_leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (is_leap) max_days = 29;
    }

    return day <= max_days;
}

// --- Dynamic Type Validators ---

static bool validate_type_int(const json_object *obj, const char *name, char *err, size_t len) {
    if (!json_object_is_type(obj, json_type_int)) {
        return emit_error(err, len, ERR_NOT_INT, name);
    }
    return true;
}

static bool validate_type_double(const json_object *obj, const char *name, char *err, size_t len) {
    if (!json_object_is_type(obj, json_type_double) && !json_object_is_type(obj, json_type_int)) {
        return emit_error(err, len, ERR_NOT_DOUBLE, name);
    }
    return true;
}

static bool validate_type_string(const json_object *obj, const char *name, char *err, size_t len) {
    if (!json_object_is_type(obj, json_type_string)) {
        return emit_error(err, len, ERR_NOT_STRING, name);
    }
    return true;
}

static bool validate_type_date(const json_object *obj, const char *name, char *err, size_t len) {
    if (!json_object_is_type(obj, json_type_string)) {
        return emit_error(err, len, ERR_NOT_DATE, name);
    }
    if (!validate_date_string_fast(json_object_get_string((json_object *)obj))) {
        return emit_error(err, len, ERR_INVALID_DATE, name);
    }
    return true;
}

static const TypeValidatorFunc TYPE_VALIDATOR_MAP[TYPE_MAX_TYPES] = {
    [TYPE_INT]    = validate_type_int,
    [TYPE_DOUBLE] = validate_type_double,
    [TYPE_STRING] = validate_type_string,
    [TYPE_DATE]   = validate_type_date
};



// --- Engine Core Validation ---

bool validate_json(const ValidationContext *ctx, const json_object *root, char *err_buf, size_t err_len) {
    if (!ctx || !root || !err_buf || err_len == 0) return false;
    
    if (!json_object_is_type(root, json_type_object)) {
        snprintf(err_buf, err_len, "Payload must be a JSON object.");
        return false;
    }

    for (size_t i = 0; i < ctx->schema_count; i++) {
        const FieldValidator *field = &ctx->schema[i];
        json_object *field_obj = NULL;

        bool exists = json_object_object_get_ex(root, field->field_name, &field_obj);

        if (!exists || !field_obj) {
            if (field->is_required) {
                return emit_error(err_buf, err_len, ERR_REQUIRED, field->field_name);
            }
            continue;
        }

        if (field->type >= TYPE_MAX_TYPES || !TYPE_VALIDATOR_MAP[field->type]) {
            return emit_error(err_buf, err_len, ERR_UNKNOWN_TYPE, NULL);
        }

        if (!TYPE_VALIDATOR_MAP[field->type](field_obj, field->field_name, err_buf, err_len)) {
            return false;
        }

        if (field->custom_validator && !field->custom_validator(ctx, field_obj, field->field_name, err_buf, err_len)) {
            return false; 
        }
    }

    if (ctx->global_validator && !ctx->global_validator(ctx, root, NULL, err_buf, err_len)) {
        return false;
    }

    return true;
}

const char* json_get_string(const struct json_object* obj, const char* key) {
    struct json_object* val;
    if (json_object_object_get_ex((struct json_object*)obj, key, &val)) {
        return json_object_get_string(val);
    }
    return nullptr;
}

int64_t json_get_int(const struct json_object* obj, const char* key) {
    struct json_object* val;
    if (json_object_object_get_ex((struct json_object*)obj, key, &val)) {
        return json_object_get_int64(val);
    }
    return 0;
}

double json_get_double(const struct json_object* obj, const char* key) {
    struct json_object* val;
    if (json_object_object_get_ex((struct json_object*)obj, key, &val)) {
        return json_object_get_double(val);
    }
    return 0.0;
}
