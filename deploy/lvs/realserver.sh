#!/bin/sh
# LVS DR real server setup — bind VIP to lo + suppress ARP
VIP=${VIP:-192.168.1.100}

ip addr add $VIP/32 dev lo 2>/dev/null
echo 1 > /proc/sys/net/ipv4/conf/lo/arp_ignore  2>/dev/null
echo 2 > /proc/sys/net/ipv4/conf/lo/arp_announce 2>/dev/null
echo 1 > /proc/sys/net/ipv4/conf/all/arp_ignore  2>/dev/null
echo 2 > /proc/sys/net/ipv4/conf/all/arp_announce 2>/dev/null
echo "[LVS] real server ready — VIP=$VIP bound to lo"
