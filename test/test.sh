#!/bin/bash
set -e

API_URL="http://127.0.0.1:8080"
USERNAME="mcordova"
PASSWORD="basica"

# Extract TELEMETRY_API_KEY from bin/apiserver.env
TELEMETRY_API_KEY=$(grep '^TELEMETRY_API_KEY=' bin/apiserver.env | cut -d '=' -f2 | tr -d '\r')

echo "Attempting to login with username: $USERNAME"

LOGIN_RESPONSE=$(curl -s -X POST "$API_URL/login" \
    -H "Content-Type: application/json" \
    -d "{\"username\": \"$USERNAME\", \"password\": \"$PASSWORD\"}")

# Use jq to extract the token
TOKEN=$(echo "$LOGIN_RESPONSE" | jq -r '.token')

if [ "$TOKEN" == "null" ] || [ -z "$TOKEN" ]; then
    echo "Login failed. Response was:"
    echo "$LOGIN_RESPONSE"
    exit 1
fi

echo "Login successful!"
echo "Extracted Token: ${TOKEN:0:20}... (truncated for display)"
echo "Telemetry API Key: $TELEMETRY_API_KEY"

NUM_REQUESTS=100
echo "Running stress test with $NUM_REQUESTS requests to ALL endpoints..."

# Running background jobs to simulate concurrency stress test
for i in $(seq 1 $NUM_REQUESTS); do
    (
        # Request to /ping (No security required)
        curl -s -o /dev/null -X GET "$API_URL/ping"
        
        # Request to /version (Telemetry API Key)
        curl -s -o /dev/null -X GET "$API_URL/version" -H "Authorization: Bearer $TELEMETRY_API_KEY"

        # Request to /sysinfo (Telemetry API Key)
        curl -s -o /dev/null -X GET "$API_URL/sysinfo" -H "Authorization: Bearer $TELEMETRY_API_KEY"
        
        # Request to /rsysinfo (JWT)
        curl -s -o /dev/null -X GET "$API_URL/rsysinfo" -H "Authorization: Bearer $TOKEN"
        
        # Request to /customer (JWT)
        curl -s -o /dev/null -X POST "$API_URL/customer" \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $TOKEN" \
            -d '{"id": "ALFKI"}'
            
        # Request to /customer/get (JWT)
        curl -s -o /dev/null -X POST "$API_URL/customer/get" \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $TOKEN" \
            -d '{"id": "ALFKI"}'

        curl -s -o /dev/null -X POST "$API_URL/customer/get" \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $TOKEN" \
            -d '{"id": "ERNSH"}'

        curl -s -o /dev/null -X POST "$API_URL/customer/get" \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $TOKEN" \
            -d '{"id": "BERGS"}'

        curl -s -o /dev/null -X POST "$API_URL/customer/get" \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $TOKEN" \
            -d '{"id": "QUICK"}'

        # Request to /sales (JWT)
        curl -s -o /dev/null -X POST "$API_URL/sales" \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $TOKEN" \
            -d '{"start_date": "1994-01-01", "end_date": "1996-12-31"}'

        # Request to /shippers (JWT)
        curl -s -o /dev/null -X GET "$API_URL/shippers" -H "Authorization: Bearer $TOKEN"

        # Request to /products (JWT)
        curl -s -o /dev/null -X GET "$API_URL/products" -H "Authorization: Bearer $TOKEN"

        # Request to /uuid (No security required)
        curl -s -o /dev/null -X GET "$API_URL/uuid"
        
        # Request to /getqr (JWT)
        curl -s -o /dev/null -X GET "$API_URL/getqr" -H "Authorization: Bearer $TOKEN"

        # Request to /metrics (Telemetry API Key)
        curl -s -o /dev/null -X GET "$API_URL/metrics" -H "Authorization: Bearer $TELEMETRY_API_KEY"
    ) &
    
    if [ $((i % 20)) -eq 0 ]; then
        echo "Dispatched $i request batches..."
    fi
done

echo "Waiting for all background requests to finish..."
wait
echo "Stress test finished!"
