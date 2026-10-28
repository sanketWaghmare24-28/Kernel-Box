#!/bin/bash
set -euo pipefail

# Scenario E: Container Networking & Inter-Container Communication Test Suite
# Tests: Bridge kb0 setup, veth pair allocation, isolated network namespaces,
# direct inter-container IP communication (ping & HTTP), and targeted teardown.

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

if [ "$EUID" -ne 0 ]; then
    echo "Error: Scenario E test must be executed as root (sudo)."
    exit 1
fi

echo "=========================================================="
echo " SCENARIO E: CONTAINER NETWORKING TEST"
echo "=========================================================="

C_SRV="net-server"
C_CLI="net-client"

./mini rm "$C_SRV" -f >/dev/null 2>&1 || true
./mini rm "$C_CLI" -f >/dev/null 2>&1 || true

echo "[Step 1] Auditing Linux bridge kb0 and iptables configuration..."
if ip link show kb0 >/dev/null 2>&1; then
    echo "  Bridge kb0 exists: $(ip -br addr show kb0)"
else
    echo "  Bridge kb0 will be initialized on first container launch."
fi

echo "[Step 2] Launching container 1 (server on port 3991:3000)..."
CID_SRV=$(./mini run demo-api:v1 --name "$C_SRV" --publish 3991:3000 -d)
sleep 2

IP_SRV=$(./mini inspect "$C_SRV" | grep '"ip_address":' | head -n1 | awk '{print $2}' | tr -d '",')
echo "  Server Container IP: $IP_SRV"

echo "[Step 3] Launching container 2 (client on port 3992:3000)..."
CID_CLI=$(./mini run demo-api:v1 --name "$C_CLI" --publish 3992:3000 -d)
sleep 2

IP_CLI=$(./mini inspect "$C_CLI" | grep '"ip_address":' | head -n1 | awk '{print $2}' | tr -d '",')
echo "  Client Container IP: $IP_CLI"

if [ "$IP_SRV" = "$IP_CLI" ]; then
    echo "FAIL: Containers were allocated duplicate IP addresses: $IP_SRV"
    exit 1
fi
echo "  -> PASS: Unique IP addresses allocated from pool 10.0.100.2-254."

echo "[Step 4] Testing Inter-Container ICMP Communication (ping from client to server)..."
set +e
PING_OUT=$(./mini exec "$C_CLI" ping -c 2 -W 1 "$IP_SRV" 2>&1)
PING_RET=$?
set -e
echo "  Ping output:"
echo "$PING_OUT" | sed 's/^/    /'

if [ "$PING_RET" -ne 0 ]; then
    echo "FAIL: ICMP ping between containers across kb0 bridge failed!"
    exit 1
fi
echo "  -> PASS: ICMP traffic routes across isolated namespaces over bridge kb0."

echo "[Step 5] Testing Inter-Container Application HTTP Communication..."
set +e
HTTP_RESP=$(./mini exec "$C_CLI" /bin/sh -c "wget -qO- http://$IP_SRV:3000/health" 2>&1)
HTTP_RET=$?
set -e
echo "  Direct container-to-container HTTP response:"
echo "    $HTTP_RESP"

if [ "$HTTP_RET" -ne 0 ] || ! echo "$HTTP_RESP" | grep -q "healthy"; then
    echo "FAIL: Inter-container HTTP request across virtual network failed!"
    exit 1
fi
echo "  -> PASS: Direct container-to-container HTTP communication succeeded."

echo "[Step 6] Testing targeted removal of client container..."
./mini stop "$C_CLI" >/dev/null 2>&1
./mini rm "$C_CLI" >/dev/null 2>&1

VETH_CLI="veth${CID_CLI:0:6}h"
if ip link show "$VETH_CLI" >/dev/null 2>&1; then
    echo "FAIL: Client veth $VETH_CLI was not deleted!"
    exit 1
fi
echo "  -> PASS: Client veth interface $VETH_CLI removed."

echo "[Step 7] Verifying server container ($C_SRV) remains completely unaffected..."
if ! ./mini ps | grep -q "Running.*$C_SRV"; then
    echo "FAIL: Server container died when client was removed!"
    exit 1
fi

SRV_RESP=$(curl -s http://127.0.0.1:3991/health || echo "fail")
echo "  Server response via host port 3991: $SRV_RESP"
if ! echo "$SRV_RESP" | grep -q "healthy"; then
    echo "FAIL: Server container became unreachable after client teardown!"
    exit 1
fi
echo "  -> PASS: Server container remains operational and isolated."

echo "[Step 8] Cleaning up server container..."
./mini stop "$C_SRV" >/dev/null 2>&1
./mini rm "$C_SRV" >/dev/null 2>&1

echo "=========================================================="
echo " SCENARIO E: ALL CONTAINER NETWORKING TESTS PASSED"
echo "=========================================================="
