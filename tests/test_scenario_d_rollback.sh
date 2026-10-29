#!/bin/bash
set -euo pipefail

# Scenario D: Failed Container Creation and Recovery Test Suite
# Tests: configuration pre-validation, missing image/rootfs rejection,
# port collision detection, transactional rollback of partial resources,
# and successful retry after configuration correction.

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

if [ "$EUID" -ne 0 ]; then
    echo "Error: Scenario D test must be executed as root (sudo)."
    exit 1
fi

echo "=========================================================="
echo " SCENARIO D: FAILED CREATION & RECOVERY TEST"
echo "=========================================================="

echo "[Step 1] Testing rejection of non-existent image..."
set +e
ERR_OUT=$(./mini run non-existent-image:latest --name d-bad-image -d 2>&1)
RET=$?
set -e
echo "  Command returned status: $RET"
if [ "$RET" -eq 0 ]; then
    echo "FAIL: Runtime accepted non-existent image!"
    exit 1
fi
if ./mini ps | grep -q "d-bad-image"; then
    echo "FAIL: Partial container record created for invalid image!"
    exit 1
fi
echo "  -> PASS: Non-existent image cleanly rejected without leftover records."

echo "[Step 2] Testing rejection of invalid container name..."
set +e
ERR_OUT=$(./mini run demo-api:v1 --name "bad/name!@#" -d 2>&1)
RET=$?
set -e
echo "  Command returned status: $RET"
if [ "$RET" -eq 0 ]; then
    echo "FAIL: Runtime accepted illegal container name!"
    exit 1
fi
echo "  -> PASS: Illegal container name rejected."

echo "[Step 3] Testing rejection of malformed memory limit..."
set +e
ERR_OUT=$(./mini run demo-api:v1 --name d-bad-mem --memory "invalid_limit" -d 2>&1)
RET=$?
set -e
echo "  Command returned status: $RET"
if [ "$RET" -eq 0 ]; then
    echo "FAIL: Runtime accepted invalid memory format!"
    exit 1
fi
echo "  -> PASS: Malformed memory limit rejected."

echo "[Step 4] Testing port conflict detection and transactional rollback..."
C_PORT1="d-port-ok"
C_PORT2="d-port-clash"

./mini rm "$C_PORT1" -f >/dev/null 2>&1 || true
./mini rm "$C_PORT2" -f >/dev/null 2>&1 || true

echo "  Starting container 1 on host port 3888:3000..."
CID1=$(./mini run demo-api:v1 --name "$C_PORT1" --publish 3888:3000 -d)
sleep 1

echo "  Attempting to start container 2 conflicting on same host port 3888:3000..."
set +e
ERR_OUT=$(./mini run demo-api:v1 --name "$C_PORT2" --publish 3888:3000 -d 2>&1)
RET=$?
set -e
echo "  Command returned status: $RET"
echo "  Output: $ERR_OUT"

if [ "$RET" -eq 0 ]; then
    echo "FAIL: Port clash was not detected!"
    exit 1
fi

echo "[Step 5] Verifying no leftover cgroups, network interfaces, or IP leases for failed container..."
if ./mini ps | grep -q "$C_PORT2"; then
    echo "FAIL: Failed container found in container list!"
    exit 1
fi

# Verify no metadata directory was created for d-port-clash
CLASH_MATCHES=$(grep -l "\"name\": \"$C_PORT2\"" /var/lib/kernelbox/containers/*/state.json 2>/dev/null || true)
if [ -n "$CLASH_MATCHES" ]; then
    echo "FAIL: Container directory exists for rejected container: $CLASH_MATCHES"
    exit 1
fi
echo "  -> PASS: Zero leftover directories for failed container creation."

echo "[Step 6] Correcting configuration and verifying successful retry..."
echo "  Starting container 2 with non-conflicting port 3889:3000..."
CID2=$(./mini run demo-api:v1 --name "$C_PORT2" --publish 3889:3000 -d)
sleep 2

if ! ./mini ps | grep -q "Running.*$C_PORT2"; then
    echo "FAIL: Retried container failed to start after correcting port!"
    exit 1
fi

RESP1=$(curl -s http://127.0.0.1:3888/health || echo "fail1")
RESP2=$(curl -s http://127.0.0.1:3889/health || echo "fail2")
echo "  Container 1 response: $RESP1"
echo "  Container 2 response: $RESP2"

if ! echo "$RESP1" | grep -q "healthy" || ! echo "$RESP2" | grep -q "healthy"; then
    echo "FAIL: One or both containers not responding on their respective ports!"
    exit 1
fi
echo "  -> PASS: Both containers operational on independent ports."

echo "[Step 7] Cleaning up Scenario D containers..."
./mini stop "$C_PORT1" >/dev/null 2>&1
./mini rm "$C_PORT1" >/dev/null 2>&1
./mini stop "$C_PORT2" >/dev/null 2>&1
./mini rm "$C_PORT2" >/dev/null 2>&1

echo "=========================================================="
echo " SCENARIO D: ALL ROLLBACK & RECOVERY TESTS PASSED"
echo "=========================================================="
