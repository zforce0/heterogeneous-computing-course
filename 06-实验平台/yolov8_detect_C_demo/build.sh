#!/bin/bash

rm build -rf
mkdir build
cd build
cmake ..
make
cd ..
cp build/yolov8_detect_demo yolov8_detect_demo_release
