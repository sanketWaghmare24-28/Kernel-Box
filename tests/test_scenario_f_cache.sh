#!/usr/bin/env bash
# tests/test_scenario_f_cache.sh — Scenario F Cache & Rootfs Optimization Verification

set -euo pipefail

RUNTIME="./mini"
IMAGE="demo-api:v1"
SANITIZED_IMAGE="demo-api_v1"
IMAGE_DIR="/var/lib/kernelbox/images/${SANITIZED_IMAGE}"
CACHE_DIR="${IMAGE_DIR}/.rootfs_cache"
STAMP_FILE="${IMAGE_DIR}/.cache_stamp"

PASS_COUNT=0
FAIL_COUNT=0

pass() {
    echo -e "\033[32m[PASS]\033[0m $1"
    PASS_COUNT=$((PASS_COUNT + 1))
}

fail() {
    echo -e "\033[31m[FAIL]\033[0m $1"
    FAIL_COUNT=$((FAIL_COUNT + 1))
}

cleanup() {
    echo "Cleaning up test containers..."
    # Restore permissions if test 7 modified them
    chmod -R 755 "$CACHE_DIR" 2>/dev/null || true
    for c in test-f-c1 test-f-c2 test-f-c3 test-f-conc-1 test-f-conc-2 test-f-conc-3 test-f-cold-1 test-f-cold-2 test-f-cold-3 test-f-fallback test-f-copy-fail test-f-build-inv; do
        $RUNTIME stop "$c" 2>/dev/null || true
        $RUNTIME rm -f "$c" 2>/dev/null || true
    done
}
trap cleanup EXIT
cleanup

echo "=== SCENARIO F: ROOTFS TEMPLATE CACHE VERIFICATION ==="

# -----------------------------------------------------------------------------
# Test 1: Clean slate & initial cache creation
# -----------------------------------------------------------------------------
echo "--- Test 1: First container creates rootfs cache ---"
rm -rf "$CACHE_DIR" "$STAMP_FILE"

C1_ID=$($RUNTIME create "$IMAGE" --name test-f-c1)
if [ -n "$C1_ID" ] && [ -d "$CACHE_DIR" ] && [ -f "$STAMP_FILE" ]; then
    pass "Container 1 created and rootfs cache + stamp generated"
else
    fail "Container 1 failed to create cache or stamp"
fi

STAMP_V1=$(cat "$STAMP_FILE")
if [ -n "$STAMP_V1" ]; then
    pass "Cache stamp recorded: $STAMP_V1"
else
    fail "Cache stamp file is empty"
fi

# -----------------------------------------------------------------------------
# Test 2: Cache hit on second container
# -----------------------------------------------------------------------------
echo "--- Test 2: Second container creates from cache ---"
C2_ID=$($RUNTIME create "$IMAGE" --name test-f-c2)
if [ -n "$C2_ID" ]; then
    pass "Container 2 created successfully"
else
    fail "Container 2 failed to create"
fi

# -----------------------------------------------------------------------------
# Test 3: Mutation isolation (Independent writable rootfs)
# -----------------------------------------------------------------------------
echo "--- Test 3: Mutation isolation ---"
$RUNTIME start test-f-c1
$RUNTIME start test-f-c2
sleep 1

C1_ROOTFS="/var/lib/kernelbox/containers/${C1_ID}/rootfs"
C2_ROOTFS="/var/lib/kernelbox/containers/${C2_ID}/rootfs"

# Create file in C1
echo "secret_data_container_1" > "${C1_ROOTFS}/app/mutation_marker.txt"

# Verify C1 has it
if [ -f "${C1_ROOTFS}/app/mutation_marker.txt" ]; then
    pass "Mutation marker present in container 1"
else
    fail "Mutation marker missing in container 1"
fi

# Verify C2 DOES NOT have it
if [ ! -f "${C2_ROOTFS}/app/mutation_marker.txt" ]; then
    pass "Mutation marker NOT present in container 2 (isolation preserved)"
else
    fail "LEAK: Mutation marker found in container 2!"
fi

# Verify Cache template DOES NOT have it
if [ ! -f "${CACHE_DIR}/app/mutation_marker.txt" ]; then
    pass "Mutation marker NOT present in cache template"
else
    fail "CORRUPTION: Mutation marker leaked into cache template!"
fi

# Modify existing file in C1
echo "tampered" >> "${C1_ROOTFS}/app/server.js"
if ! grep -q "tampered" "${C2_ROOTFS}/app/server.js" && ! grep -q "tampered" "${CACHE_DIR}/app/server.js"; then
    pass "Existing file modification in C1 did not mutate C2 or cache template"
else
    fail "Cross-container contamination detected in server.js"
fi

$RUNTIME stop test-f-c1
$RUNTIME stop test-f-c2

# -----------------------------------------------------------------------------
# Test 4: Cache invalidation on image update (mtime/touch)
# -----------------------------------------------------------------------------
echo "--- Test 4: Cache invalidation upon image modification ---"
sleep 1
touch "${IMAGE_DIR}/rootfs.tar.gz"

C3_ID=$($RUNTIME create "$IMAGE" --name test-f-c3)
STAMP_V2=$(cat "$STAMP_FILE")

if [ "$STAMP_V1" != "$STAMP_V2" ]; then
    pass "Cache invalidated and updated with new stamp: $STAMP_V2"
else
    fail "Cache stamp did not change after image tarball update"
fi

# -----------------------------------------------------------------------------
# Test 5: Cache invalidation on 'mini build'
# -----------------------------------------------------------------------------
echo "--- Test 5: Cache invalidation on image build ---"
# Rebuild image from context
$RUNTIME build examples/node-api -t "$IMAGE" > /dev/null
if [ ! -d "$CACHE_DIR" ] && [ ! -f "$STAMP_FILE" ]; then
    pass "Image build cleanly purged rootfs cache and stamp"
else
    fail "Image build failed to purge old cache or stamp"
fi

# -----------------------------------------------------------------------------
# Test 6: Cold concurrent cache creation race
# -----------------------------------------------------------------------------
echo "--- Test 6: Cold concurrent cache creation race (flock contention) ---"
# Ensure cache is completely wiped
rm -rf "$CACHE_DIR" "$STAMP_FILE"

PIDS=()
for i in 1 2 3; do
    $RUNTIME create "$IMAGE" --name "test-f-cold-$i" > /dev/null &
    PIDS+=($!)
done

COLD_FAILED=0
for pid in "${PIDS[@]}"; do
    if ! wait "$pid"; then
        COLD_FAILED=$((COLD_FAILED + 1))
    fi
done

if [ "$COLD_FAILED" -eq 0 ] && [ -d "$CACHE_DIR" ] && [ -f "$STAMP_FILE" ]; then
    pass "All 3 cold concurrent container creations succeeded under flock contention"
else
    fail "Cold concurrent cache race failed ($COLD_FAILED containers failed)"
fi

# -----------------------------------------------------------------------------
# Test 7: Warm concurrent container creation
# -----------------------------------------------------------------------------
echo "--- Test 7: Warm concurrent container creation (shared flock) ---"
PIDS=()
for i in 1 2 3; do
    $RUNTIME create "$IMAGE" --name "test-f-conc-$i" > /dev/null &
    PIDS+=($!)
done

WARM_FAILED=0
for pid in "${PIDS[@]}"; do
    if ! wait "$pid"; then
        WARM_FAILED=$((WARM_FAILED + 1))
    fi
done

if [ "$WARM_FAILED" -eq 0 ]; then
    pass "All 3 warm concurrent container creations succeeded"
else
    fail "Warm concurrent creations failed ($WARM_FAILED containers failed)"
fi

# -----------------------------------------------------------------------------
# Test 8: Fallback when cache directory is empty/broken
# -----------------------------------------------------------------------------
echo "--- Test 8: Fallback when cache directory is empty ---"
rm -rf "${CACHE_DIR:?}"/*
FALLBACK_ID=$($RUNTIME create "$IMAGE" --name test-f-fallback)
if [ -n "$FALLBACK_ID" ] && [ -f "/var/lib/kernelbox/containers/${FALLBACK_ID}/rootfs/app/server.js" ]; then
    pass "Container created successfully via automatic fallback when cache was cleared"
else
    fail "Fallback failed when cache was empty"
fi

# -----------------------------------------------------------------------------
# Test 9: Fallback when copy fails (permissions error during cp)
# -----------------------------------------------------------------------------
echo "--- Test 9: Fallback when cp -a fails mid-copy ---"
# Re-populate cache
$RUNTIME create "$IMAGE" --name test-f-c1 >/dev/null 2>&1 || true
# Make cache root unreadable so cp -a fails immediately
chmod 000 "$CACHE_DIR"
FAIL_ID=$($RUNTIME create "$IMAGE" --name test-f-copy-fail)
# Restore permissions immediately
chmod 755 "$CACHE_DIR"

if [ -n "$FAIL_ID" ] && [ -f "/var/lib/kernelbox/containers/${FAIL_ID}/rootfs/app/server.js" ]; then
    pass "Container created successfully via fallback to tar extraction when cp -a failed"
else
    fail "Fallback recovery failed when cp -a failed"
fi

echo "=========================================="
echo "Scenario F tests completed: $PASS_COUNT passed, $FAIL_COUNT failed."
if [ "$FAIL_COUNT" -eq 0 ]; then
    exit 0
else
    exit 1
fi
