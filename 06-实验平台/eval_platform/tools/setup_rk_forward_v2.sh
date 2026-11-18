#!/usr/bin/env bash
# Run ON RK3588 as root via systemd-run. Topology AFTER swapping the cables:
#
#   Mac 192.168.77.1 --(eth1)-- RK 192.168.77.21 --(eth0)-- RV 192.168.77.22
#
# Kernel refuses bridging the two GMAC ports, so use L3 forwarding + proxy ARP.
# Mac and RV1126B configs stay untouched.
exec > /tmp/rk_forward_v2.log 2>&1
set -x

C_ETH0='有线连接 1'
C_ETH1='有线连接 2'
MAC_SIDE=eth1
RV_SIDE=eth0

# NM's DHCP profile on eth0 keeps resetting the link; take both ports out.
nmcli dev set $MAC_SIDE managed no
nmcli dev set $RV_SIDE managed no
nmcli con down "$C_ETH0" 2>/dev/null
nmcli con down "$C_ETH1" 2>/dev/null

ip addr flush dev $MAC_SIDE
ip addr flush dev $RV_SIDE
ip addr add 192.168.77.21/24 dev $MAC_SIDE
ip addr add 192.168.77.23/32 dev $RV_SIDE
ip link set $MAC_SIDE up
ip link set $RV_SIDE up
ip route replace 192.168.77.22/32 dev $RV_SIDE src 192.168.77.23

sysctl -w net.ipv4.ip_forward=1
sysctl -w net.ipv4.conf.$MAC_SIDE.proxy_arp=1
sysctl -w net.ipv4.conf.$RV_SIDE.proxy_arp=1
sysctl -w net.ipv4.conf.all.rp_filter=0
sysctl -w net.ipv4.conf.$MAC_SIDE.rp_filter=0
sysctl -w net.ipv4.conf.$RV_SIDE.rp_filter=0

sleep 2
echo "=== state ==="
ip -br addr
ip route

if ping -c 3 -W 2 192.168.77.1 >/dev/null 2>&1; then
  echo FORWARD_V2_OK
else
  echo FORWARD_V2_FAIL_ROLLBACK
  ip addr flush dev $MAC_SIDE
  ip addr flush dev $RV_SIDE
  ip addr add 192.168.77.21/24 dev $MAC_SIDE
  ip route del 192.168.77.22/32 dev $RV_SIDE 2>/dev/null
  sysctl -w net.ipv4.ip_forward=0
  nmcli dev set $MAC_SIDE managed yes
  nmcli dev set $RV_SIDE managed yes
  nmcli con up "$C_ETH0" 2>/dev/null
  nmcli con up "$C_ETH1" 2>/dev/null
fi
