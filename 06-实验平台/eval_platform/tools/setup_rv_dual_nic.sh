#!/usr/bin/env bash
# RV1126B 双网口适配：插 NET0(eth0) 或 NET1(eth1) 都能拿到 192.168.77.22
#
# 在 RV1126B 板端执行（需 root / sudo）：
#   scp eval_platform/tools/setup_rv_dual_nic.sh rv1126b:/tmp/ && ssh rv1126b "sudo bash /tmp/setup_rv_dual_nic.sh"
#
# 原理：
#   - eth0 = 板载 GMAC（NET0）：配静态 192.168.77.22/24（主力口，当前默认插这里）
#   - eth1 = USB 网卡（NET1）：配 DHCP，由 RK3588 上的 g4-dhcp.service 按 MAC 固定分配 .22
#   - 两个口任一时刻只插一根网线，因此 .22 不会冲突；哪个口插线哪个口对外服务
#   - 视觉服务 camera_stream.py 监听 0.0.0.0:8082，与具体网口无关
#
# 前置（RK3588 侧，一次性）：/etc/g4-dhcp.conf 需含下面这条（两 MAC 绑同一 IP）：
#   dhcp-host=4e:0e:39:be:41:b3,a6:52:d5:68:39:f1,192.168.77.22,infinite
# 并确保 systemctl is-active g4-dhcp == active。
set -euo pipefail

IP="192.168.77.22/24"
MAC0="4e:0e:39:be:41:b3"   # eth0 (NET0, 板载)
MAC1="a6:52:d5:68:39:f1"   # eth1 (NET1, USB)
TS="$(date +%Y%m%d-%H%M%S)"
NP="/etc/netplan"

echo "=== RV1126B 双网口适配 $(date) ==="
[ "$(id -u)" = "0" ] || { echo "请用 sudo/root 执行"; exit 1; }

# 1) 备份现有 netplan
for f in "$NP"/50-eth0-init.yaml "$NP"/51-eth1-init.yaml; do
  [ -f "$f" ] && cp "$f" "$f.bak.$TS" && echo "备份 $f"
done

# 2) eth0：静态 .22（若当前已是静态则保留，确保 dhcp4:false + optional:true）
cat > "$NP/50-eth0-init.yaml" <<EOF
network:
  version: 2
  renderer: networkd
  ethernets:
    eth0:
      optional: true
      dhcp4: false
      addresses: [$IP]
EOF

# 3) eth1：DHCP（由 RK3588 按 MAC 固定给 .22）
cat > "$NP/51-eth1-init.yaml" <<EOF
network:
  version: 2
  renderer: networkd
  ethernets:
    eth1:
      optional: true
      dhcp4: true
      dhcp4-overrides:
        route-metric: 100
EOF

echo "--- eth0 配置 ---"; cat "$NP/50-eth0-init.yaml"
echo "--- eth1 配置 ---"; cat "$NP/51-eth1-init.yaml"

# 4) 应用（optional:true 使未插线的口不阻塞启动）
netplan apply
sleep 2

echo "=== 结果 ==="
ip -brief addr show | grep -E 'eth0|eth1' || true
echo "eth0 carrier=$(cat /sys/class/net/eth0/carrier 2>/dev/null)  eth1 carrier=$(cat /sys/class/net/eth1/carrier 2>/dev/null)  (1=插线)"
echo "MAC eth0=$(cat /sys/class/net/eth0/address)  eth1=$(cat /sys/class/net/eth1/address)"
echo
echo "完成。插 NET0 → 静态 .22；插 NET1 → DHCP 拿 .22（需 RK3588 g4-dhcp 在线）。"
echo "验证：从 Mac 执行 ping -c2 192.168.77.22 与 curl http://192.168.77.22:8082/status.json"
