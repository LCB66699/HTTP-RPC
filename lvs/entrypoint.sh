#!/bin/sh
VIP=${VIP:-192.168.1.100}
ROLE=${ROLE:-MASTER}
PRIORITY=${PRIORITY:-100}

sed -i "s/{{VIP}}/$VIP/g" /etc/keepalived/keepalived.conf.tmpl
sed -i "s/{{ROLE}}/$ROLE/g" /etc/keepalived/keepalived.conf.tmpl
sed -i "s/{{PRIORITY}}/$PRIORITY/g" /etc/keepalived/keepalived.conf.tmpl
mv /etc/keepalived/keepalived.conf.tmpl /etc/keepalived/keepalived.conf

echo "[LVS] Role=$ROLE Priority=$PRIORITY VIP=$VIP"
exec keepalived -n -f /etc/keepalived/keepalived.conf
