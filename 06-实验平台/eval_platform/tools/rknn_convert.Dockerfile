FROM --platform=linux/amd64 ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    python3.8 python3.8-dev python3-pip python3-setuptools \
    libxslt1-dev zlib1g-dev libgl1-mesa-glx libglib2.0-0 libsm6 libxext6 libgomp1 \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install --no-cache-dir --upgrade pip setuptools wheel

RUN pip3 install --no-cache-dir \
    numpy==1.23.5 \
    onnx==1.14.1 \
    onnxruntime==1.15.1 \
    onnxoptimizer==0.3.8 \
    scipy==1.10.1 \
    opencv-python-headless==4.7.0.72 \
    tqdm ruamel.yaml psutil requests fast-histogram \
    torch==1.13.1+cpu -f https://download.pytorch.org/whl/cpu/torch_stable.html

COPY rknn_toolkit2-1.5.0+1fa95b5c-cp38-cp38-linux_x86_64.whl /opt/
RUN pip3 install --no-cache-dir --no-deps /opt/rknn_toolkit2-1.5.0+1fa95b5c-cp38-cp38-linux_x86_64.whl

WORKDIR /work
COPY resnet50.onnx convert.py /work/

CMD ["python3", "convert.py"]
