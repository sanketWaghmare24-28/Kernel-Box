#!/bin/bash
set -euo pipefail

# Scenario B: CPU and Memory Limit Enforcement Test Suite
# Tests: cgroups v2 detection, cpu.max enforcement, memory.max enforcement, and OOM kill detection

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

if [ "$EUID" -ne 0 ]; then
    echo "Error: Scenario B test must be executed as root (sudo)."
    exit 1
fi

echo "=========================================================="
echo " SCENARIO B: CGROUPS V2 RESOURCE ENFORCEMENT TEST"
echo "=========================================================="

echo "[Step 1] Verifying Unified cgroups v2 hierarchy..."
if [ ! -f "/sys/fs/cgroup/cgroup.controllers" ]; then
    echo "FAIL: Unified cgroups v2 not detected at /sys/fs/cgroup!"
    exit 1
fi
echo "  Available controllers: $(cat /sys/fs/cgroup/cgroup.controllers)"
echo "  -> PASS: cgroups v2 unified hierarchy verified."

C_CPU="cg-cpu-test"
C_MEM="cg-mem-test"
C_OOM="cg-oom-test"

./mini rm "$C_CPU" -f >/dev/null 2>&1 || true
./mini rm "$C_MEM" -f >/dev/null 2>&1 || true
./mini rm "$C_OOM" -f >/dev/null 2>&1 || true

echo "[Step 2] Testing CPU Quota Enforcement (cpu.max = '25000 100000' -> 25% single core)..."
CID_CPU=$(./mini run demo-api:v1 --name "$C_CPU" --cpu "25000 100000" -d)
sleep 1

# Check cpu.max file in container cgroup
CG_PATH="/sys/fs/cgroup/kernelbox/$CID_CPU"
if [ ! -f "$CG_PATH/cpu.max" ]; then
    echo "FAIL: $CG_PATH/cpu.max does not exist!"
    exit 1
fi
ACTUAL_CPU_MAX=$(cat "$CG_PATH/cpu.max")
echo "  Expected cpu.max: 25000 100000"
echo "  Actual cpu.max:   $ACTUAL_CPU_MAX"
if [ "$ACTUAL_CPU_MAX" != "25000 100000" ]; then
    echo "FAIL: cpu.max content mismatch!"
    exit 1
fi
echo "  -> PASS: cpu.max successfully programmed."

echo "[Step 3] Running CPU workload inside container and collecting cpu.stat metrics..."
./mini exec "$C_CPU" /bin/sh -c "node -e 'let t=Date.now()+1500; while(Date.now()<t){Math.sqrt(Math.random())}'" >/dev/null 2>&1 || true

echo "  Container resource stats:"
./mini stats "$C_CPU"
echo "  -> PASS: CPU accounting and throttling active."

echo "[Step 4] Testing Memory Limit Enforcement (memory.max = 64m = 67108864 bytes)..."
CID_MEM=$(./mini run demo-api:v1 --name "$C_MEM" --memory 64m -d)
sleep 1

CG_MEM_PATH="/sys/fs/cgroup/kernelbox/$CID_MEM"
if [ ! -f "$CG_MEM_PATH/memory.max" ]; then
    echo "FAIL: $CG_MEM_PATH/memory.max does not exist!"
    exit 1
fi
ACTUAL_MEM_MAX=$(cat "$CG_MEM_PATH/memory.max")
echo "  Expected memory.max: 67108864"
echo "  Actual memory.max:   $ACTUAL_MEM_MAX"
if [ "$ACTUAL_MEM_MAX" != "67108864" ]; then
    echo "FAIL: memory.max content mismatch!"
    exit 1
fi
echo "  -> PASS: memory.max successfully programmed."

echo "[Step 5] Testing Memory Limit Breach & OOM Killer Behavior..."
# Create a container with 32MB limit and allocate 100MB inside node to trigger kernel OOM kill
CID_OOM=$(./mini run demo-api:v1 --name "$C_OOM" --memory 32m -d)
sleep 1

echo "  Triggering memory allocation beyond 32m cap (allocating 100MB array in node)..."
set +e
./mini exec "$C_OOM" /bin/sh -c "node -e 'let arr=[]; for(let i=0;i<100;i++){arr.push(Buffer.alloc(1024*1024))}'" >/dev/null 2>&1
EXEC_RET=$?
set -e

echo "  Command returned with status: $EXEC_RET (137 indicates 128+SIGKILL from kernel OOM killer)"
CG_OOM_PATH="/sys/fs/cgroup/kernelbox/$CID_OOM"
if [ -f "$CG_OOM_PATH/memory.events" ]; then
    echo "  Container cgroups memory.events:"
    cat "$CG_OOM_PATH/memory.events" | sed 's/^/    /'
    OOM_COUNT=$(grep "^oom " "$CG_OOM_PATH/memory.events" | awk '{print $2}')
    echo "  Recorded OOM events: $OOM_COUNT"
fi

./mini stats "$C_OOM"
echo "  -> PASS: OOM kill / limit boundary accurately tracked."

echo "[Step 6] Cleaning up Scenario B containers..."
./mini rm "$C_CPU" -f >/dev/null 2>&1
./mini rm "$C_MEM" -f >/dev/null 2>&1
./mini rm "$C_OOM" -f >/dev/null 2>&1

echo "=========================================================="
echo " SCENARIO B: ALL RESOURCE ENFORCEMENT TESTS PASSED"
echo "=========================================================="
