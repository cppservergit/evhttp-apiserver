#!/bin/bash
set -e

API_URL="http://127.0.0.1:8080"
USERNAME="mcordova"
PASSWORD="basica"

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

NUM_REQUESTS=100
echo "Running stress test with $NUM_REQUESTS requests to ALL endpoints..."

# Running background jobs to simulate concurrency stress test
for i in $(seq 1 $NUM_REQUESTS); do
    (
        # Request to /ping
        curl -s -o /dev/null -X GET "$API_URL/ping" -H "Authorization: Bearer $TOKEN"
        
        # Request to /version
        curl -s -o /dev/null -X GET "$API_URL/version" -H "Authorization: Bearer $TOKEN"

        # Request to /sysinfo
        curl -s -o /dev/null -X GET "$API_URL/sysinfo" -H "Authorization: Bearer $TOKEN"
        
        # Request to /rsysinfo
        curl -s -o /dev/null -X GET "$API_URL/rsysinfo" -H "Authorization: Bearer $TOKEN"
        
        # Request to /customer
        curl -s -o /dev/null -X POST "$API_URL/customer" \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $TOKEN" \
            -d '{"id": "ALFKI", "company_name": "Alfreds", "contact_name": "Maria", "contact_title": "Sales", "address": "Obere Str. 57", "city": "Berlin", "country": "Germany"}'
            
        # Request to /customer/get
        curl -s -o /dev/null -X POST "$API_URL/customer/get" \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $TOKEN" \
            -d '{"id": "ALFKI"}'

        # Request to /sales
        curl -s -o /dev/null -X POST "$API_URL/sales" \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $TOKEN" \
            -d '{"start_date": "2024-01-01", "end_date": "2024-01-31"}'

        # Request to /shippers
        curl -s -o /dev/null -X GET "$API_URL/shippers" -H "Authorization: Bearer $TOKEN"

        # Request to /products
        curl -s -o /dev/null -X GET "$API_URL/products" -H "Authorization: Bearer $TOKEN"

        # Request to /uuid
        curl -s -o /dev/null -X GET "$API_URL/uuid" -H "Authorization: Bearer $TOKEN"
        
        # Request to /getqr
        curl -s -o /dev/null -X GET "$API_URL/getqr" -H "Authorization: Bearer $TOKEN"

        # Request to /metrics
        curl -s -o /dev/null -X GET "$API_URL/metrics" -H "Authorization: Bearer $TOKEN"
    ) &
    
    if [ $((i % 20)) -eq 0 ]; then
        echo "Dispatched $i request batches..."
    fi
done

echo "Waiting for all background requests to finish..."
wait
echo "Stress test finished!"
