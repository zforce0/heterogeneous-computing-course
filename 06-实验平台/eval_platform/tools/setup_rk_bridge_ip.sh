#!/usr/bin/env bash
# Run ON RK3588 as root via systemd-run. Build a plain Linux bridge with ip(8).
# NM's two legacy profiles keep reclaiming eth0/eth1, so take the devices
# out of NM management first. Both physical ports become equivalent.
exec > /tmp/br_ip.log 2>&1
set -x

OLD1='有线连接 1'
OLD2='有线连接 2'

nmcli dev set eth0 managed no
nmcli dev set eth1 managed no

ip link add name br0 type bridge 2>/dev/null || ip link del br0 && ip link add name br0 type bridge
ip link set br0 type bridge stp_state 0 2>/dev/null
ip link set eth0 master br0
ip link set eth1 master br0
ip addr flush dev eth0
ip addr flush dev eth1
ip addr add 192.168.77.21/24 dev br0
ip link set eth0 up
ip link set eth1 up
ip link set br0 up
sleep 3

echo "=== after ==="
ip -br addr
bridge link 2>/dev/null || true

if ping -c 3 -W 2 192.168.77.1 >/dev/null 2>&1; then
  echo BRIDGE_IP_OK
else
  echo BRIDGE_IP_FAIL_ROLLBACK
  ip link set eth0 nomaster
  ip link set eth1 nomaster
  ip link del br0 2>/dev/null
  nmcli dev set eth0 managed yes
  nmcli dev set eth1 managed yes
  nmcli con up "$OLD1" 2>/dev/null
  nmcli con up "$OLD2" 2>/dev/null
fi
