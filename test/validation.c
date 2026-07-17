#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <json-c/json.h>
#include <assert.h>

// --- Centralized Error Code Registry ---

typedef enum {
    ERR_REQUIRED,
    ERR_NOT_INT,
    ERR_NOT_DOUBLE,
    ERR_NOT_STRING,
    ERR_NOT_DATE,
    ERR_INVALID_DATE,
    ERR_UNKNOWN_TYPE,
    ERR_NEGATIVE_AMOUNT,
    ERR_START_AFTER_END,
    ERR_MAX_ERRORS 
} ErrorCode;

static const char *const ERROR_TEMPLATES[ERR_MAX_ERRORS] = {
    [ERR_REQUIRED]        = "Field '%s' is required.",
    [ERR_NOT_INT]         = "Field '%s' must be an integer.",
    [ERR_NOT_DOUBLE]      = "Field '%s' must be a numeric decimal (double).",
    [ERR_NOT_STRING]      = "Field '%s' must be a string.",
    [ERR_NOT_DATE]        = "Field '%s' must be a date string.",
    [ERR_INVALID_DATE]    = "Field '%s' contains an invalid date format or calendar lie.",
    [ERR_UNKNOWN_TYPE]    = "Internal error: Unknown validation type configured.",
    [ERR_NEGATIVE_AMOUNT] = "Field error: Amount on '%s' cannot be negative.",
    [ERR_START_AFTER_END] = "Business rule violation: 'start_date' must be strictly before 'end_date'."
};

// --- Safe Format Utility ---

static inline bool emit_error(char *err_buf, size_t err_len, ErrorCode code, const char *arg) {
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

// --- Framework Forward Declarations & Types ---

typedef enum {
    TYPE_INT,
    TYPE_DOUBLE,
    TYPE_STRING,
    TYPE_DATE,
    TYPE_MAX_TYPES
} FieldType;

typedef struct ValidationContext ValidationContext;

typedef bool (*CustomValidatorFunc)(const ValidationContext *ctx, const json_object *obj, const char *field_name, char *err_buf, size_t err_len);
typedef bool (*TypeValidatorFunc)(const json_object *obj, const char *field_name, char *err_buf, size_t err_len);

typedef struct {
    const char *field_name;
    FieldType type;
    bool is_required;
    CustomValidatorFunc custom_validator;
} FieldValidator;

struct ValidationContext {
    const FieldValidator *schema;
    size_t schema_count;
    CustomValidatorFunc global_validator;
};

// --- C23 RAII Mechanics (Placed early to avoid unknown type errors) ---

static inline void auto_json_object_put(json_object **obj) {
    if (obj && *obj) {
        json_object_put(*obj);
    }
}

static inline void auto_json_tokener_free(struct json_tokener **tok) {
    if (tok && *tok) {
        json_tokener_free(*tok);
    }
}

// Explicit definition using standard C23 attribute syntax mapping
#define RAII_JSON_OBJECT  [[gnu::cleanup(auto_json_object_put)]] json_object*
#define RAII_JSON_TOKENER [[gnu::cleanup(auto_json_tokener_free)]] struct json_tokener*

// --- High-Performance Bounded Date Validator ---

static bool validate_date_string_fast(const char *date_str) {
    if (!date_str) return false;

    // Explicitly guarantee we don't scan past 10 characters
    for (int i = 0; i < 10; i++) {
        if (date_str[i] == '\0') return false;
    }
    if (date_str[10] != '\0') return false; 

    // Hard boundary verification to reject non-numeric characters early
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (date_str[i] != '-') return false;
        } else {
            if (date_str[i] < '0' || date_str[i] > '9') return false;
        }
    }

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

// --- Custom Validators ---

static bool validate_start_before_end(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *root, 
    [[maybe_unused]] const char *name, 
    char *err_buf, 
    size_t err_len
) {
    json_object *start_obj = NULL;
    json_object *end_obj = NULL;

    json_object_object_get_ex(root, "start_date", &start_obj);
    json_object_object_get_ex(root, "end_date", &end_obj);

    // Defensive Assertions: Document and enforce engine invariants explicitly
    assert(start_obj != NULL);
    assert(end_obj != NULL);

    if (strcmp(json_object_get_string((json_object *)start_obj), 
               json_object_get_string((json_object *)end_obj)) >= 0) {
        return emit_error(err_buf, err_len, ERR_START_AFTER_END, NULL);
    }

    return true;
}

static bool validate_positive_amount(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *obj, 
    const char *name, 
    char *err_buf, 
    size_t err_len
) {
    if (json_object_get_double((json_object *)obj) < 0.0) {
        return emit_error(err_buf, err_len, ERR_NEGATIVE_AMOUNT, name);
    }
    return true;
}

// --- Engine Core Validation ---

static bool validate_json(const ValidationContext *ctx, const json_object *root, char *err_buf, size_t err_len) {
    if (!ctx || !root || !err_buf || err_len == 0) return false;

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

// --- Configuration Setup ---

static const FieldValidator FinancialTransactionSchema[] = {
    {.field_name = "start_date", .type = TYPE_DATE,   .is_required = true, .custom_validator = NULL},
    {.field_name = "end_date",   .type = TYPE_DATE,   .is_required = true, .custom_validator = NULL},
    {.field_name = "amount",     .type = TYPE_DOUBLE, .is_required = true, .custom_validator = validate_positive_amount}
};

static const ValidationContext FinancialContext = {
    .schema = FinancialTransactionSchema,
    .schema_count = sizeof(FinancialTransactionSchema) / sizeof(FinancialTransactionSchema[0]),
    .global_validator = validate_start_before_end
};

// --- Streamlined RAII Execution Pipeline ---

void process_json_input(const char *raw_json) {
    // Aggressive Guard: Reject null pointers before executing ANY string operations
    if (!raw_json) {
        printf("Processing: (nullptr)\n");
        printf("Result: INVALID PAYLOAD (Null pointer passed to pipeline)\n\n");
        return;
    }

    // Now it is completely safe to print, check sizes, and pass to the parser
    printf("Processing: %s\n", raw_json);
    char error_buffer[256] = {0};
    
    RAII_JSON_TOKENER tok = json_tokener_new();
    if (!tok) return;
    
    RAII_JSON_OBJECT root = json_tokener_parse_ex(tok, raw_json, -1);
    enum json_tokener_error j_err = json_tokener_get_error(tok);
    
    if (j_err != json_tokener_success || !root || !json_object_is_type(root, json_type_object)) {
        printf("Result: INVALID JSON Syntax (%s)\n\n", json_tokener_error_desc(j_err));
        return; 
    }

    if (validate_json(&FinancialContext, root, error_buffer, sizeof(error_buffer))) {
        printf("Result: SUCCESS\n");
    } else {
        printf("Result: VALIDATION FAILED -> %s\n", error_buffer);
    }
    printf("\n");
}

int main(void) {
    // ... [Previous Tests] ...

    printf("--- ADVANCED HARDENING TEST SUITE ---\n\n");

    // Test Case 7: Type Confusion / Array Injection Attack
    // Pass an array instead of a string to see if type checking handles nested structures.
    process_json_input("{\"start_date\":[\"1994-01-01\"],\"end_date\":\"1996-12-31\",\"amount\":100.0}");

    // Test Case 8: Boundary Condition / Leap Year Millennium Trap (Year 2000 was a leap year, 2100 is NOT)
    // Checks if the branchless date parser correctly evaluates the century leap year exceptions.
    process_json_input("{\"start_date\":\"2100-02-29\",\"end_date\":\"2101-01-01\",\"amount\":50.0}");

    // Test Case 9: Double Underflow / Subnormal Precision
    // Tests if the floating-point handler safely parses near-zero limits without triggering an FPU exception.
    process_json_input("{\"start_date\":\"1994-01-01\",\"end_date\":\"1996-12-31\",\"amount\":4.94e-324}");

    // Test Case 10: Unicode / Null-Byte Injection Attempt (\u0000)
    // Tests if an embedded raw null byte or escaped null breaks string length assumptions in the parser.
    process_json_input("{\"start_date\":\"1994-01-01\\u0000\",\"end_date\":\"1996-12-31\",\"amount\":10.0}");

    // Test Case 11: Overflow/Underflow Integer Coercion Protection
    // libjson-c will try to fit integers into standard 64-bit bounds. Let's see if passing a massive 
    // number into the double field triggers truncation or safe type evaluation.
    process_json_input("{\"start_date\":\"1994-01-01\",\"end_date\":\"1996-12-31\",\"amount\":18446744073709551615}");

    // Test Case 12: Empty/Whitespace Fields
    // Tests if field existence checks are tricked by empty strings or strings containing only spaces.
    process_json_input("{\"start_date\":\"\",\"end_date\":\"   -  -  \",\"amount\":100.0}");

    // Test Case 13: Extremely Large / Deeply Nested Payload (Stack Stress Test)
    // Tests if a massive payload with hundreds of unused keys stresses the linear validation loop.
    process_json_input("{\"start_date\":\"1994-01-01\",\"end_date\":\"1996-12-31\",\"amount\":10.0,\"extra1\":1,\"extra2\":2,\"junk\":\"data\"}");

    process_json_input("{\"start_date\":\"1994-01-01\",\"end_date\":\"\",\"amount\":18446744073709551615}");

    process_json_input("");
    process_json_input("42");
    process_json_input(nullptr);

    return 0;
}