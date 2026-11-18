# Agent 通过 SSH 访问 RV1126B 板端初始化配置

> 板端 IP: `192.168.3.252`  
> root 密码 <REDACTED>  
> 更新日期: 2026-09-29

---

## 一、板端 SSH 服务初始化配置

板端默认 SSH 配置可能禁止密码登录或 root 登录，导致 Agent 无法通过密码认证连接。需通过本地终端（键盘+显示器）或串口登录板端，执行以下配置。

### 1. 设置 / 确认 root 密码

```bash
passwd root
# 按提示输入新密码 <REDACTED>）
```

若忘记原密码，此命令可直接重设。

### 2. 配置 sshd 允许密码登录 + root 登录

编辑 `/etc/ssh/sshd_config`，确保以下两项为 `yes`：

```bash
# 允许密码认证（默认可能为 no 或被注释）
PasswordAuthentication yes

# 允许 root 直接登录
PermitRootLogin yes

# 可选：允许键盘交互认证（某些发行版需要）
ChallengeResponseAuthentication yes
KbdInteractiveAuthentication yes
```

可直接用 sed 批量修改：

```bash
sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config
sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config
sed -i 's/^#\?ChallengeResponseAuthentication.*/ChallengeResponseAuthentication yes/' /etc/ssh/sshd_config
sed -i 's/^#\?KbdInteractiveAuthentication.*/KbdInteractiveAuthentication yes/' /etc/ssh/sshd_config
```

### 3. 解锁 root 账户（若被锁定）

```bash
passwd -S root
# 输出若为 "root L ..." 表示账户被锁定，执行：
passwd -u root
```

### 4. 重启 sshd 使配置生效

```bash
systemctl restart sshd
# 或
service ssh restart
# 或直接杀掉 sshd 进程让 systemd 重启
killall sshd
```

### 5. 验证

在开发机上执行：

```bash
ssh -o StrictHostKeyChecking=no root@192.168.3.252 'echo ok'
# 输入密码 <REDACTED>，应输出 ok
```

---

## 二、Agent 侧连接工具

项目使用以下脚本通过 pexpect 自动输入密码连接板端：

### `/tmp/ssh_run.py` — 单命令执行

```python
#!/usr/bin/env python3
import pexpect, sys

cmd = sys.argv[1]
child = pexpect.spawn('/bin/bash', ['-c',
    f'ssh -o StrictHostKeyChecking=no root@192.168.3.252 {cmd!r}'],
    timeout=int(sys.argv[2]) if len(sys.argv) > 2 else 60, encoding='utf-8')
sent = 0
while True:
    i = child.expect(['[Pp]assword', pexpect.EOF, pexpect.TIMEOUT])
    if i == 0 and sent < 3:
        child.sendline('<REDACTED>'); sent += 1
    elif i == 1:
        break
    else:
        print('TIMEOUT'); break
print(child.before)
child.close()
sys.exit(child.exitstatus or 0)
```

用法：

```bash
python3 /tmp/ssh_run.py '要执行的板端命令' [超时秒数]
```

### `/tmp/deploy_run.py` — 同步源码并执行

将本地 `/mnt/hgfs/share/code/rv1126_test` 目录通过 tar+ssh 管道同步到板端 `/userdata/rv1126_test`，然后执行指定命令。

```bash
# 同步源码 + 执行构建
SYNC=1 python3 /tmp/deploy_run.py 'cd /userdata/rv1126_test && bash build.sh' 300

# 不同步，仅执行命令
SYNC=0 python3 /tmp/deploy_run.py 'ls /userdata/rv1126_test' 30
```

环境变量：
- `BOARD`: 板端地址（默认 `root@192.168.3.252`）
- `SSHPASS`: 板端密码 <REDACTED>）
- `LOCAL_DIR`: 本地源码目录（默认 `/mnt/hgfs/share/code/rv1126_test`）
- `REMOTE_DIR`: 板端目标目录（默认 `/userdata/rv1126_test`）
- `SYNC`: 是否同步源码（默认 `1`）

---

## 三、常见问题排查

### 3.1 密码正确但认证失败

**现象**：`Permission denied (publickey,password)`

**排查**：
1. 确认 sshd 已重启加载新配置：`systemctl status sshd`
2. 检查 `/etc/ssh/sshd_config` 中是否有 `Match` 块覆盖了全局配置
3. 检查 `/etc/ssh/sshd_config.d/` 目录下是否有额外配置文件
4. 查看 sshd 日志：`journalctl -u sshd -n 50`

### 3.2 known_hosts 权限问题

**现象**：`hostfile_replace_entries: mkstemp: Permission denied`

这是开发机 `~/.ssh/known_hosts` 文件权限问题，不影响实际连接（`-o StrictHostKeyChecking=no` 已跳过主机密钥校验），可忽略。

### 3.3 NFS exports 与 SSH 无关

`/etc/exports` 是 NFS 文件共享配置，与 SSH 登录认证**完全无关**。修改 `insecure` 选项不影响 SSH 访问。

---

## 四、板端当前 USB 拓扑（2026-09-29）

用户已将两个摄像头分到不同 USB 控制器，解决了 USB 2.0 带宽冲突问题：

```
Bus 001 (xhci-hcd, USB 2.0 480M)
  └── Dev 3: 0bda:5840 USB Camera (红外)

Bus 003 (xhci-hcd, USB 3.0 5000M)
  └── Hub (05e3:0626)
       └── Dev 3: 0bda:8153 RTL8153 千兆网卡

Bus 004 (ehci-platform, USB 2.0 480M)
  └── Hub (05e3:0610)
       └── Dev 4: 1d6c:0103 webcamproduct (可见光)
```

红外与可见光分别在 Bus 001 和 Bus 004 两个独立控制器上，不再共享 USB 2.0 等时带宽预算，双相机可同时全速运行。
