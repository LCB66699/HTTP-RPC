#!/bin/bash
# 等待 master 就绪后建立复制关系
until mysqladmin ping -h mysql-master -u root -p020421 --silent 2>/dev/null; do
  echo "Waiting for mysql-master..."
  sleep 2
done

mysql -u root -p020421 -h mysql-master -e "
  CREATE USER IF NOT EXISTS 'repl'@'%' IDENTIFIED BY 'repl_020421';
  GRANT REPLICATION SLAVE ON *.* TO 'repl'@'%';
  FLUSH PRIVILEGES;
" 2>/dev/null

mysql -u root -p020421 <<SQL
CHANGE MASTER TO
  MASTER_HOST='mysql-master',
  MASTER_PORT=3306,
  MASTER_USER='repl',
  MASTER_PASSWORD='repl_020421',
  MASTER_AUTO_POSITION=1;
START SLAVE;
SQL

echo "Slave replication started"
