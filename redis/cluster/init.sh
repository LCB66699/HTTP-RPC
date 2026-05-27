#!/bin/sh
# Redis Cluster 智能初始化 — 自动检测并修复 IP 漂移
set -e

PW="${REDIS_PASSWORD:-rpc-redis-123456}"
SEEDS="redis-cluster-1:7000 redis-cluster-2:7001 redis-cluster-3:7002 redis-cluster-4:7003 redis-cluster-5:7004 redis-cluster-6:7005"

sleep 3

# ---- 1. 检查集群是否健康 ----
if redis-cli -c -h redis-cluster-1 -p 7000 -a "$PW" --no-auth-warning cluster info 2>/dev/null | grep -q 'cluster_state:ok'; then
    echo "=== Redis Cluster healthy ==="
    exit 0
fi

echo "[init] cluster unhealthy, checking for stale IPs..."

# ---- 2. 逐节点比对 DNS IP vs nodes.conf IP ----
STALE=0
for i in 1 2 3 4 5 6; do
    PORT=$((7000 + i - 1))
    NODE="redis-cluster-$i"
    RESOLVED=$(getent hosts "$NODE" 2>/dev/null | awk '{print $1; exit}')
    CLUSTER_INFO=$(redis-cli -h "$NODE" -p $PORT -a "$PW" --no-auth-warning CLUSTER NODES 2>/dev/null | grep myself || true)
    CLUSTER_IP=$(echo "$CLUSTER_INFO" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1)

    if [ -n "$RESOLVED" ] && [ -n "$CLUSTER_IP" ] && [ "$RESOLVED" != "$CLUSTER_IP" ]; then
        echo "[init] IP mismatch on $NODE: stored=$CLUSTER_IP dns=$RESOLVED"
        STALE=1
    fi
done

# ---- 3. IP 漂移 → 全量重置 ----
if [ $STALE -eq 1 ]; then
    echo "[init] stale IPs detected, resetting all nodes..."
    for i in 1 2 3 4 5 6; do
        PORT=$((7000 + i - 1))
        redis-cli -h "redis-cluster-$i" -p $PORT -a "$PW" --no-auth-warning FLUSHALL 2>/dev/null || true
        redis-cli -h "redis-cluster-$i" -p $PORT -a "$PW" --no-auth-warning CLUSTER RESET HARD 2>/dev/null || true
    done
    sleep 1
fi

# ---- 4. 创建集群 ----
echo "[init] creating cluster..."
echo yes | redis-cli --cluster create $SEEDS --cluster-replicas 1 -a "$PW" --no-auth-warning
echo "=== Redis Cluster initialized ==="
