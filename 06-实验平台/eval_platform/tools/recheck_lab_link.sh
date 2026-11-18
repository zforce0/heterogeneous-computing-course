#!/usr/bin/env bash
# 实验网链路/设备一键复检。交换机或设备上电后运行：
#   bash eval_platform/tools/recheck_lab_link.sh
# 只读，不修改网络配置；未通过时给出明确的下一步命令。
set -uo pipefail

LAB_IFACE="en5"
LAB_SVC="AX88179B"
LAB_IP="192.168.77.1"
NODES=("192.168.77.10:ascend" "192.168.77.21:rk3588" "192.168.77.22:rv1126b")

echo "=== 实验网复检 $(date '+%Y-%m-%d %H:%M:%S %z') ==="

echo "--- 1. 链路层 ---"
ifconfig "$LAB_IFACE" 2>/dev/null | grep -E 'inet |media:|status:' || { echo "FAIL: $LAB_IFACE 不存在"; exit 1; }
LINK=$(ifconfig "$LAB_IFACE" | grep 'status:' | awk '{print $2}')
[ "$LINK" = "active" ] && echo "[PASS] 链路 active" || echo "[FAIL] 链路 $LINK —— 检查网线/交换机电源"

echo "--- 2. 地址与路由 ---"
ADDR=$(ipconfig getifaddr "$LAB_IFACE" 2>/dev/null || echo "")
if [ "$ADDR" = "$LAB_IP" ]; then
  echo "[PASS] $LAB_IFACE = $LAB_IP/24"
else
  echo "[WARN] 当前地址 '${ADDR:-无}'，预期 $LAB_IP/24"
  echo "       配置: sudo networksetup -setmanual \"$LAB_SVC\" $LAB_IP 255.255.255.0"
fi
ROUTE=$(netstat -rn -f inet 2>/dev/null | grep -E '^192\.168\.77[[:space:]]+link#' | head -1)
[ -n "$ROUTE" ] && echo "[PASS] 直连路由: $ROUTE" || echo "[FAIL] 缺 192.168.77.0/24 直连路由"

echo "--- 3. 对端是否在发帧（3s 采样） ---"
C1=$(netstat -ib 2>/dev/null | awk -v i="$LAB_IFACE" '$1==i && $3=="<Link#23>" {print $5}' | head -1)
sleep 3
C2=$(netstat -ib 2>/dev/null | awk -v i="$LAB_IFACE" '$1==i && $3=="<Link#23>" {print $5}' | head -1)
if [ -n "$C1" ] && [ -n "$C2" ]; then
  echo "入向 Ipkts: $C1 -> $C2 (delta $((C2-C1)))"
  [ "$C2" -gt "$C1" ] && echo "[PASS] 对端有帧（但可能只是交换机 STP/LLDP）" || echo "[WARN] 无入向帧"
fi

echo "--- 4. 节点可达性 ---"
UP=0
for entry in "${NODES[@]}"; do
  ip="${entry%%:*}"; name="${entry##*:}"
  if ping -c 1 -t 1 "$ip" >/dev/null 2>&1; then
    echo "[PASS] $name ($ip) ping 通"; UP=$((UP+1))
  else
    echo "[FAIL] $name ($ip) 无应答"
  fi
done
echo "--- ARP（仅已解析条目） ---"
arp -a -n 2>/dev/null | grep 192.168.77 | grep -v incomplete || echo "(无已解析条目)"

echo "--- 5. SSH / 服务 ---"
if [ "$UP" -gt 0 ]; then
  for entry in "${NODES[@]}"; do
    ip="${entry%%:*}"; name="${entry##*:}"
    nc -z -G 1 "$ip" 22 2>/dev/null && echo "[PASS] $name:22 开放" || echo "[WARN] $name:22 未开放"
  done
else
  echo "[SKIP] 无可达节点"
fi

echo "--- 6. proxy ARP 残留检查（星型拓扑专有） ---"
# 2026-11-14 00:00：RV1126B 改接交换机后，RK3588 上链式时代的 rk-forward.service
# 仍代答 .22 的 ARP，导致 .21/.22/.23 指向同一 MAC —— 这是 RV 不通的根因。
RK_MAC=$(arp -a -n 2>/dev/null | awk '$1=="?",""{} /192\.168\.77\.21\)/ {print $4}')
RV_MAC=$(arp -a -n 2>/dev/null | awk '/192\.168\.77\.22\)/ && !/incomplete/ {print $4}')
if [ -n "$RK_MAC" ] && [ "$RK_MAC" = "$RV_MAC" ]; then
  echo "[FAIL] .22 与 .21 解析到同一 MAC ($RK_MAC) —— RK3588 的 proxy ARP 仍在劫持 RV1126B"
  echo "       修复：在 RK3588 上 root 执行 disable_rk_proxy_arp.sh（仅星型拓扑适用）"
  echo "       然后 Mac 侧：sudo arp -d 192.168.77.22 && ping -c 2 192.168.77.22"
else
  echo "[INFO] .21 MAC=${RK_MAC:-N/A} / .22 MAC=${RV_MAC:-N/A}（不同或 .22 未解析）"
fi

echo
echo "=== 结论（00:00 星型拓扑） ==="
if [ "$LINK" != "active" ]; then
  echo "链路未 up：检查网线两端、交换机电源与端口指示灯。"
elif [ -n "$RK_MAC" ] && [ "$RK_MAC" = "$RV_MAC" ]; then
  echo "RK3588 在线，但 proxy ARP 残留劫持 .22 → RV1126B 不可达（首要问题）。"
  echo "  1) 在 RK3588 上 root 执行：disable_rk_proxy_arp.sh"
  echo "  2) Mac 侧：sudo arp -d 192.168.77.22 && ping -c 2 192.168.77.22"
  echo "  3) 昇腾盒子 .10 仍无应答：确认上电、网线、静态地址 192.168.77.10/24"
  echo "  4) 补 SSH 免密后：ssh ascend 'bash -s' < probe_ascend_box.sh"
elif [ "$UP" -eq 0 ]; then
  echo "交换机/链路已通，但 3 台设备都没有三层应答。"
  echo "常见原因：设备未上电、网线未插到交换机、或设备不在 192.168.77.0/24。"
  echo "请确认昇腾盒子 / RK3588 / RV1126B 已开机并接好网线，再跑一次本脚本。"
else
  echo "$UP 个节点已上线。下一步："
  echo "  ssh ascend 'bash -s' < eval_platform/tools/probe_ascend_box.sh | tee ascend_probe.txt"
  echo "  bash eval_platform/runs/20261113T145438Z_G4_m1_ascend_heterogeneous/edge_vision/run_edge_vision.sh check"
fi
echo
echo "回退网络配置（如需恢复 DHCP）：sudo networksetup -setdhcp \"$LAB_SVC\""
