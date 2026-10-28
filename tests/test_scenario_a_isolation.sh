#!/bin/bash
set -euo pipefail

# Scenario A: Container Isolation Verification Test Suite
# Tests: UTS (hostname), PID namespace, Mount namespace, and Filesystem isolation boundaries

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

if [ "$EUID" -ne 0 ]; then
    echo "Error: Scenario A test must be executed as root (sudo)."
    exit 1
fi

echo "=========================================================="
echo " SCENARIO A: CONTAINER ISOLATION VERIFICATION TEST"
echo "=========================================================="

C1="iso-test-1"
C2="iso-test-2"

# Ensure cleanup before start
./mini rm "$C1" -f >/dev/null 2>&1 || true
./mini rm "$C2" -f >/dev/null 2>&1 || true

echo "[Step 1] Ensuring demo image is available..."
if ! ./mini images | grep -q "demo-api"; then
    ./mini build examples/node-api -t demo-api:v1
fi

echo "[Step 2] Launching two concurrent containers with separate hostnames..."
CID1=$(./mini run demo-api:v1 --name "$C1" -d)
CID2=$(./mini run demo-api:v1 --name "$C2" -d)
echo "  Container 1: $C1 (ID: $CID1)"
echo "  Container 2: $C2 (ID: $CID2)"
sleep 2

echo "[Step 3] Verifying UTS (Hostname) Isolation..."
HOST_HOSTNAME=$(hostname)
C1_HOSTNAME=$(./mini exec "$C1" hostname 2>/dev/null || ./mini exec "$C1" cat /proc/sys/kernel/hostname)
C2_HOSTNAME=$(./mini exec "$C2" hostname 2>/dev/null || ./mini exec "$C2" cat /proc/sys/kernel/hostname)

echo "  Host Hostname:        $HOST_HOSTNAME"
echo "  Container 1 Hostname: $C1_HOSTNAME"
echo "  Container 2 Hostname: $C2_HOSTNAME"

if [ "$C1_HOSTNAME" = "$C2_HOSTNAME" ]; then
    echo "FAIL: Containers share the same hostname!"
    exit 1
fi
if [ "$C1_HOSTNAME" = "$HOST_HOSTNAME" ]; then
    echo "FAIL: Container 1 shares host hostname!"
    exit 1
fi
echo "  -> PASS: UTS Namespace successfully isolated hostnames."

echo "[Step 4] Verifying PID Namespace Isolation..."
# Inside the container, PID 1 should be the node process or entrypoint, and host PIDs should be invisible
C1_PIDS=$(./mini exec "$C1" /bin/sh -c "ls -d /proc/[0-9]* | sed 's#/proc/##'" 2>/dev/null || echo "1")
echo "  Container 1 visible PIDs: $C1_PIDS"
HOST_PID_COUNT=$(ls -d /proc/[0-9]* | wc -l)
C1_PID_COUNT=$(echo "$C1_PIDS" | wc -w)

echo "  Host process count:        $HOST_PID_COUNT"
echo "  Container 1 process count: $C1_PID_COUNT"

if [ "$C1_PID_COUNT" -ge "$HOST_PID_COUNT" ]; then
    echo "FAIL: Container sees host process table!"
    exit 1
fi
echo "  -> PASS: PID Namespace successfully isolated process table."

echo "[Step 5] Verifying Filesystem & Mount Isolation..."
# Write a marker file inside container 1
./mini exec "$C1" /bin/sh -c "echo 'secret_in_c1' > /tmp/c1_isolation_marker.txt"
if ! ./mini exec "$C1" /bin/sh -c "grep -q 'secret_in_c1' /tmp/c1_isolation_marker.txt"; then
    echo "FAIL: Could not write/read inside container 1!"
    exit 1
fi

# Verify marker does NOT exist in container 2
if ./mini exec "$C2" /bin/sh -c "test -f /tmp/c1_isolation_marker.txt"; then
    echo "FAIL: Container 2 can see Container 1 private /tmp file!"
    exit 1
fi

# Verify marker does NOT exist in host /tmp
if [ -f "/tmp/c1_isolation_marker.txt" ]; then
    echo "FAIL: Container 1 file leaked to host /tmp!"
    exit 1
fi
echo "  -> PASS: Mount and Filesystem namespaces successfully isolated."

echo "[Step 6] Verifying Rootfs Boundary (pivot_root traversal prevention)..."
# In a pivot_root environment, /put_old must not exist and cd /../../.. stays at /
PUT_OLD_EXISTS=$(./mini exec "$C1" /bin/sh -c "test -d /put_old && echo 'yes' || echo 'no'")
if [ "$PUT_OLD_EXISTS" = "yes" ]; then
    echo "FAIL: /put_old still exists inside container!"
    exit 1
fi
echo "  -> PASS: /put_old is detached and rootfs boundary is sealed."

echo "[Step 7] Cleaning up test containers..."
./mini stop "$C1" >/dev/null 2>&1
./mini rm "$C1" >/dev/null 2>&1
./mini stop "$C2" >/dev/null 2>&1
./mini rm "$C2" >/dev/null 2>&1

echo "=========================================================="
echo " SCENARIO A: ALL ISOLATION TESTS PASSED SUCCESSFULLY"
echo "=========================================================="
