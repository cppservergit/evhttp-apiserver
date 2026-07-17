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
echo "Running stress test with $NUM_REQUESTS requests to secure handlers (/shippers and /getqr)..."

# Running background jobs to simulate concurrency stress test
for i in $(seq 1 $NUM_REQUESTS); do
    (
        # Request to /shippers (secure JSON endpoint)
        SHIPPER_HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X GET "$API_URL/shippers" \
            -H "Authorization: Bearer $TOKEN")
            
        if [ "$SHIPPER_HTTP_CODE" != "200" ]; then
            echo "Request $i to /shippers failed with HTTP $SHIPPER_HTTP_CODE"
        fi
        
        # Request to /getqr (secure SVG endpoint)
        GETQR_HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X GET "$API_URL/getqr" \
            -H "Authorization: Bearer $TOKEN")
            
        if [ "$GETQR_HTTP_CODE" != "200" ]; then
            echo "Request $i to /getqr failed with HTTP $GETQR_HTTP_CODE"
        fi
    ) &
    
    if [ $((i % 20)) -eq 0 ]; then
        echo "Dispatched $i requests..."
    fi
done

echo "Waiting for all background requests to finish..."
wait
echo "Stress test finished!"
