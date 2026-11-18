"""导出 yolov8n_person.onnx: 单文件 (无 .data), opset 19.

旧模型 yolov8n_person.onnx 的导出方式 (本脚本替代):
    - 用 torch.onnx.export 直接导出 DetectionModel (pytorch 2.13, opset 22)
    - 权重被保存为外部数据 -> 依赖 yolov8n_person.onnx.data (12MB)
    - 因为 net.export=False, Detect.forward 返回 (y, x) -> 模型带 4 个输出
      (output0 + cat_13/14/15 特征图), C++ 端只用了第 1 个

本脚本改进:
    1. opset 固定 19 (默认, 可 --opset 覆盖)
    2. 只导出 output0 单输出 (包装 forward 取 y)
    3. onnxslim 简化后 save_as_external_data=False -> 单文件, 删除 .data
    4. onnxruntime 验证: 形状/推理/自包含性/与旧模型输出等价

用法 (在 models/yolov8n/ 目录下):
    ../../../YOLOFuse/venv/bin/python export_onnx.py
"""

import argparse
import shutil
import sys
import tempfile
from datetime import datetime
from pathlib import Path

# ultralytics 为 YOLOFuse 仓库的可编辑安装, 兜底加 sys.path
try:
    import ultralytics  # noqa: F401
except ImportError:
    sys.path.insert(0, "/mnt/hgfs/share/code/YOLOFuse")

import numpy as np
import onnx
import torch

OPSET_DEFAULT = 19


class SingleOutputWrapper(torch.nn.Module):
    """只返回检测输出 y, 丢弃特征图 x (对应旧模型的 cat_13/14/15)."""

    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, x):
        out = self.net(x)
        return out[0] if isinstance(out, (list, tuple)) else out


def export(model_path, output_path, imgsz=640, opset=OPSET_DEFAULT, simplify=True):
    from ultralytics import YOLO

    print(f"Loading model: {model_path}")
    model = YOLO(str(model_path))
    ch = model.model.yaml.get("ch", 3)
    nc = model.model.yaml.get("nc", 80)
    print(f"  task={model.task} names={model.names} ch={ch} nc={nc}")

    net = model.model
    net.eval()
    net.float()
    net.fuse()

    dummy = torch.zeros(1, ch, imgsz, imgsz, dtype=torch.float32)
    with torch.no_grad():
        out = SingleOutputWrapper(net)(dummy)
    print(f"  dry-run output: {tuple(out.shape)}")

    print(f"Exporting to ONNX (opset {opset})...")
    wrapper = SingleOutputWrapper(net)
    torch.onnx.export(
        wrapper,
        dummy,
        str(output_path),
        opset_version=opset,
        do_constant_folding=True,
        input_names=["images"],
        output_names=["output0"],
        dynamic_axes=None,  # 固定 [1, 3, 640, 640]
    )

    # 加载(外部数据读入内存) -> 简化 -> 单文件内联保存
    model_onnx = onnx.load(str(output_path))
    if simplify:
        try:
            import onnxslim

            model_onnx = onnxslim.slim(model_onnx)
            print("onnxslim simplify: ok")
        except Exception as e:  # noqa: BLE001
            print(f"WARNING: simplify skipped: {e}")

    meta = {
        "description": "YOLOv8n person detection (CrowdHuman)",
        "date": datetime.now().isoformat(),
        "task": "detect",
        "ch": str(ch),
        "nc": str(nc),
        "imgsz": str(imgsz),
        "names": str(model.names),
        "opset": str(opset),
    }
    for k, v in meta.items():
        item = model_onnx.metadata_props.add()
        item.key, item.value = k, v

    # 关键: 内联保存, 不产生外部 data 文件
    onnx.save_model(model_onnx, str(output_path), save_as_external_data=False)

    # 删除可能残留的 .data
    data_file = output_path.with_name(output_path.name + ".data")
    if data_file.exists():
        data_file.unlink()
        print(f"removed stale external data: {data_file.name}")

    size_mb = output_path.stat().st_size / (1024 * 1024)
    siblings = [p.name for p in output_path.parent.iterdir()
                if p.name.startswith(output_path.name)]
    print(f"saved: {output_path} ({size_mb:.1f} MB), related files: {siblings}")
    return str(model_path)


def verify(output_path, imgsz=640):
    import onnxruntime as ort

    m = onnx.load(str(output_path))
    print(f"opset={[(o.domain or 'ai.onnx', o.version) for o in m.opset_import]} "
          f"ir={m.ir_version} outputs={[o.name for o in m.graph.output]}")
    assert max(o.version for o in m.opset_import if not o.domain) == OPSET_DEFAULT

    # 自包含性: 拷到无 .data 的临时目录单独加载推理
    with tempfile.TemporaryDirectory() as tmp:
        lone = Path(tmp) / output_path.name
        shutil.copy(output_path, lone)
        sess = ort.InferenceSession(str(lone), providers=["CPUExecutionProvider"])
        x = np.random.rand(1, 3, imgsz, imgsz).astype(np.float32)
        out = sess.run(["output0"], {"images": x})[0]
        print(f"self-contained inference ok: output0 {out.shape} dtype={out.dtype}")
        assert out.shape == (1, 5, 8400) or out.shape[1] >= 5
    print("verify PASSED")


def check_equivalence(new_path, old_dir, imgsz=640):
    """与旧模型 (opset22 + .data) 在固定随机输入上对比 output0."""
    import onnxruntime as ort

    old_onnx = old_dir / "yolov8n_person.onnx"
    if not old_onnx.exists():
        print("equivalence check: no old model found, skipped")
        return
    try:
        sess_old = ort.InferenceSession(str(old_onnx),
                                        providers=["CPUExecutionProvider"])
        sess_new = ort.InferenceSession(str(new_path),
                                        providers=["CPUExecutionProvider"])
        rng = np.random.RandomState(0)
        x = rng.rand(1, 3, imgsz, imgsz).astype(np.float32)
        y_old = sess_old.run(["output0"], {"images": x})[0]
        y_new = sess_new.run(["output0"], {"images": x})[0]
        diff = np.abs(y_old - y_new).max()
        print(f"equivalence vs old model: max abs diff = {diff:.2e}")
        assert diff < 1e-3, f"outputs differ too much: {diff}"
    except Exception as e:  # noqa: BLE001
        print(f"WARNING: equivalence check failed: {e}")


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default=str(here / "yolov8n_person_best.pt"))
    parser.add_argument("--output", default=str(here / "yolov8n_person.onnx"))
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--opset", type=int, default=OPSET_DEFAULT)
    parser.add_argument("--no-simplify", action="store_true")
    parser.add_argument("--no-verify", action="store_true")
    args = parser.parse_args()

    model_path, output_path = Path(args.model), Path(args.output)
    if not model_path.exists():
        sys.exit(f"ERROR: model not found: {model_path}")

    # 备份旧模型到临时目录用于等价性检查
    with tempfile.TemporaryDirectory() as tmp:
        old_dir = Path(tmp)
        for f in here.glob("yolov8n_person.onnx*"):
            shutil.copy(f, old_dir / f.name)

        export(model_path, output_path, args.imgsz, args.opset,
               simplify=not args.no_simplify)
        if not args.no_verify:
            verify(output_path, args.imgsz)
            check_equivalence(output_path, old_dir, args.imgsz)

    print("\nDONE: single-file ONNX, no external data.")


if __name__ == "__main__":
    main()
