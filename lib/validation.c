#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <json-c/json.h>
#include <apiserver/validation.h>
#include <stdint.h>

// --- Centralized Error Code Registry ---


// --- Safe Format Utility ---

bool emit_error(char *err_buf, size_t err_len, ErrorCode code, const char *arg) {
    if (!err_buf || err_len == 0) return false;
    
    switch (code) {
        case ERR_REQUIRED:
            (void)snprintf(err_buf, err_len, "Field '%s' is required.", arg ? arg : "");
            break;
        case ERR_NOT_INT:
            (void)snprintf(err_buf, err_len, "Field '%s' must be an integer.", arg ? arg : "");
            break;
        case ERR_NOT_DOUBLE:
            (void)snprintf(err_buf, err_len, "Field '%s' must be a numeric decimal (double).", arg ? arg : "");
            break;
        case ERR_NOT_STRING:
            (void)snprintf(err_buf, err_len, "Field '%s' must be a string.", arg ? arg : "");
            break;
        case ERR_NOT_DATE:
            (void)snprintf(err_buf, err_len, "Field '%s' must be a date string.", arg ? arg : "");
            break;
        case ERR_INVALID_DATE:
            (void)snprintf(err_buf, err_len, "Field '%s' contains an invalid date format or calendar lie.", arg ? arg : "");
            break;
        case ERR_UNKNOWN_TYPE:
            (void)snprintf(err_buf, err_len, "Internal error: Unknown validation type configured.");
            break;
        case ERR_TOO_LONG:
            (void)snprintf(err_buf, err_len, "Field '%s' exceeds maximum allowed length.", arg ? arg : "");
            break;
        case ERR_TOO_SMALL:
            (void)snprintf(err_buf, err_len, "Field '%s' is below minimum allowed value.", arg ? arg : "");
            break;
        case ERR_TOO_LARGE:
            (void)snprintf(err_buf, err_len, "Field '%s' exceeds maximum allowed value.", arg ? arg : "");
            break;
        default:
            (void)snprintf(err_buf, err_len, "Unknown error code: %d", code);
            break;
    }

    return false; 
}

// --- Framework Types ---

typedef bool (*TypeValidatorFunc)(const json_object *obj, const FieldValidator *field, char *err_buf, size_t err_len);




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

    if (month < 1 || month > 12 || day < 1 || year < 1900 || year > 2199) return false;

    static const int days_in_month[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int max_days = days_in_month[month];

    if (month == 2) {
        bool is_leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (is_leap) max_days = 29;
    }

    return day <= max_days;
}

// --- Dynamic Type Validators ---

static bool validate_type_int(const json_object *obj, const FieldValidator *field, char *err, size_t len) {
    if (!json_object_is_type(obj, json_type_int)) {
        return emit_error(err, len, ERR_NOT_INT, field->field_name);
    }
    int val = json_object_get_int(obj);
    if (field->has_min && val < field->min_int) {
        return emit_error(err, len, ERR_TOO_SMALL, field->field_name);
    }
    if (field->has_max && val > field->max_int) {
        return emit_error(err, len, ERR_TOO_LARGE, field->field_name);
    }
    return true;
}

static bool validate_type_double(const json_object *obj, const FieldValidator *field, char *err, size_t len) {
    if (!json_object_is_type(obj, json_type_double) && !json_object_is_type(obj, json_type_int)) {
        return emit_error(err, len, ERR_NOT_DOUBLE, field->field_name);
    }
    double val = json_object_get_double(obj);
    if (isnan(val) || isinf(val)) {
        return emit_error(err, len, ERR_NOT_DOUBLE, field->field_name);
    }
    if (field->has_min && val < field->min_dbl) {
        return emit_error(err, len, ERR_TOO_SMALL, field->field_name);
    }
    if (field->has_max && val > field->max_dbl) {
        return emit_error(err, len, ERR_TOO_LARGE, field->field_name);
    }
    return true;
}

static size_t utf8_strlen(const char *s) {
    size_t len = 0;
    for (; *s; ++s) {
        if (((unsigned char)*s & 0xC0) != 0x80) {
            len++;
        }
    }
    return len;
}

static bool validate_type_string(const json_object *obj, const FieldValidator *field, char *err, size_t len) {
    if (!json_object_is_type(obj, json_type_string)) {
        return emit_error(err, len, ERR_NOT_STRING, field->field_name);
    }
    if (field->max_len > 0) {
        const char *str = json_object_get_string((struct json_object*)(uintptr_t)obj);
        if (str && utf8_strlen(str) > field->max_len) {
            return emit_error(err, len, ERR_TOO_LONG, field->field_name);
        }
    }
    return true;
}

static bool validate_type_date(const json_object *obj, const FieldValidator *field, char *err, size_t len) {
    if (!json_object_is_type(obj, json_type_string)) {
        return emit_error(err, len, ERR_NOT_DATE, field->field_name);
    }
    if (!validate_date_string_fast(json_object_get_string((struct json_object*)(uintptr_t)obj))) {
        return emit_error(err, len, ERR_INVALID_DATE, field->field_name);
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

static bool check_field_empty(json_object *field_obj) {
    if (json_object_is_type(field_obj, json_type_string)) {
        const char* str = json_object_get_string(field_obj);
        return (!str || str[0] == '\0');
    }
    if (json_object_is_type(field_obj, json_type_array)) {
        return (json_object_array_length(field_obj) == 0);
    }
    if (json_object_is_type(field_obj, json_type_object)) {
        return (json_object_object_length(field_obj) == 0);
    }
    return false;
}

bool validate_json(const ValidationContext *ctx, const json_object *root, char *err_buf, size_t err_len) {
    if (!ctx || !root || !err_buf || err_len == 0) return false;
    
    if (!json_object_is_type(root, json_type_object)) {
        (void)snprintf(err_buf, err_len, "Payload must be a JSON object.");
        return false;
    }

    for (size_t i = 0; i < ctx->schema_count; i++) {
        const FieldValidator *field = &ctx->schema[i];
        json_object *field_obj = nullptr;

        bool exists = json_object_object_get_ex(root, field->field_name, &field_obj);

        if (!exists || !field_obj) {
            if (field->is_required) return emit_error(err_buf, err_len, ERR_REQUIRED, field->field_name);
            continue;
        }

        if (field->is_required && check_field_empty(field_obj)) {
            return emit_error(err_buf, err_len, ERR_REQUIRED, field->field_name);
        }

        if (field->type >= TYPE_MAX_TYPES || !TYPE_VALIDATOR_MAP[field->type]) {
            return emit_error(err_buf, err_len, ERR_UNKNOWN_TYPE, nullptr);
        }

        if (!TYPE_VALIDATOR_MAP[field->type](field_obj, field, err_buf, err_len)) {
            return false;
        }

        if (field->custom_validator && !field->custom_validator(field_obj, err_buf, err_len)) {
            return false; 
        }
    }

    if (ctx->global_validator && !ctx->global_validator(root, err_buf, err_len)) return false;

    return true;
}

const char* json_get_string(const struct json_object* obj, const char* key) {
    struct json_object* val;
    if (json_object_object_get_ex(obj, key, &val)) {
        return json_object_get_string(val);
    }
    return nullptr;
}

const char* json_get_string_ex(const struct json_object* obj, const char* key, int* len) {
    struct json_object* val;
    if (json_object_object_get_ex(obj, key, &val)) {
        *len = json_object_get_string_len(val);
        return json_object_get_string(val);
    }
    return nullptr;
}


int64_t json_get_int(const struct json_object* obj, const char* key) {
    struct json_object* val;
    if (json_object_object_get_ex(obj, key, &val)) {
        return json_object_get_int64(val);
    }
    return 0;
}

double json_get_double(const struct json_object* obj, const char* key) {
    struct json_object* val;
    if (json_object_object_get_ex(obj, key, &val)) {
        return json_object_get_double(val);
    }
    return 0.0;
}

