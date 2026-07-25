#!/bin/bash

# Usage: 
# ./api-tester-emp.sh [HOST] [URI_PREFIX]

# Configuration
HOST="${1:-${HOST:-http://localhost:8080}}"
URI_PREFIX="${2:-${URI_PREFIX:-}}"

LOGIN_ENDPOINT="$URI_PREFIX/login"
EMPLOYEE_ENDPOINT="$URI_PREFIX/employee"
LOGIN_PAYLOAD='{"username":"mcordova", "password":"basica"}'

# 1. Authenticate and get the token
echo "Authenticating and fetching token from $HOST$LOGIN_ENDPOINT..."
TOKEN1=$(curl -s -k --json "$LOGIN_PAYLOAD" "$HOST$LOGIN_ENDPOINT" | jq -r '.token')

if [ -z "$TOKEN1" ] || [ "$TOKEN1" == "null" ]; then
    echo "❌ Failed to obtain token. Exiting."
    exit 1
fi

echo "✅ Token acquired successfully."
echo ""

# 2. Print the table header
echo "✅ Test results completed for $EMPLOYEE_ENDPOINT"
echo
printf "%-25s | %-8s | %-8s | %-6s\n" "Test Case" "Expected" "Actual" "Result"
printf "%-25s | %-8s | %-8s | %-6s\n" "-------------------------" "--------" "--------" "------"

SUCCESS_JSON_BODY=""

# 3. Define the test runner function
run_test() {
    local test_name="$1"
    local payload="$2"
    local token="$3"
    local expected_status="$4"

    local auth_args=()
    if [ -n "$token" ]; then
        auth_args=(-H "Authorization: Bearer $token")
    fi

    local response
    response=$(curl -s -k -w "\n%{http_code}" "$HOST$EMPLOYEE_ENDPOINT" --json "$payload" "${auth_args[@]}")

    local actual_status=$(echo "$response" | tail -n1)
    local body=$(echo "$response" | sed '$d')

    local result_mark="❌ FAIL"
    if [ "$actual_status" == "$expected_status" ]; then
        result_mark="✅ OK"
    fi

    printf "%-25s | %-8s | %-8s | %-6s\n" "$test_name" "$expected_status" "$actual_status" "$result_mark"

    if [ "$test_name" == "Correct Case" ] && [ "$actual_status" == "200" ]; then
        SUCCESS_JSON_BODY="$body"
    fi
}

# 4. Execute the Test Cases
run_test "Correct Case" '{"id": 1}' "$TOKEN1" "200"
run_test "Correct Case 2" '{"id": 5}' "$TOKEN1" "200"
run_test "Missing argument" '{"other": 5}' "$TOKEN1" "400"
run_test "Invalid type (string)" '{"id": "1"}' "$TOKEN1" "400"
run_test "Invalid type (array)" '{"id": []}' "$TOKEN1" "400"
run_test "Out of bounds (too low)" '{"id": 0}' "$TOKEN1" "400"
run_test "Out of bounds (too high)" '{"id": 10}' "$TOKEN1" "400"
run_test "Invalid Token" '{"id": 1}' "ey_invalid_token_12345" "401"
run_test "No Token" '{"id": 1}' "" "401"

# 5. Print the payload of the successful request
echo ""
if [ -n "$SUCCESS_JSON_BODY" ]; then
    echo "--- 200 OK JSON Output ---"
    echo "$SUCCESS_JSON_BODY" | jq .
else
    echo "--- No 200 OK output captured ---"
fi
