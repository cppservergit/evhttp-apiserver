#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <json-c/json.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <math.h>
#include "validation.h"

// --- Custom Validators for Test ---
#define ERR_NEGATIVE_AMOUNT ((ErrorCode)100)
#define ERR_START_AFTER_END ((ErrorCode)101)
#define ERR_INVALID_CUSTOMER_ID ((ErrorCode)102)
#define ERR_DATE_TOO_EARLY ((ErrorCode)103)
#define ERR_DATE_TOO_LATE ((ErrorCode)104)

#define assert_validation_error(ctx, root, expected_str) \
    do { \
        err_buf[0] = '\0'; \
        assert(validate_json((ctx), (root), err_buf, sizeof(err_buf)) == false); \
        if (strstr(err_buf, (expected_str)) == nullptr) { \
            fprintf(stderr, "Assertion failed: expected error containing '%s', got '%s'\n", (expected_str), err_buf); \
            assert(strstr(err_buf, (expected_str)) != nullptr); \
        } \
    } while (0)

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
    
    // Test emit_error coverage
    assert(emit_error(err_buf, sizeof(err_buf), ERR_INVALID_CUSTOMER_ID, "abc") == false);
    assert(emit_error(err_buf, sizeof(err_buf), ERR_DATE_TOO_EARLY, nullptr) == false);
    assert(emit_error(err_buf, sizeof(err_buf), ERR_DATE_TOO_LATE, nullptr) == false);

    // Test validate_json null safety
    assert(validate_json(nullptr, nullptr, nullptr, 0) == false);
    struct json_object* empty_obj = json_object_new_object();
    assert_validation_error(&TestContext, empty_obj, "is required.");

    // Test validate_json on non-object payload
    struct json_object* arr = json_object_new_array();
    assert_validation_error(&TestContext, arr, "JSON object");
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

    // Math Safety: NAN and INFINITY
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"count\": 10, \"name\": \"test\"}");
    json_object_object_add(root, "amount", json_object_new_double(NAN));
    // We expect the validation engine to reject NANs as invalid doubles (if it does?) or we just assert something happens.
    assert_validation_error(&TestContext, root, "numeric decimal");
    json_object_put(root);
    
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"count\": 10, \"name\": \"test\"}");
    json_object_object_add(root, "amount", json_object_new_double(INFINITY));
    assert_validation_error(&TestContext, root, "numeric decimal");
    json_object_put(root);

    // Test valid schema
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"count\": 10, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == true);
    json_object_put(root);

    // Test missing required field
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5}");
    assert_validation_error(&TestContext, root, "is required."); // missing "name"
    json_object_put(root);

    // Test required field with empty string
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"\"}");
    assert_validation_error(&TestContext, root, "is required.");
    json_object_put(root);
    
    // Test required field with empty object
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": {}}");
    assert_validation_error(&TestContext, root, "is required.");
    json_object_put(root);

    // Test required field with empty array
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": []}");
    assert_validation_error(&TestContext, root, "is required.");
    json_object_put(root);
    
    // Test required field with null
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": null}");
    assert_validation_error(&TestContext, root, "is required.");
    json_object_put(root);

    // Test TYPE_INT failure
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"count\": \"10\", \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "must be an integer");
    json_object_put(root);

    // Test TYPE_DOUBLE failure (passing string)
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": \"100.5\", \"count\": 10, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "numeric decimal");
    json_object_put(root);

    // Test TYPE_DOUBLE success with integer
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100, \"count\": 10, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == true);
    json_object_put(root);

    // Test TYPE_STRING failure
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"count\": 10, \"name\": 123}");
    assert_validation_error(&TestContext, root, "must be a string");
    json_object_put(root);

    // Test custom validator failure
    root = json_tokener_parse("{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": -50.0, \"count\": 10, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "Unknown error code: 100");
    json_object_put(root);

    // Test global validator failure (start > end)
    root = json_tokener_parse("{\"start_date\":\"2024-12-31\", \"end_date\":\"2024-01-01\", \"amount\": 100.0, \"count\": 10, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "Unknown error code: 101");
    json_object_put(root);

    // Test global validator failure (start == end, must be strictly before)
    root = json_tokener_parse("{\"start_date\":\"2024-06-15\", \"end_date\":\"2024-06-15\", \"amount\": 100.0, \"count\": 10, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "Unknown error code: 101");
    json_object_put(root);

    // Test DATE specific edge cases
    // Not a string
    root = json_tokener_parse("{\"start_date\":1234, \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "must be a date string");
    json_object_put(root);
    
    // Short date
    root = json_tokener_parse("{\"start_date\":\"2024\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "invalid date");
    json_object_put(root);

    // Long date
    root = json_tokener_parse("{\"start_date\":\"2024-01-01T00:00:00\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "invalid date");
    json_object_put(root);

    // Wrong hyphen positions
    root = json_tokener_parse("{\"start_date\":\"2024/01/01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "invalid date");
    json_object_put(root);

    // Non-numeric characters
    root = json_tokener_parse("{\"start_date\":\"2024-01-XX\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "invalid date");
    json_object_put(root);

    // Invalid month
    root = json_tokener_parse("{\"start_date\":\"2024-13-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "invalid date");
    json_object_put(root);

    root = json_tokener_parse("{\"start_date\":\"2024-00-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "invalid date");
    json_object_put(root);

    // Invalid day
    root = json_tokener_parse("{\"start_date\":\"2024-01-32\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "invalid date");
    json_object_put(root);
    
    root = json_tokener_parse("{\"start_date\":\"2024-01-00\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "invalid date");
    json_object_put(root);

    // Invalid leap year
    root = json_tokener_parse("{\"start_date\":\"2023-02-29\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "invalid date");
    json_object_put(root);

    // Valid leap year
    root = json_tokener_parse("{\"start_date\":\"2024-02-29\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert(validate_json(&TestContext, root, err_buf, sizeof(err_buf)) == true);
    json_object_put(root);
    
    // Invalid century leap year (1900 is not leap)
    root = json_tokener_parse("{\"start_date\":\"1900-02-29\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": \"test\"}");
    assert_validation_error(&TestContext, root, "invalid date");
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
    assert_validation_error(&bad_ctx, root, "Unknown validation type");
    json_object_put(root);

    json_object_put(empty_obj);
    printf("All validation edge cases passed successfully!\n");
}

static void* fuzz_thread(void* arg) {
    int iterations = (int)(intptr_t)arg;
    char err_buf[256];
    char payload[1024];
    
    unsigned int seed = (unsigned int)time(nullptr) ^ (unsigned int)pthread_self();
    
    const char* templates[] = {
        "{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": %s, \"count\": %s, \"name\": \"%s\"}",
        "{\"start_date\":\"%s\", \"end_date\":\"%s\", \"amount\": %s, \"count\": %s, \"name\": \"%s\"}",
        "{\"start_date\":\"2024-01-01\", \"end_date\":\"2024-12-31\", \"amount\": 100.5, \"name\": %s}"
    };
    
    const char* bad_dates[] = {
        "2024-01-01", "202-01-01", "2024-99-99", "2024-00-00", "2024-01-32",
        "invalid_date_string", "", "2024\\u000001-01"
    };
    
    const char* bad_numbers[] = {
        "100", "-100", "0", 
        "999999999999999999999999999999999999.99",   // Double overflow
        "-999999999999999999999999999999999999.99",
        "1e400", "-1e400",                           // Extreme double bounds
        "2147483647", "2147483648",                  // Int32 overflow
        "-2147483648", "-2147483649",                // Int32 underflow
        "\"not_a_number\"", "null", "[]", "{}"
    };
    
    const char* bad_names[] = {
        "test", "", "averylongnamethatgoesonsonandonandonandonandonandon",
        "\\u0000", "null", "123", "[]"
    };

    for (int i = 0; i < iterations; i++) {
        int t = rand_r(&seed) % 3;
        
        if (t == 0) {
            (void)snprintf(payload, sizeof(payload), templates[t], 
                     bad_numbers[rand_r(&seed) % 15], 
                     bad_numbers[rand_r(&seed) % 15], 
                     bad_names[rand_r(&seed) % 7]);
        } else if (t == 1) {
            (void)snprintf(payload, sizeof(payload), templates[t], 
                     bad_dates[rand_r(&seed) % 8],
                     bad_dates[rand_r(&seed) % 8],
                     bad_numbers[rand_r(&seed) % 15], 
                     bad_numbers[rand_r(&seed) % 15], 
                     bad_names[rand_r(&seed) % 7]);
        } else {
            (void)snprintf(payload, sizeof(payload), templates[t], 
                     bad_numbers[rand_r(&seed) % 15]); 
        }

        // Randomly mutate a bit (causes syntax errors, unclosed strings, etc.)
        if (rand_r(&seed) % 10 == 0) {
            size_t len = strlen(payload);
            if (len > 0) {
                payload[rand_r(&seed) % len] = (char)(rand_r(&seed) % 255);
            }
        }

        // Parse and validate
        struct json_object* root = json_tokener_parse(payload);
        if (root) {
            // Should not crash, leak memory, or trigger ASAN bounds errors
            validate_json(&TestContext, root, err_buf, sizeof(err_buf));
            json_object_put(root);
        }
    }
    return nullptr;
}

int main(void) {
    test_coverage();
    
    int num_threads = 8;
    int iterations_per_thread = 2000;
    pthread_t threads[8];
    
    printf("Starting %d fuzzing threads (%d iterations each)...\n", num_threads, iterations_per_thread);
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], nullptr, fuzz_thread, (void*)(intptr_t)iterations_per_thread);
    }
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], nullptr);
    }
    printf("Multithreaded fuzzing complete.\n");
    return 0;
}