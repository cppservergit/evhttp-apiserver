#!/bin/bash

# Define the endpoints
URL_SYSINFO="http://localhost:8080/sysinfo"
URL_VERSION="http://localhost:8080/version"
URL_PING="http://localhost:8080/ping"
URL_METRICS="http://localhost:8080/metrics"
URL_RSYSINFO="http://localhost:8080/rsysinfo"
URL_SALES="http://localhost:8080/sales"
URL_CUSTOMER="http://localhost:8080/customer"
URL_CUSTOMER_GET="http://localhost:8080/customer/get"
URL_SHIPPERS="http://localhost:8080/shippers"
URL_PRODUCTS="http://localhost:8080/products"

# Number of concurrent requests to fire
NUM_THREADS=80

echo "Starting stress test: $NUM_THREADS concurrent threads for each endpoint..."

# Loop to spawn background threads
for i in $(seq 1 $NUM_THREADS); do
    curl -s -w "Thread $i - sysinfo: %{http_code}\n" -o /dev/null "$URL_SYSINFO" &
    curl -s -w "Thread $i - version: %{http_code}\n" -o /dev/null "$URL_VERSION" &
    curl -s -w "Thread $i - ping: %{http_code}\n" -o /dev/null "$URL_PING" &
    curl -s -w "Thread $i - metrics: %{http_code}\n" -o /dev/null "$URL_METRICS" &
    curl -s -w "Thread $i - rsysinfo: %{http_code}\n" -o /dev/null "$URL_RSYSINFO" &
    curl -s -w "Thread $i - sales: %{http_code}\n" -o /dev/null -X POST -H "Content-Type: application/json" -d '{"start_date":"1996-01-01","end_date":"1996-12-31"}' "$URL_SALES" &
    curl -s -w "Thread $i - customer: %{http_code}\n" -o /dev/null -X POST -H "Content-Type: application/json" -d '{"id":"ALFKI"}' "$URL_CUSTOMER" &
    curl -s -w "Thread $i - customer_get: %{http_code}\n" -o /dev/null -X POST -H "Content-Type: application/json" -d '{"id":"ALFKI"}' "$URL_CUSTOMER_GET" &
    curl -s -w "Thread $i - shippers: %{http_code}\n" -o /dev/null "$URL_SHIPPERS" &
    curl -s -w "Thread $i - products: %{http_code}\n" -o /dev/null "$URL_PRODUCTS" &
done

# Wait for all background jobs to finish
wait

echo "Stress test completed."
