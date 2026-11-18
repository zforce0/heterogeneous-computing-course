#!/usr/bin/env bash
# G4 一键收尾：待 RV1126B / 昇腾盒子上电并接好交换机后运行。
#   bash eval_platform/tools/finalize_lab_setup.sh
#
# 已完成（2026-11-14 01:28，无需重做）：
#   - Mac 侧生成 3 把无口令 ed25519 密钥 + ~/.ssh/config 别名
#   - RK3588 (cat@.21) 公钥已装、免密与 sudo -n 均验证通过
#   - RK3588 上执行 disable_rk_proxy_arp.sh：proxy_arp=0 / forward=0 / 服务 inactive
#   - Mac 侧 .22 的陈旧 ARP 条目已清除
#
# 本脚本只做「等设备上线后的收尾」，不猜测凭据、不伪造结果。
set -uo pipefail
LAB="192.168.77"
PUB_RV="$HOME/.ssh/id_ed25519_g4_rv1126b.pub"
PUB_ASC="$HOME/.ssh/id_ed25519_g4_ascend.pub"

alive() { ping -c 2 -t 2 "$1" 2>&1 | grep -q 'round-trip'; }

echo "=== G4 收尾 $(date '+%Y-%m-%d %H:%M:%S %z') ==="
echo "--- 0. 前置：RK3588 修复是否仍生效 ---"
if ssh -o BatchMode=yes -o ConnectTimeout=6 rk3588 '
     a=$(cat /proc/sys/net/ipv4/conf/eth1/proxy_arp)
     f=$(cat /proc/sys/net/ipv4/ip_forward)
     s=$(systemctl is-active rk-forward.service 2>/dev/null)
     echo "proxy_arp=$a forward=$f service=$s"' 2>/dev/null; then :; else
  echo "[FAIL] RK3588 免密不通，请先修 SSH；后续跳过"; exit 1
fi

echo "--- 1. RK 侧关闭任何复发的 proxy ARP ---"
VA=$(ssh -o BatchMode=yes rk3588 'cat /proc/sys/net/ipv4/conf/eth1/proxy_arp' 2>/dev/null)
VF=$(ssh -o BatchMode=yes rk3588 'cat /proc/sys/net/ipv4/ip_forward' 2>/dev/null)
if [ "$VA" != "0" ] || [ "$VF" != "0" ]; then
  echo "  检测到复发，重跑 disable_rk_proxy_arp.sh"
  ssh -o BatchMode=yes rk3588 'sudo bash -s' < "$(dirname "$0")/disable_rk_proxy_arp.sh" 2>&1 | tail -12
else
  echo "  [PASS] proxy_arp=0 / forward=0 保持生效"
fi

echo "--- 2. RV1126B ($LAB.22) ---"
if alive "$LAB.22"; then
  echo "  [PASS] RV1126B 已上线"
  if ssh -o BatchMode=yes -o ConnectTimeout=5 rv1126b 'hostname' >/dev/null 2>&1; then
    echo "  [PASS] RV1126B 免密已可用"
  else
    echo "  [TODO] 尚未免密。用下面命令一次性安装公钥（需 root 口令 <REDACTED>）："
    echo "         ssh-copy-id -i $PUB_RV root@$LAB.22"
    echo "         或在板端本地把公钥追加到 /root/.ssh/authorized_keys"
  fi
  echo "  视觉模型启动：bash eval_platform/runs/20261113T145438Z_G4_m1_ascend_heterogeneous/edge_vision/run_edge_vision.sh rv1126b"
else
  echo "  [WAIT] RV1126B 仍不在线：确认已上电、网线插交换机、静态地址 $LAB.22/24"
fi

echo "--- 3. 昇腾盒子 ($LAB.10) ---"
if alive "$LAB.10"; then
  echo "  [PASS] 昇腾盒子已上线"
  if ssh -o BatchMode=yes -o ConnectTimeout=5 ascend 'hostname' >/dev/null 2>&1; then
    echo "  [PASS] 昇腾免密已可用，开始采集设备规格"
    ssh -o BatchMode=yes ascend 'bash -s' < "$(dirname "$0")/probe_ascend_box.sh" | tee ascend_probe.txt
  else
    echo "  [TODO] 尚未免密。先安装公钥（用户名按盒子实际配置，常见 root / HwHiAiUser）："
    echo "         ssh-copy-id -i $PUB_ASC root@$LAB.10"
    echo "         装好后重跑本脚本即可自动采集；也可直接运行："
    echo "         ssh root@$LAB.10 'bash -s' < eval_platform/tools/probe_ascend_box.sh"
  fi
else
  echo "  [WAIT] 昇腾盒子不在线：确认已上电、网线插交换机、静态地址 $LAB.10/24"
  echo "         （若它是 300I Duo / 910B 级，采集结果会翻转「M1 是 LLM 最强节点」的判定）"
fi

echo "--- 4. 全量复检 ---"
bash "$(dirname "$0")/recheck_lab_link.sh" 2>&1 | tail -22
echo
echo "=== 说明 ==="
echo "本脚本不尝试猜测或爆破任何口令；未测项一律标 WAIT/TODO，不填默认值。"
