# 课程产品包 V6.4 · 常用操作

.PHONY: help check labs evalkit platform package

help:
	@echo "目标："
	@echo "  make check      校验课程规划/适配指南/展示材料/商务材料的占位符是否齐全"
	@echo "  make labs       依次运行 4 个教具包 selftest（sim 模式，无需硬件）"
	@echo "  make evalkit    运行测评包一键测评"
	@echo "  make platform   实验平台部署自检（只读）"
	@echo "  make package    生成脱敏交付包（dist/）"

check:
	python3 scripts/check_placeholders.py

labs:
	@for m in 03-教具包/M*; do \
		echo "=== $$m ==="; \
		( cd "$$m" && python3 selftest/run_demo.py 2>&1 | tail -3 ) || echo "  [skip] $$m"; \
	done

evalkit:
	cd 05-测评包 && python3 run_eval.py

platform:
	cd 06-实验平台 && make doctor

package:
	python3 scripts/package_product.py
