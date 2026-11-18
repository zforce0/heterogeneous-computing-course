# 06 · 复现与校验

## 6.1 归档体系

每次实验以 `run_id` 建目录：`eval_platform/runs/<UTC时间>_<主题>/`。关键归档：

| run_id | 主题 | 事实来源 |
|---|---|---|
| `20261105T190201Z_G3_interconnect_heterogeneous` | E09 证据协议、E10 策略消融、E11 链路受限 | `report.md` |
| `20261113T145438Z_G4_m1_ascend_heterogeneous` | 昇腾 LLM 部署、NPU 尝试、节点分工、关键帧 | `report.md`、`npu_attempt.md`、`network_topology_snapshot.md` |
| `20261105T045400Z_E04_network_storage` | 早期网络/存储基线 | `report.html`、`summary.json` |

`report.md` 是数字的事实来源；`manifest.json` / `checksums.sha256` 用于核对归档完整性。大二进制 `bundle.bin`（16 MB）不进课程包。

## 6.2 重建页面（离线可复现）

```bash
make page
# 等价于 python3 eval_platform/dashboard/build_g4_dashboard.py
```

生成器只读三处输入：

- `eval_platform/llm/llm_bench_ascend.json`：LLM 基准用例；
- `eval_platform/runs/20261113T145438Z_G4_m1_ascend_heterogeneous/keyframes_ascend.json`：关键帧与 LLM verdict；
- `eval_platform/dashboard/samples/*`：小样本图；
- `g4_live.template.html`：模板。

输出 `g4_live.html`（自包含页面）与 `g4_state.json`（机器可读快照）。**不联网、不连板**，所以在任何机器上跑出的页面结构相同；差异只来自输入 JSON。

## 6.3 复现各实验

| 实验 | 复现命令 | 预期输出/注意 |
|---|---|---|
| 部署自检 | `make doctor` | 只读；离线远端显示 `[--]` 不算失败 |
| 拓扑发现 | `python3 eval_platform/tools/discover_topology.py --json` | 逐网口证据；`--write` 会覆盖 `topology_live.json` |
| LLM 吞吐 | `python3 eval_platform/tools/bench_llm_ascend.py` | 3 条用例；中位 4.04 tok/s 是原机结果，新机以实测为准 |
| 节点名单 | `curl -s 'http://127.0.0.1:8899/api/roster?refresh=1'` | 采不到写 unavailable，不伪造 |
| 事件事实 | `curl -s 'http://127.0.0.1:8899/api/events?seconds=60'` | 事件为内存态，重启清零；先让采样累积 >30 s |
| 双光研判 | `curl -sN -X POST .../api/analyze/stream -d '{"seconds":60}'` | 三行；判断行由后端强制回填 |
| 证据协议 | 见 G3 report §3 / `tools/evidence_*.py` | 需要两块板在线；核对 sent/ACK/duplicate |
| 课程包 | `make package && make verify` | 清单/SHA256/脱敏/语法/临时目录重建 |

## 6.4 时间、时钟与数据口径

- 三端墙钟曾实测漂移（RK ~7 个月、RV ~24 天、昇腾盒子落后约 15 个月）。跨设备协议用单调钟 + RTT，不使用墙钟建期限。
- 页面时间戳带 `+0800`；关键帧 `ts_ms` 对齐误差 1–2 ms（归档实测）。
- 实时流与归档关键帧是**不同时间尺度**的数据：前者 live，后者标 `archived`。热成像方向也不一样（实时按模组原始朝向，归档帧曾物理旋转 90°），不能直接叠加比对。
- 事件为近 60 s 滑窗；「当前稳态」不是事件。0.6B 的语义结论标注「仅供参考，不作为测量口径」。

## 6.5 打包与校验流程

```bash
make package    # 生成 dist/course-kit + tar.gz + zip，脱敏并排除大二进制
make verify     # 校验：
                #   1) MANIFEST.json 中每个文件的 SHA256
                #   2) 无残留明文设备口令
                #   3) Python / Shell 语法
                #   4) 在临时目录复制一份，跑 doctor.py 与页面重建
```

`verify` 的临时目录副本不会修改原工程；它模拟「别人拿到包后第一次打开」的场景。若 `doctor.py` 报远端离线，这是预期（档位 A/B），不是失败。

## 6.6 交付前人工检查

- [ ] `dist/course-kit/` 里没有 `config/lab.env`、没有私钥、没有 `~$*.docx` / `.DS_Store`
- [ ] `ARCHIVE_NOTICE.md` 说明了模型如何取回
- [ ] `docs/05-course-map.md` 的 7+4 名称与课程大纲一致
- [ ] 页面里两类问答角标不同，事实条数字与 `/api/events`、`/api/analyze/stream` 一致
