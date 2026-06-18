#!/bin/bash
# MySQL slave init — uses env vars, no hardcoded values
MASTER_HOST="${MYSQL_MASTER_HOST:-mysql-master}"
MASTER_PORT="${MYSQL_MASTER_PORT:-3306}"
ROOT_PASS="${MYSQL_ROOT_PASSWORD:-123456}"

echo "[slave-init] Waiting for master ${MASTER_HOST}:${MASTER_PORT}..."
until mysqladmin ping -h "$MASTER_HOST" -u root -p"$ROOT_PASS" --silent 2>/dev/null; do
    sleep 2
done
echo "[slave-init] Master reachable"

echo "[slave-init] Creating replication user on master..."
mysql -u root -p"$ROOT_PASS" -h "$MASTER_HOST" -e "
    CREATE USER IF NOT EXISTS 'repl'@'%' IDENTIFIED BY '$ROOT_PASS';
    GRANT REPLICATION SLAVE ON *.* TO 'repl'@'%';
    FLUSH PRIVILEGES;
" 2>/dev/null

echo "[slave-init] Configuring replication..."
mysql -u root -p"$ROOT_PASS" <<SQL
CHANGE MASTER TO
    MASTER_HOST='$MASTER_HOST',
    MASTER_PORT=$MASTER_PORT,
    MASTER_USER='repl',
    MASTER_PASSWORD='$ROOT_PASS',
    MASTER_AUTO_POSITION=1;
START SLAVE;
SQL

echo "[slave-init] Done"
