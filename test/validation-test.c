#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <json-c/json.h>
#include "validation.h"

// --- Custom Validators for Test ---
static bool validate_positive_amount(const ValidationContext *ctx, const json_object *obj, const char *name, char *err_buf, size_t err_len) {
    (void)ctx;
    if (json_object_get_double((struct json_object*)obj) < 0.0) {
        return emit_error(err_buf, err_len, ERR_NEGATIVE_AMOUNT, name);
    }
    return true;
}

static bool validate_start_before_end(const ValidationContext *ctx, const json_object *root, const char *name, char *err_buf, size_t err_len) {
    (void)ctx; (void)name;
    struct json_object *start_obj = nullptr;
    struct json_object *end_obj = nullptr;

    json_object_object_get_ex((struct json_object*)root, "start_date", &start_obj);
    json_object_object_get_ex((struct json_object*)root, "end_date", &end_obj);

    if (start_obj && end_obj) {
        if (strcmp(json_object_get_string(start_obj), json_object_get_string(end_obj)) >= 0) {
            return emit_error(err_buf, err_len, ERR_START_AFTER_END, nullptr);
        }
    }
    return true;
}

static const FieldValidator TestSchema[] = {
    {.field_name = "start_date", .type = TYPE_DATE,   .is_required = true,  .custom_validator = nullptr},
    {.field_name = "end_date",   .type = TYPE_DATE,   .is_required = true,  .custom_validator = nullptr},
    {.field_name = "amount",     .type = TYPE_DOUBLE, .is_required = true,  .custom_validator = validate_positive_amount},
    {.field_name = "count",      .type = TYPE_INT,    .is_required = false, .custom_validator = nullptr},
    {.field_name = "name",       .type = TYPE_STRING, .is_required = true,  .custom_validator = nullptr},
    {.field_name = "opt_string", .type = TYPE_STRING, .is_required = false, .custom_validator = nullptr},
};

static const ValidationContext TestContext = {
    .schema = TestSchema,
    .schema_count = sizeof(TestSchema) / sizeof(TestSchema[0]),
    .global_validator = validate_start_before_end
};

static void test_coverage(void) {
    char err_buf[256];
    
    // Test emit_error null safety
    assert(emit_error(nullptr, 100, ERR_REQUIRED, "x") == false);
    assert(emit_error(err_buf, 0, ERR_REQUIRED, "x") == false);
    
    // Test emit_error unknown code
    assert(emit_error(err_buf, sizeof(err_buf), ERR_MAX_ERRORS, "x") == false);
    assert(emit_error(err_buf, sizeof(err_buf), (ErrorCode)999, "x") == false);

    // Test validate_json null safety
    assert(validate_json(nullptr, nullptr, nullptr, 0) == false);
    struct json_object* empty_obj = json_object_new_object();
    assert(validate_json(&TestContext, empty_obj, err_buf, sizeof(err_buf)) == false);

    // Test validate_json on non-object payload
    struct json_object* arr = json_object_new_array();
    assert(validate_json(&TestContext, arr, err_buf, sizeof(err_buf)) == false);
    json_object_put(arr);

    // Test json extraction utilities
    struct json_object* root = json_object_new_object();
    json_object_object_add(root, "str", json_object_new_string("hello"));
    json_object_object_add(root, "i", json_object_new_int(42));
    json_object_object_add(root, "d", json_object_new_double(3.14));
    
    assert(strcmp(json_get_string(root, "str"), "hello") == 0);
    assert(json_get_string(root, "missing") == nullptr);
    
    assert(json_get_int(root, "i") == 42);
    assert(json_get_int(root, "missing") == 0);
    
    assert(json_get_double(root, "d") == 3.14);
    assert(json_get_double(root, "missing") == 0.0);
    json_object_put(root);

    // Test valid schema
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"count\": 10, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == true);
    json_object_put(root);

    // Test missing required field
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false); // missing "name"
    json_object_put(root);

    // Test required field with empty string
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);
    
    // Test required field with empty object
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": {}}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Test required field with empty array
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": []}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);
    
    // Test required field with null
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": null}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Test TYPE_INT failure
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"count\": \"10\", \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Test TYPE_DOUBLE failure (passing string)
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": \"100.5\", \"count\": 10, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Test TYPE_DOUBLE success with integer
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100, \"count\": 10, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == true);
    json_object_put(root);

    // Test TYPE_STRING failure
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"count\": 10, \"name\": 123}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Test custom validator failure
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": -50.0, \"count\": 10, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Test global validator failure
    root = json_tokener_parse("{\"start_date\":\"2024-12-31\", \"end_date\":\"2024-01-01\", \"amount\": 100.0, \"count\": 10, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Test DATE specific edge cases
    // Not a string
    root = json_tokener_parse("{\"start_date\":1234, \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);
    
    // Short date
    root = json_tokener_parse("{\"start_date\":\"2024\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Long date
    root = json_tokener_parse("{\"start_date\":\"2024-01-01T00:00:00\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Wrong hyphen positions
    root = json_tokener_parse("{\"start_date\":\"2024/01/01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Non-numeric characters
    root = json_tokener_parse("{\"start_date\":\"2024-01-XX\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Invalid month
    root = json_tokener_parse("{\"start_date\":\"2024-13-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    root = json_tokener_parse("{\"start_date\":\"2024-00-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Invalid day
    root = json_tokener_parse("{\"start_date\":\"2024-01-32\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);
    
    root = json_tokener_parse("{\"start_date\":\"2024-01-00\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Invalid leap year
    root = json_tokener_parse("{\"start_date\":\"2023-02-29\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Valid leap year
    root = json_tokener_parse("{\"start_date\":\"2024-02-29\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == true);
    json_object_put(root);
    
    // Invalid century leap year (1900 is not leap)
    root = json_tokener_parse("{\"start_date\":\"1900-02-29\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    // Valid century leap year (2000 is leap)
    root = json_tokener_parse("{\"start_date\":\"2000-02-29\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == true);
    json_object_put(root);

    // Test unknown type schema definition (trigger ERR_UNKNOWN_TYPE)
    FieldValidator bad_schema[] = {
        {.field_name = "start_date", .type = (FieldType)999, .is_required = true, .custom_validator = nullptr}
    };
    ValidationContext bad_ctx = {
        .schema = bad_schema,
        .schema_count = 1,
        .global_validator = nullptr
    };
    root = json_tokener_parse("{\"start_date\":\"2000-01-01\"}");
    assert(validate_json(&bad_ctx, root, err_buf, sizeof(err_buf)) == false);
    json_object_put(root);

    json_object_put(empty_obj);
    printf("All validation edge cases passed successfully!\n");
}

int main(void) {
    test_coverage();
    return 0;
}