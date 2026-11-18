import sys
import onnx
from rknn.api import RKNN

# 把符号 batch 维度('N')固定为 1，toolkit 不认动态 batch
m = onnx.load('/work/resnet50.onnx')
fixed = False
for inp in m.graph.input:
    for d in inp.type.tensor_type.shape.dim:
        if d.dim_param:
            d.dim_param = ''
            d.dim_value = 1
            fixed = True
model_path = '/work/resnet50.onnx'
if fixed:
    onnx.save(m, '/work/resnet50_fixed.onnx')
    model_path = '/work/resnet50_fixed.onnx'
    print('batch dim fixed to 1 -> resnet50_fixed.onnx')

rknn = RKNN(verbose=True)
print('[1/4] config target_platform=rk3588')
rknn.config(target_platform='rk3588')

print('[2/4] load_onnx', model_path)
ret = rknn.load_onnx(model=model_path)
if ret != 0:
    print('load_onnx failed:', ret)
    sys.exit(1)

print('[3/4] build (FP16) ...')
ret = rknn.build(do_quantization=False)
if ret != 0:
    print('build failed:', ret)
    sys.exit(1)

print('[4/4] export rknn ...')
ret = rknn.export_rknn('/work/resnet50.rknn')
if ret != 0:
    print('export failed:', ret)
    sys.exit(1)

print('CONVERT_OK')
rknn.release()
