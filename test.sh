#!/bin/bash
set -e

echo "=== Building Image ==="
sudo ./mini build examples/node-api -t demo-api:v1

echo "=== Listing Images ==="
sudo ./mini images

echo "=== Running Container ==="
# Run detached
CID=$(sudo ./mini run demo-api:v1 --name demo-api-1 --publish 3000:3000 -d)
echo "Started container ID: $CID"

echo "=== Listing Containers ==="
sudo ./mini ps

echo "=== Testing Container API ==="
sleep 2
curl -s http://10.0.100.2:3000/health || curl -s http://127.0.0.1:3000/health || echo "Failed to connect to API"

echo "=== Fetching Stats ==="
sudo ./mini stats demo-api-1

echo "=== Fetching Logs ==="
sudo ./mini logs demo-api-1

echo "=== Stopping Container ==="
sudo ./mini stop demo-api-1

echo "=== Removing Container ==="
sudo ./mini rm demo-api-1
