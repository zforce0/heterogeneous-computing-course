// dnn_bench.cpp — OpenCV DNN 在 RK3588 上跑真实 ONNX 模型，CPU vs GPU(OpenCL)
// 用法: dnn_bench <model.onnx> <target> <iters>
//   target: 0=CPU(DNN_TARGET_CPU)  1=GPU(DNN_TARGET_OPENCL)
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/core/ocl.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace cv;
using namespace cv::dnn;
using namespace std::chrono;

int main(int argc, char** argv) {
    std::string model = argc > 1 ? argv[1] : "/tmp/mobilenetv2.onnx";
    int target = argc > 2 ? atoi(argv[2]) : 0;
    int iters = argc > 3 ? atoi(argv[3]) : 20;

    if (target == 1) {
        if (!cv::ocl::haveOpenCL()) { printf("NO_OPENCL\n"); return 2; }
        cv::ocl::setUseOpenCL(true);
        printf("OpenCL device: %s\n", cv::ocl::Device::getDefault().name().c_str());
    } else {
        cv::ocl::setUseOpenCL(false);
    }

    Net net = readNetFromONNX(model);
    if (net.empty()) { printf("load model failed\n"); return 3; }
    if (target == 1) {
        net.setPreferableBackend(DNN_BACKEND_OPENCV);
        net.setPreferableTarget(DNN_TARGET_OPENCL);
    } else {
        net.setPreferableBackend(DNN_BACKEND_OPENCV);
        net.setPreferableTarget(DNN_TARGET_CPU);
    }

    // 构造 224x224x3 随机图 -> NCHW blob
    Mat img(224, 224, CV_8UC3);
    randu(img, Scalar(0, 0, 0), Scalar(255, 255, 255));
    Mat blob = blobFromImage(img, 1.0 / 255.0, Size(224, 224), Scalar(0, 0, 0), true, false);

    net.setInput(blob);
    Mat out = net.forward();   // warmup

    auto t0 = high_resolution_clock::now();
    for (int i = 0; i < iters; i++) { net.setInput(blob); out = net.forward(); }
    auto t1 = high_resolution_clock::now();
    double avg_ms = duration<double, std::milli>(t1 - t0).count() / iters;
    printf("RESULT target=%s model=%s avg_ms=%.2f fps=%.1f\n",
           target == 1 ? "GPU_OpenCL" : "CPU",
           model.c_str(), avg_ms, 1000.0 / avg_ms);
    printf("output shape: %d x %d\n", out.size[2], out.size[3]);
    return 0;
}
