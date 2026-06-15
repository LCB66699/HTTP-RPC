#!/bin/sh
# Redis Cluster init — wait for all nodes, handle IP drift, idempotent
set -e

PW="${REDIS_PASSWORD:-rpc-redis-123456}"
SEEDS="redis-cluster-1:7000 redis-cluster-2:7001 redis-cluster-3:7002 redis-cluster-4:7003 redis-cluster-5:7004 redis-cluster-6:7005"
NODE_COUNT=6
MAX_WAIT=60

# ---- 0. Wait for all nodes to be reachable ----
echo "[init] waiting for all $NODE_COUNT nodes..."
elapsed=0
while [ $elapsed -lt $MAX_WAIT ]; do
    ready=0
    for i in $(seq 1 $NODE_COUNT); do
        port=$((7000 + i - 1))
        if redis-cli -h "redis-cluster-$i" -p $port -a "$PW" --no-auth-warning ping 2>/dev/null | grep -q PONG; then
            ready=$((ready + 1))
        fi
    done
    if [ $ready -eq $NODE_COUNT ]; then
        echo "[init] all $NODE_COUNT nodes ready"
        break
    fi
    echo "[init] $ready/$NODE_COUNT ready, waiting..."
    sleep 2
    elapsed=$((elapsed + 2))
done

if [ $elapsed -ge $MAX_WAIT ]; then
    echo "[init] timeout waiting for nodes"
    exit 1
fi

# ---- 1. Check if cluster is already healthy ----
if redis-cli -c -h redis-cluster-1 -p 7000 -a "$PW" --no-auth-warning cluster info 2>/dev/null | grep -q 'cluster_state:ok'; then
    echo "=== Redis Cluster healthy ==="
    exit 0
fi

echo "[init] cluster not healthy, checking for stale IPs..."

# ---- 2. Compare DNS IP vs nodes.conf IP per node ----
STALE=0
for i in $(seq 1 $NODE_COUNT); do
    port=$((7000 + i - 1))
    node="redis-cluster-$i"
    resolved=$(getent hosts "$node" 2>/dev/null | awk '{print $1; exit}')
    cluster_info=$(redis-cli -h "$node" -p $port -a "$PW" --no-auth-warning CLUSTER NODES 2>/dev/null | grep myself || true)
    cluster_ip=$(echo "$cluster_info" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1)

    if [ -n "$resolved" ] && [ -n "$cluster_ip" ] && [ "$resolved" != "$cluster_ip" ]; then
        echo "[init] IP mismatch on $node: stored=$cluster_ip dns=$resolved"
        STALE=1
    fi
done

# ---- 3. IP drift → full reset ----
if [ $STALE -eq 1 ]; then
    echo "[init] stale IPs detected, resetting all nodes..."
    for i in $(seq 1 $NODE_COUNT); do
        port=$((7000 + i - 1))
        redis-cli -h "redis-cluster-$i" -p $port -a "$PW" --no-auth-warning FLUSHALL 2>/dev/null || true
        redis-cli -h "redis-cluster-$i" -p $port -a "$PW" --no-auth-warning CLUSTER RESET HARD 2>/dev/null || true
    done
    sleep 2
fi

# ---- 4. Create cluster (idempotent — tolerates "already known" nodes) ----
echo "[init] creating cluster..."
for attempt in 1 2 3; do
    if echo yes | redis-cli --cluster create $SEEDS --cluster-replicas 1 -a "$PW" --no-auth-warning 2>&1; then
        echo "=== Redis Cluster initialized ==="
        exit 0
    fi
    # If cluster already formed by another init, check and exit cleanly
    if redis-cli -c -h redis-cluster-1 -p 7000 -a "$PW" --no-auth-warning cluster info 2>/dev/null | grep -q 'cluster_state:ok'; then
        echo "=== Redis Cluster already healthy ==="
        exit 0
    fi
    echo "[init] create attempt $attempt failed, retrying..."
    sleep 3
done

echo "[init] FATAL: cluster creation failed after 3 attempts"
exit 1
