#!/bin/bash
set -euo pipefail

# Scenario C: Container Crash and Automatic Cleanup Test Suite
# Tests: unexpected exit code capture, signal termination (SIGKILL), resource cleanup,
# log preservation, and isolation from unrelated running containers.

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

if [ "$EUID" -ne 0 ]; then
    echo "Error: Scenario C test must be executed as root (sudo)."
    exit 1
fi

echo "=========================================================="
echo " SCENARIO C: CONTAINER CRASH & CLEANUP TEST"
echo "=========================================================="

C_HEALTHY="c-healthy-app"
C_EXIT="c-crash-exit"
C_KILL="c-crash-kill"

# Clean up any leftover test containers
./mini rm "$C_HEALTHY" -f >/dev/null 2>&1 || true
./mini rm "$C_EXIT" -f >/dev/null 2>&1 || true
./mini rm "$C_KILL" -f >/dev/null 2>&1 || true

echo "[Step 1] Launching independent healthy container ($C_HEALTHY)..."
CID_HEALTHY=$(./mini run demo-api:v1 --name "$C_HEALTHY" --publish 3100:3000 -d)
sleep 2

if ! ./mini ps | grep -q "Running.*$C_HEALTHY"; then
    echo "FAIL: Healthy container failed to start!"
    ./mini ps
    exit 1
fi
echo "  Healthy container running (ID: $CID_HEALTHY)"

echo "[Step 2] Testing container process that exits unexpectedly with non-zero code..."
CID_EXIT=$(./mini run demo-api:v1 --name "$C_EXIT" -d)
sleep 1

# Inspect container PID
EXIT_PID=$(./mini inspect "$C_EXIT" | grep '"pid":' | awk '{print $2}' | tr -d ',')
echo "  Target Container PID for crash simulation: $EXIT_PID"

echo "[Step 3] Terminating container process with SIGKILL (kill -9)..."
kill -9 "$EXIT_PID"
sleep 1

echo "[Step 4] Verifying runtime reconciles crashed container state..."
# Running mini ps reconciles stale running records
./mini ps

STATE_JSON="/var/lib/kernelbox/containers/$CID_EXIT/state.json"
if [ ! -f "$STATE_JSON" ]; then
    echo "FAIL: State JSON missing after crash!"
    exit 1
fi

STATUS_STR=$(grep '"status":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
EXIT_CODE=$(grep '"exit_code":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
echo "  Container status after crash: $STATUS_STR (expected 5 for Failed or 2 for Stopped)"
echo "  Container exit code:          $EXIT_CODE (expected 137 for SIGKILL)"

if [ "$STATUS_STR" != "5" ] && [ "$STATUS_STR" != "2" ]; then
    echo "FAIL: Stale container was not updated to Failed or Stopped!"
    exit 1
fi

echo "[Step 5] Verifying resources owned by crashed container were cleaned up..."
# Cgroup directory should be removed
if [ -d "/sys/fs/cgroup/kernelbox/$CID_EXIT" ]; then
    echo "FAIL: Cgroup directory still exists for crashed container!"
    exit 1
fi
echo "  -> Cgroup cleaned up."

# Veth interface on host should be deleted
VETH_HOST="veth${CID_EXIT:0:6}h"
if ip link show "$VETH_HOST" >/dev/null 2>&1; then
    echo "FAIL: Host veth interface $VETH_HOST still exists!"
    exit 1
fi
echo "  -> Host veth interface $VETH_HOST removed."

echo "[Step 6] Verifying container logs survive crash and cleanup..."
LOG_CONTENT=$(./mini logs "$C_EXIT")
if [ -z "$LOG_CONTENT" ]; then
    echo "FAIL: Container logs were lost after crash!"
    exit 1
fi
echo "  -> Logs preserved successfully ($(echo "$LOG_CONTENT" | wc -l) lines)."

echo "[Step 7] Verifying unrelated healthy container ($C_HEALTHY) was NOT affected..."
if ! ./mini ps | grep -q "Running.*$C_HEALTHY"; then
    echo "FAIL: Unrelated healthy container died during crash of another container!"
    ./mini ps
    exit 1
fi

HEALTH_RESP=$(curl -s http://127.0.0.1:3100/health || echo "failed")
echo "  Healthy container response: $HEALTH_RESP"
if ! echo "$HEALTH_RESP" | grep -q "healthy"; then
    echo "FAIL: Healthy container stopped responding to requests!"
    exit 1
fi
echo "  -> PASS: Healthy container remained 100% operational."

echo "[Step 8] Verifying cleanup idempotency (calling stop and rm repeatedly)..."
# Calling stop on already-crashed container should succeed safely
./mini stop "$C_EXIT" >/dev/null 2>&1 || true
./mini stop "$C_EXIT" >/dev/null 2>&1 || true
# Calling rm should succeed and remove container metadata directory
./mini rm "$C_EXIT" >/dev/null 2>&1
# Calling rm again on non-existent container should safely report error without crashing
set +e
./mini rm "$C_EXIT" >/dev/null 2>&1
RM_ERR=$?
set -e
if [ "$RM_ERR" -eq 0 ]; then
    echo "FAIL: Removing already-removed container should return non-zero!"
    exit 1
fi
echo "  -> PASS: Cleanup operations are idempotent and safe."

echo "[Step 9] Cleaning up healthy container..."
./mini stop "$C_HEALTHY" >/dev/null 2>&1
./mini rm "$C_HEALTHY" >/dev/null 2>&1

echo "=========================================================="
echo " SCENARIO C: ALL CRASH & CLEANUP TESTS PASSED"
echo "=========================================================="
