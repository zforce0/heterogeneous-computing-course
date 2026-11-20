# 互联异构计算课程工程 · 常用操作
# 在仓库根目录执行 `make help` 查看全部目标。所有目标均不修改网络配置。

.PHONY: help doctor page run start stop status check pipeline package verify clean

help:
	@echo "目标："
	@echo "  make start     一键启动（清理旧进程→后台启动→健康检查→打开浏览器）★推荐"
	@echo "  make stop      停止控制台   |   make status 查看运行状态"
	@echo "  make doctor    检查本机 Python/文件/端口与三个远端服务的可达性（只读）"
	@echo "  make page      用实测快照重建 g4_live.html 与 g4_state.json"
	@echo "  make run       启动实时控制台 127.0.0.1:8899（前台，Ctrl-C 结束）"
	@echo "  make check     运行 G4 链路前置检查（需要实验网设备已上电）"
	@echo "  make pipeline  另见 eval_platform/tools/run_g4_pipeline.sh check|llm|rk|rv|all"
	@echo "  make package   生成可交付的脱敏课程包（dist/course-kit + 压缩包）"
	@echo "  make verify    在临时目录模拟解包并做静态自检"
	@echo "  make clean     删除缓存与上次生成的课程包（不动源码与实测数据）"

doctor:
	python3 scripts/doctor.py

page:
	python3 eval_platform/dashboard/build_g4_dashboard.py

start:
	bash start.sh

stop:
	bash start.sh stop

status:
	bash start.sh status

run:
	bash eval_platform/tools/start_dashboard.sh

check:
	bash eval_platform/tools/run_g4_pipeline.sh check

pipeline:
	@bash eval_platform/tools/run_g4_pipeline.sh all

package:
	python3 scripts/package_course_kit.py

verify:
	python3 scripts/verify_package.py

clean:
	python3 scripts/clean_cache.py
