#!/usr/bin/env bash
# Run ON RK3588 as root (systemd-run / sudo). Bridges eth0 + eth1 into br0.
# br0 holds 192.168.77.21/24; both physical ports become equivalent, so
# Mac <-> any RK port and RK <-> RV1126B all share one L2 domain.
# Self-verifying: pings Mac 192.168.77.1; rolls back on failure.
exec > /tmp/br_setup.log 2>&1
set -x

echo "=== before ==="
nmcli -t -f NAME,DEVICE con show --active

# Remove stale bridge profiles, if any
nmcli con delete br-eth0 br-eth1 br-lan 2>/dev/null

nmcli con add type bridge ifname br0 con-name br-lan autoconnect yes \
  ipv4.method manual ipv4.addresses 192.168.77.21/24 ipv4.gateway "" \
  ipv6.method link-local
nmcli con add type bridge-slave ifname eth0 master br0 con-name br-eth0 autoconnect yes
nmcli con add type bridge-slave ifname eth1 master br0 con-name br-eth1 autoconnect yes

nmcli con up br-lan
nmcli con up br-eth0
nmcli con up br-eth1

sleep 6
echo "=== after ==="
ip -br addr
nmcli -t -f NAME,DEVICE con show --active

if ping -c 3 -W 2 192.168.77.1 >/dev/null 2>&1; then
  echo BRIDGE_OK
else
  echo BRIDGE_FAIL_ROLLBACK
  nmcli con down br-eth1 br-eth0 br-lan 2>/dev/null
  nmcli con delete br-eth0 br-eth1 br-lan 2>/dev/null
  nmcli con up "有线连接 1" 2>/dev/null
  nmcli con up "有线连接 2" 2>/dev/null
fi
