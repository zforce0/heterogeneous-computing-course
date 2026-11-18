# 打包说明（请先看）

这是《多核互联异构计算技术及应用》课程材料包的**源码与文档快照**。
打包时间、文件清单与校验值见 `MANIFEST.json`。

## 有意不包含的内容

| 类型 | 原因 | 如何取回 |
|---|---|---|
| `*.rknn` / `*.onnx` / `*.pt` / `*.gguf` | 体积大（合计 >100 MB）、许可各异 | 按 `docs/01-environment.md` 从板端或原实验机拷贝；模型文件名与输入形状已写入文档 |
| 编译产物 `build/`、`*.o`、可执行文件 | 平台相关（aarch64 / x86_64） | 用包内 `CMakeLists.txt` / `build.sh` / `build_edge_detectors.sh` 重建 |
| `twocameraOnMAC/2026*.md` 过程日志 | 含历史设备口令 | 留在原实验机，不上传 |
| `output/`、`.workbuddy/`、嵌套 `.git/` | 文档流水线中间产物与本机私有状态 | 不需要 |
| `config/lab.env` | 可能含本机地址/别名 | 用 `config/lab.env.example` 复制生成 |

## 安全

历史文档中的设备默认口令已由 `scripts/sanitize.py` 替换为 `<REDACTED>`；
SSH 私钥与口令从未写入本包。实验网 IP 是课程内网事实，保留在归档报告里；
**运行期代码不依赖这些常量**，改用 `config/lab.env`（见 `docs/01-environment.md`）。

## 诚实口径

归档报告中的实测数字来自原实验机（2026-10 ~ 2026-11），不是本包在新机器上重跑的结果。
换机部署后请以 `make doctor` 与页面 SSE 的实时输出为准；未测项标 `N/A`。
