#!/usr/bin/env bash
# Run ON RK3588 as root via systemd-run.
# Kernel refuses "ip link set ethX master br0" on this BSP, so use L3 forwarding
# + proxy ARP instead of a bridge. Mac and RV1126B configs stay untouched.
#
#   Mac 192.168.77.1 --(eth0)-- RK 192.168.77.21 --(eth1)-- RV 192.168.77.22
#
exec > /tmp/rk_forward.log 2>&1
set -x

C1='有线连接 1'
C2='有线连接 2'

# NM keeps reclaiming the ports and re-adding .21 on eth1; take them out.
nmcli dev set eth0 managed no
nmcli dev set eth1 managed no
nmcli con down "$C1" 2>/dev/null
nmcli con down "$C2" 2>/dev/null

ip addr flush dev eth0
ip addr flush dev eth1
ip addr add 192.168.77.21/24 dev eth0
ip addr add 192.168.77.23/32 dev eth1
ip link set eth0 up
ip link set eth1 up
ip route replace 192.168.77.22/32 dev eth1 src 192.168.77.23

sysctl -w net.ipv4.ip_forward=1
sysctl -w net.ipv4.conf.eth0.proxy_arp=1
sysctl -w net.ipv4.conf.eth1.proxy_arp=1
sysctl -w net.ipv4.conf.all.rp_filter=0
sysctl -w net.ipv4.conf.eth0.rp_filter=0
sysctl -w net.ipv4.conf.eth1.rp_filter=0

sleep 2
echo "=== state ==="
ip -br addr
ip route

if ping -c 3 -W 2 192.168.77.1 >/dev/null 2>&1; then
  echo FORWARD_OK
else
  echo FORWARD_FAIL_ROLLBACK
  ip addr flush dev eth0
  ip addr flush dev eth1
  ip addr add 192.168.77.21/24 dev eth1
  ip route del 192.168.77.22/32 dev eth1 2>/dev/null
  sysctl -w net.ipv4.ip_forward=0
  nmcli dev set eth0 managed yes
  nmcli dev set eth1 managed yes
  nmcli con up "$C1" 2>/dev/null
  nmcli con up "$C2" 2>/dev/null
fi
