#!/usr/bin/env bash
# 在 RK3588 上执行：关闭链式拓扑残留的三层转发 + proxy ARP。
#
# 背景（2026-11-14 00:00）：RV1126B 已从「直连 RK3588 eth0」改为「直连交换机」，
# 拓扑从链式变为星型。但 RK3588 上的 rk-forward.service 仍在代答 .22 的 ARP
# （实测 .21/.22/.23 三个 IP 都解析到 RK eth1 的 46:0c:f8:68:d1:b9），
# 导致 Mac 发给 RV1126B 的帧被劫持到 RK，RV 收不到，ping 100% 丢包。
#
# 用法（在 RK3588 本机，需 root）：
#   sudo bash disable_rk_proxy_arp.sh
# 或从 Mac：
#   ssh cat@192.168.77.21 'sudo bash -s' < eval_platform/tools/disable_rk_proxy_arp.sh
#
# ⚠️ 只在「所有设备都挂交换机」的星型拓扑下执行。
#    若改回链式（RV 直连 RK eth0），不要执行，反而需要恢复 rk-forward.service。
set -u

ETH1="eth1"   # 接交换机/Mac 侧，192.168.77.21
ETH0="eth0"   # 原接 RV1126B 侧，192.168.77.23/32

echo "=== 关闭 RK3588 链式拓扑残留（星型拓扑） ==="
echo "执行前状态："
sysctl net.ipv4.ip_forward 2>/dev/null || true
for i in "$ETH0" "$ETH1"; do
  printf '  %s proxy_arp = %s\n' "$i" "$(cat /proc/sys/net/ipv4/conf/$i/proxy_arp 2>/dev/null || echo N/A)"
done

echo "--- 1. 停止并禁用三层转发服务 ---"
if systemctl list-unit-files 2>/dev/null | grep -q 'rk-forward.service'; then
  systemctl stop rk-forward.service 2>/dev/null && echo "  stopped rk-forward.service" || echo "  stop failed (check manually)"
  systemctl disable rk-forward.service 2>/dev/null && echo "  disabled rk-forward.service" || true
else
  echo "  rk-forward.service 不存在，跳过"
fi

echo "--- 2. 关闭两口 proxy_arp 与 ip_forward ---"
for i in "$ETH0" "$ETH1" all; do
  sysctl -w "net.ipv4.conf.$i.proxy_arp=0" >/dev/null 2>&1 || true
done
sysctl -w net.ipv4.ip_forward=0 >/dev/null 2>&1 || true

echo "--- 3. 撤销 .22 的主机路由（若存在） ---"
ip route del 192.168.77.22/32 dev "$ETH0" 2>/dev/null && echo "  removed .22 host route" || echo "  no .22 host route (ok)"

echo "--- 4. 刷新 ARP / 邻居缓存 ---"
ip neigh flush dev "$ETH1" 2>/dev/null || true
ip neigh flush dev "$ETH0" 2>/dev/null || true

echo "执行后状态："
sysctl net.ipv4.ip_forward 2>/dev/null || true
for i in "$ETH0" "$ETH1"; do
  printf '  %s proxy_arp = %s\n' "$i" "$(cat /proc/sys/net/ipv4/conf/$i/proxy_arp 2>/dev/null || echo N/A)"
done

echo
echo "=== 下一步（在 Mac 上验证） ==="
echo "  sudo arp -d 192.168.77.22        # 清掉被代答的缓存（需管理员）"
echo "  ping -c 2 192.168.77.22"
echo "  arp -a -n | grep 192.168.77.22   # 应解析到 RV1126B 自己的 MAC，不再是 46:0c:f8:68:d1:b9"
echo "  bash eval_platform/tools/recheck_lab_link.sh"
echo
echo "若 .22 仍不通：确认 RV1126B 已上电、网线插在交换机、且地址仍为 192.168.77.22/24。"
