# Linux 后端问题排查手册

适用场景：HTTP-RPC 分布式系统（27+ 容器，C++ gRPC + MySQL + Redis + nginx + Docker）。

---

## 一、快速定位

### 1.1 系统整体状态

```bash
# 负载 + 内存
uptime                          # 1/5/15min 平均负载，> CPU 核数 = 过载
free -h                         # 内存 + swap，swap 使用 > 500M = OOM 前兆
df -h                           # 磁盘使用率 > 90% = 危险

# 最占 CPU/内存的进程
top -bn1 | head -20
ps aux --sort=-%cpu | head -10
ps aux --sort=-%rss | head -10
```

### 1.2 Docker 快速诊断

```bash
# 所有容器状态（看哪个 Exit / unhealthy）
docker ps -a --format "table {{.Names}}\t{{.Status}}" | grep -v Up

# 挂了容器的日志
docker logs --tail 50 <容器名>

# 资源消耗排名
docker stats --no-stream --format "table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.MemPerc}}"
```

### 1.3 磁盘 I/O

```bash
# 实时磁盘读写（哪个进程在刷盘）
iotop -o              # 只看有 I/O 的进程
iotop -b -n1 | head -15

# 磁盘使用
du -sh data/*/ | sort -h | tail -10   # 本项目数据目录
```

---

## 二、网络排查

### 2.1 TCP 状态概览

```bash
# 按状态统计连接数
ss -tan | awk '{print $1}' | sort | uniq -c | sort -rn

# TIME_WAIT 过多（>5000）= 频繁短连接，考虑 keepalive
ss -tan state time-wait | wc -l

# CLOSE_WAIT 持续存在 = 应用层没 close()，可能是 fd 泄漏
ss -tan state close-wait

# 某端口的连接数
ss -tan sport = :3306 | wc -l    # MySQL
ss -tan sport = :6379 | wc -l    # Redis
ss -tan sport = :8081 | wc -l    # Gateway
```

### 2.2 Send-Q / Recv-Q 解释

```bash
ss -tnp | awk '$2>0 || $3>0 {print}'
# 第二列 = Recv-Q（内核收到但应用没读）
# 第三列 = Send-Q（应用发了但对方没收完）
```

| 场景 | Recv-Q > 0 | Send-Q > 0 |
|------|-----------|-----------|
| MySQL Master | 客户端请求堆积 | Slave/Consumer binlog 拉取速度跟不上 |
| Gateway | 请求进来积压 | 后端 gRPC 响应发不出去 |
| nginx | 客户端上传慢 | 后端 Gateway 消费慢 |

持续 > 0 就是瓶颈信号。

### 2.3 端口连接详情

```bash
# MySQL 连接来源分布
ss -tnp sport = :3306 | awk '{print $5}' | cut -d: -f1 | sort | uniq -c | sort -rn

# Gateway 连接数（每 IP 多少连接）
ss -tnp sport = :8081 | awk '{print $5}' | cut -d: -f1 | sort | uniq -c | sort -rn
```

---

## 三、进程排查

### 3.1 fd 数量

```bash
# 某进程的 fd 数量（>10000 = 可能泄漏）
ls /proc/$(pidof rpc_server)/fd 2>/dev/null | wc -l

# Docker 容器的 fd
docker exec <容器名> ls /proc/1/fd | wc -l

# 系统级 fd 上限
cat /proc/sys/fs/file-max
cat /proc/sys/fs/file-nr    # 已分配 / 已使用 / 上限
```

### 3.2 进程资源

```bash
# 进程的 RSS/VSZ
ps -eo pid,rss,vsz,comm | grep -E "rpc_|gateway|mysql|redis" | sort -k2 -rn

# 进程网络连接数
lsof -p $(pidof rpc_server) -i 2>/dev/null | wc -l

# 线程数
cat /proc/$(pidof rpc_server)/status | grep Threads
```

### 3.3 Core Dump 分析

```bash
ulimit -c                          # core 文件大小限制
cat /proc/sys/kernel/core_pattern  # core 存放路径

# 如果 C++ 进程异常退出(exit 139 = SIGSEGV)，用 gdb 看调用栈：
gdb /app/rpc_server core.xxxxx
(gdb) bt                           # 调用栈
(gdb) info threads                 # 所有线程
```

---

## 四、MySQL 排查

### 4.1 进程列表

```bash
docker exec <mysql容器> mysql -uroot -p123456 -e "SHOW PROCESSLIST;" | grep -v Sleep
```

重点关注：
- `Sending data` 时间长 = 慢查询
- `Waiting for table metadata lock` = DDL 操作阻塞
- `Locked` = 行锁/表锁等待

### 4.2 主从复制

```bash
# Master 端 — 看哪些 Slave 连着
docker exec <mysql-master> mysql -uroot -p123456 -e "SHOW SLAVE HOSTS;"

# Slave 端 — 看落后多少
docker exec <mysql-slave> mysql -uroot -p123456 -e "SHOW SLAVE STATUS\G" | grep -E "Seconds_Behind_Master|Slave_IO_Running|Slave_SQL_Running|Last_IO_Error"

# Seconds_Behind_Master > 10 = Slave 跟不上
# Slave_IO_Running = No → 网络断开 / binlog 文件名错
```

### 4.3 锁与事务

```bash
# InnoDB 事务状态
docker exec <mysql容器> mysql -uroot -p123456 -e "SELECT * FROM information_schema.innodb_trx\G"

# 锁等待
docker exec <mysql容器> mysql -uroot -p123456 -e "SELECT * FROM information_schema.innodb_lock_waits\G" 2>/dev/null

# 元数据锁
docker exec <mysql容器> mysql -uroot -p123456 -e "SELECT * FROM performance_schema.metadata_locks WHERE OBJECT_SCHEMA IN ('rpc_spreadsheet','rpc_file')\G"
```

### 4.4 慢查询与性能

```bash
# 慢查询数量
docker exec <mysql容器> mysql -uroot -p123456 -e "SHOW GLOBAL STATUS LIKE 'Slow_queries';"

# Buffer Pool 命中率（应该 > 99%）
docker exec <mysql容器> mysql -uroot -p123456 -e "SHOW GLOBAL STATUS LIKE 'Innodb_buffer_pool_read%';"
# Innodb_buffer_pool_read_requests 很大但 Innodb_buffer_pool_reads 占比 < 1% = 正常

# 连接数
docker exec <mysql容器> mysql -uroot -p123456 -e "SHOW GLOBAL STATUS LIKE 'Threads_connected';"
```

### 4.5 磁盘与 binlog

```bash
# binlog 大小
docker exec <mysql容器> ls -lh /var/lib/mysql/mysql-bin.* | tail -5

# undo_log 积压（本项目，定期清理要跑的）
docker exec <mysql容器> mysql -uroot -p123456 -e "SELECT COUNT(*), MIN(created_at) FROM rpc_tx_log.undo_log;" 2>/dev/null
```

---

## 五、Redis 排查

### 5.1 连接与内存

```bash
docker exec <redis容器> redis-cli -p 7000 -a <密码> INFO stats | grep -E "total_connections|instantaneous"
docker exec <redis容器> redis-cli -p 7000 -a <密码> INFO memory | grep -E "used_memory_human|maxmemory"

# 内存使用 > maxmemory 且 evicted_keys > 0 = 驱逐中，设 maxmemory-policy
docker exec <redis容器> redis-cli -p 7000 -a <密码> INFO stats | grep evicted
```

### 5.2 集群状态

```bash
docker exec <redis容器> redis-cli -p 7000 -a <密码> CLUSTER INFO
docker exec <redis容器> redis-cli -p 7000 -a <密码> CLUSTER NODES

# cluster_state != ok → 有节点挂了
# 某节点的 flags 含 fail → 该节点被集群标记为故障
```

### 5.3 Key 分析与慢查询

```bash
# 大 Key（>10KB 过多说明 protobuf 序列化膨胀）
docker exec <redis容器> redis-cli -p 7000 -a <密码> --bigkeys

# 慢查询
docker exec <redis容器> redis-cli -p 7000 -a <密码> SLOWLOG GET 10

# Key 数量趋势（> 10 万说明有孤儿 Key）
docker exec <redis容器> redis-cli -p 7000 -a <密码> DBSIZE

# 各类型 Key 占用
docker exec <redis容器> redis-cli -p 7000 -a <密码> INFO keyspace
```

### 5.4 持久化与 I/O

```bash
# RDB 最近一次 bgsave 是否成功
docker exec <redis容器> redis-cli -p 7000 -a <密码> INFO persistence | grep -E "rdb_last_bgsave_status|aof_enabled"

# rdb_last_bgsave_status = err → RDB 生成失败（磁盘满或权限问题）
# aof_enabled = 1 → AOF 开着，每秒 fsync（缓存场景应该关掉）
```

---

## 六、系统级

### 6.1 OOM 排查

```bash
# 内核 OOM 记录
dmesg | grep -i "oom\|out of memory\|killed process" | tail -20

# Docker OOM
docker inspect <容器名> --format '{{.State.OOMKilled}}'
# true = 被 Docker --memory 限制杀掉的

# journalctl
journalctl -k | grep -i oom | tail -20
```

### 6.2 Swap 分析

```bash
# 哪些进程在用 swap
for pid in $(find /proc -maxdepth 1 -type d -name '[0-9]*' 2>/dev/null | sed 's|/proc/||'); do
    swap=$(awk '/VmSwap/{print $2}' /proc/$pid/status 2>/dev/null)
    [ -n "$swap" ] && [ "$swap" != "0" ] && echo "PID=$pid Swap=${swap}kB $(cat /proc/$pid/comm)"
done
```

---

## 七、本项目常见问题速查

| 现象 | 诊断命令 | 可能原因 |
|------|---------|---------|
| 所有容器 Exit 255 | `docker ps -a` | Docker daemon 重启，`docker compose up -d` |
| Gateway 连接超时 | `ss -tnp sport = :8081` 看 Recv-Q | nginx → gateway 连接堆积，检查 proxy_read_timeout |
| Sheet List 响应慢 | `docker exec <mysql> mysql ... -e "SHOW PROCESSLIST"` | 慢查询或锁等待，看 `idx_sheets_user_time` 是否用了 |
| Redis 内存增长 | `redis-cli DBSIZE` 连续采样 | 孤儿 Key（version/errors 无 TTL — 已修复加了 7d） |
| Master binlog 暴涨 | `ls -lh data/mysql-sp-0/mysql-bin.*` | 写入量大 + `expire_logs_days=7` 未清理旧 binlog |
| Slave 复制滞后 | `SHOW SLAVE STATUS` Seconds_Behind > 0 | Slave 磁盘 I/O 慢（relay-log 写盘），或单线程复制 |
| Send-Q MySQL:3306 | `ss -tnp sport = :3306` | Slave binlog 消费慢或 Canal 消费慢 |
| C++ 进程 SIGSEGV | `docker logs`, `dmesg` | protobuf 版本不匹配或 use-after-free |

---

## 八、持续监控建议

```bash
# 每 30s 采样一次，写入日志
while true; do
  echo "=== $(date) ===" >> /tmp/monitor.log
  echo "--- load ---" >> /tmp/monitor.log
  uptime >> /tmp/monitor.log
  echo "--- memory ---" >> /tmp/monitor.log
  free -h | head -2 >> /tmp/monitor.log
  echo "--- tcp ---" >> /tmp/monitor.log
  ss -tan | awk '/ESTAB/{s[$4]++} END{for(i in s) print i, s[i]}' | sort >> /tmp/monitor.log
  echo "--- docker ---" >> /tmp/monitor.log
  docker ps --format "{{.Names}} {{.Status}}" | grep -v "Up" >> /tmp/monitor.log
  sleep 30
done
```
