#!/bin/bash
# 固化昇腾盒子实验网静态地址（保留出厂地址，不中断 SSH）。
#
# 背景：盒子 eth1 由 NetworkManager 的 "Wired connection 2" 管理，
# 出厂仅 192.168.137.100/24；实验网 192.168.77.10/24 是 `ip addr add` 临时加的，
# 重启即丢。本脚本把两个地址一并写入持久 profile。
#
# 用法（Mac 侧）：ssh ascend 'bash -s' < eval_platform/tools/persist_ascend_lab_ip.sh
# 回滚：nmcli connection modify "Wired connection 2" \
#           ipv4.addresses "192.168.137.100/24"
set -e
CON="Wired connection 2"
LAB_ADDR="192.168.77.10/24"
FACTORY_ADDR="192.168.137.100/24"
FACTORY_GW="192.168.137.1"
B=/opt/g4-llm/logs
mkdir -p "$B"

CUR=$(nmcli -g ipv4.addresses connection show "$CON" 2>/dev/null || true)
echo "=== before: $CUR"
nmcli connection show "$CON" > "$B/nm_wired2.before.txt" 2>&1 || true
cp /etc/NetworkManager/system-connections/*.nmconnection "$B/" 2>/dev/null || true
echo "配置已备份到 $B/"

if echo "$CUR" | grep -qF "192.168.77.10"; then
    echo "[SKIP] 实验网地址已在持久配置中，无需重复写入"
else
    nmcli connection modify "$CON" ipv4.method manual \
        ipv4.addresses "$FACTORY_ADDR,$LAB_ADDR" \
        ipv4.gateway "$FACTORY_GW"
    echo "[OK] 已写入双地址"
fi

echo "=== after(profile): $(nmcli -g ipv4.addresses connection show "$CON")"
echo "=== 落盘校验 ==="
grep -rE "address1=|address2=|gateway=" "/etc/NetworkManager/system-connections/$CON.nmconnection" 2>/dev/null || true

nmcli connection reload || true
echo "=== 运行时地址 ==="
ip -brief addr show eth1
echo
echo "注意：仅刷新了持久配置，未重启接口，当前 SSH 与 g4-llm 服务不受影响。"
echo "重启后 .77.10 将自动生效；如需立即由 NM 接管，可在确认有带外/跳板访问后执行："
echo "  nmcli connection up \"$CON\""
