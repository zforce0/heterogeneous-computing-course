# 课程产品包 V6.4 · 常用操作

.PHONY: help check labs evalkit platform package protect unlock

help:
	@echo "目标："
	@echo "  make check      校验课程规划/适配指南/展示材料/商务材料的占位符是否齐全"
	@echo "  make labs       依次运行 4 个教具包 selftest（sim 模式，无需硬件）"
	@echo "  make evalkit    运行测评包一键测评"
	@echo "  make platform   实验平台部署自检（只读）"
	@echo "  make package    生成脱敏交付包（dist/）"
	@echo "  make unlock     解锁核心材料（需环境变量 KIT_PASSWORD）"
	@echo "  make protect    重新加密核心材料（需环境变量 KIT_PASSWORD）"

check:
	python3 scripts/check_placeholders.py

labs:
	@for m in 03-教具包/M*; do \
		echo "=== $$m ==="; \
		( cd "$$m" && python3 selftest/run_demo.py 2>&1 | tail -3 ) || echo "  [skip] $$m"; \
	done

evalkit:
	@if [ -f 05-测评包/run_eval.py ]; then cd 05-测评包 && python3 run_eval.py; \
	else echo "[需要解锁] 先运行: KIT_PASSWORD='<密码>' make unlock"; exit 1; fi

platform:
	cd 06-实验平台 && make doctor

package:
	python3 scripts/package_product.py

protect:
	KIT_PASSWORD="$(KIT_PASSWORD)" python3 scripts/protect_core.py encrypt

unlock:
	KIT_PASSWORD="$(KIT_PASSWORD)" python3 scripts/protect_core.py decrypt
