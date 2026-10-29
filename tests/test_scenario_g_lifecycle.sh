#!/bin/bash
set -euo pipefail

# Scenario G: Container Lifecycle & State Management Test Suite
# Tests: Decoupled create/start, duplicate start rejection, pause/unpause,
# restart with PID change, rm without force rejection, duplicate name rejection,
# metadata/log persistence after exit, stale-state reconciliation, and clean removal.

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

if [ "$EUID" -ne 0 ]; then
    echo "Error: Scenario G test must be executed as root (sudo)."
    exit 1
fi

echo "=========================================================="
echo " SCENARIO G: CONTAINER LIFECYCLE & STATE MANAGEMENT TEST"
echo "=========================================================="

C_NAME="g-life-primary"
C_STALE="g-life-stale"

cleanup() {
    ./mini stop "$C_NAME" >/dev/null 2>&1 || true
    ./mini rm "$C_NAME" -f >/dev/null 2>&1 || true
    ./mini stop "$C_STALE" >/dev/null 2>&1 || true
    ./mini rm "$C_STALE" -f >/dev/null 2>&1 || true
}
trap cleanup EXIT

# Pre-test cleanup
cleanup

echo "[Step 1] Ensuring demo image is available..."
if ! ./mini images | grep -q "demo-api"; then
    ./mini build examples/node-api -t demo-api:v1
fi

echo "[Step 2] Testing 'mini create' (prepare rootfs & metadata without process execution)..."
CID_PRIMARY=$(./mini create demo-api:v1 --name "$C_NAME" --memory 64m --publish 3995:3000)
echo "  Created Container ID: $CID_PRIMARY"

STATE_JSON="/var/lib/kernelbox/containers/$CID_PRIMARY/state.json"
if [ ! -f "$STATE_JSON" ]; then
    echo "FAIL: state.json was not created during mini create!"
    exit 1
fi

STATUS_CREATED=$(grep '"status":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
echo "  Container Status after create: $STATUS_CREATED (expected 0 for Created)"
if [ "$STATUS_CREATED" -ne 0 ]; then
    echo "FAIL: Expected status 0 (Created), got $STATUS_CREATED"
    exit 1
fi

PID_CREATED=$(grep '"pid":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
if [ "$PID_CREATED" -ne 0 ]; then
    echo "FAIL: Process PID was assigned before mini start: $PID_CREATED"
    exit 1
fi

if [ ! -d "/var/lib/kernelbox/containers/$CID_PRIMARY/rootfs" ]; then
    echo "FAIL: Container rootfs directory was not created!"
    exit 1
fi
echo "  -> PASS: 'mini create' created rootfs and metadata in state Created (0) with PID 0."

echo "[Step 3] Testing 'mini start' on previously created container..."
./mini start "$C_NAME"
sleep 2

STATUS_STARTED=$(grep '"status":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
PID_STARTED=$(grep '"pid":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
echo "  Container Status after start: $STATUS_STARTED (expected 1 for Running)"
echo "  Container PID:                $PID_STARTED"

if [ "$STATUS_STARTED" -ne 1 ] || [ "$PID_STARTED" -le 0 ]; then
    echo "FAIL: Container did not transition to Running (1) with valid PID!"
    exit 1
fi

if ! kill -0 "$PID_STARTED" 2>/dev/null; then
    echo "FAIL: Container PID $PID_STARTED is not running on host!"
    exit 1
fi

RESP_HEALTH=$(curl -s http://127.0.0.1:3995/health || echo "fail")
if ! echo "$RESP_HEALTH" | grep -q "healthy"; then
    echo "FAIL: Started container is not responding to HTTP requests on port 3995!"
    exit 1
fi
echo "  -> PASS: 'mini start' successfully spawned process, configured networking, and transitioned to Running (1)."

echo "[Step 4] Testing duplicate start rejection on already running container..."
./mini start "$C_NAME"
PID_AFTER_DUP=$(grep '"pid":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
if [ "$PID_AFTER_DUP" -ne "$PID_STARTED" ]; then
    echo "FAIL: Duplicate start modified container PID!"
    exit 1
fi
echo "  -> PASS: Duplicate start rejected without modifying container state."

echo "[Step 5] Testing 'mini pause' and 'mini unpause'..."
./mini pause "$C_NAME"
STATUS_PAUSED=$(grep '"status":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
echo "  Container Status after pause: $STATUS_PAUSED (expected 4 for Paused)"
if [ "$STATUS_PAUSED" -ne 4 ]; then
    echo "FAIL: Expected status 4 (Paused), got $STATUS_PAUSED"
    exit 1
fi

# Verify cgroups freeze state
if [ -f "/sys/fs/cgroup/kernelbox/$CID_PRIMARY/cgroup.freeze" ]; then
    FREEZE_VAL=$(cat "/sys/fs/cgroup/kernelbox/$CID_PRIMARY/cgroup.freeze")
    if [ "$FREEZE_VAL" != "1" ]; then
        echo "FAIL: cgroup.freeze is not 1 when paused!"
        exit 1
    fi
fi

./mini unpause "$C_NAME"
STATUS_RESUMED=$(grep '"status":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
echo "  Container Status after unpause: $STATUS_RESUMED (expected 1 for Running)"
if [ "$STATUS_RESUMED" -ne 1 ]; then
    echo "FAIL: Expected status 1 (Running) after unpause, got $STATUS_RESUMED"
    exit 1
fi
echo "  -> PASS: 'mini pause' and 'mini unpause' transition through Paused (4) and Running (1)."

echo "[Step 6] Testing 'mini restart' (spawns fresh process with new PID)..."
./mini restart "$C_NAME"
sleep 2

PID_RESTART=$(grep '"pid":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
STATUS_RESTART=$(grep '"status":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
echo "  Old PID: $PID_STARTED"
echo "  New PID: $PID_RESTART"

if [ "$STATUS_RESTART" -ne 1 ]; then
    echo "FAIL: Container is not Running after restart!"
    exit 1
fi
if [ "$PID_RESTART" -eq "$PID_STARTED" ]; then
    echo "FAIL: Restart did not spawn a fresh process (PID unchanged)!"
    exit 1
fi
echo "  -> PASS: 'mini restart' cleanly stopped old process and spawned new PID $PID_RESTART."

echo "[Step 7] Testing rejection of 'mini rm' on running container without --force..."
set +e
ERR_RM=$(./mini rm "$C_NAME" 2>&1)
RET_RM=$?
set -e
if [ "$RET_RM" -eq 0 ]; then
    echo "FAIL: 'mini rm' without -f unexpectedly succeeded on running container!"
    exit 1
fi
if ! echo "$ERR_RM" | grep -qi "force"; then
    echo "FAIL: Error message did not mention '--force' or '-f': $ERR_RM"
    exit 1
fi
echo "  -> PASS: 'mini rm' correctly rejected removal of running container."

echo "[Step 8] Testing rejection of duplicate container name..."
set +e
ERR_DUP=$(./mini create demo-api:v1 --name "$C_NAME" 2>&1)
RET_DUP=$?
set -e
if [ "$RET_DUP" -eq 0 ]; then
    echo "FAIL: 'mini create' accepted an already-existing container name!"
    exit 1
fi
echo "  -> PASS: Duplicate container name correctly rejected with error."

echo "[Step 9] Testing 'mini stop' and metadata/log persistence after exit..."
./mini stop "$C_NAME"

STATUS_STOPPED=$(grep '"status":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
EXIT_CODE=$(grep '"exit_code":' "$STATE_JSON" | awk '{print $2}' | tr -d ',')
echo "  Container Status after stop: $STATUS_STOPPED (expected 2 for Stopped or 5 for Failed)"
echo "  Recorded Exit Code:          $EXIT_CODE"

LOGS_POST_STOP=$(./mini logs "$C_NAME")
if [ -z "$LOGS_POST_STOP" ]; then
    echo "FAIL: Container logs vanished after container stop!"
    exit 1
fi
echo "  Logs preserved: $(echo "$LOGS_POST_STOP" | wc -l) lines."

if ! grep -q '"finished_at":' "$STATE_JSON"; then
    echo "FAIL: finished_at timestamp was not recorded in state.json!"
    exit 1
fi
echo "  -> PASS: Stopped container persists exit status, finished_at, and logs."

echo "[Step 10] Testing stale-state reconciliation on crashed container..."
CID_STALE=$(./mini run demo-api:v1 --name "$C_STALE" -d)
sleep 1
STALE_PID=$(./mini inspect "$C_STALE" | grep '"pid":' | awk '{print $2}' | tr -d ',')
echo "  Target Stale PID: $STALE_PID"
kill -9 "$STALE_PID"
sleep 1

# Reconcile via ps
./mini ps >/dev/null

STATE_STALE="/var/lib/kernelbox/containers/$CID_STALE/state.json"
STATUS_RECONCILED=$(grep '"status":' "$STATE_STALE" | awk '{print $2}' | tr -d ',')
EXIT_RECONCILED=$(grep '"exit_code":' "$STATE_STALE" | awk '{print $2}' | tr -d ',')
echo "  Reconciled Status: $STATUS_RECONCILED (expected 5 for Failed)"
echo "  Reconciled Exit:   $EXIT_RECONCILED (expected 137 for SIGKILL)"

if [ "$STATUS_RECONCILED" -ne 5 ] || [ "$EXIT_RECONCILED" -ne 137 ]; then
    echo "FAIL: Stale container was not reconciled to status 5 / exit 137!"
    exit 1
fi
echo "  -> PASS: Stale-state reconciled to Failed (5) with exit code 137."

echo "[Step 11] Testing final removal of stopped and failed containers..."
./mini rm "$C_NAME"
./mini rm "$C_STALE"

if [ -d "/var/lib/kernelbox/containers/$CID_PRIMARY" ]; then
    echo "FAIL: Primary container directory still exists after rm!"
    exit 1
fi
if [ -d "/var/lib/kernelbox/containers/$CID_STALE" ]; then
    echo "FAIL: Stale container directory still exists after rm!"
    exit 1
fi
echo "  -> PASS: Containers completely removed from storage."

echo "=========================================================="
echo " SCENARIO G: ALL LIFECYCLE & STATE TESTS PASSED"
echo "=========================================================="
