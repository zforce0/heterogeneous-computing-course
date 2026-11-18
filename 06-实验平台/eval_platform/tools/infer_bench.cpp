// infer_bench.cpp — RK3588 推理计算基准（卷积 + GEMM）
// 用法: infer_bench <cpu|gpu> <iters>
//   卷积负载: 3x3 filter2D on 224x224x64 (float32)  —— CNN 推理主导算子
//   GEMM 负载: 512x512 x 512x512 (float32)            —— 全连接/注意力主导算子
// 输出: 单次时延(ms) 与 吞吐(GFLOPS)
#include <opencv2/opencv.hpp>
#include <opencv2/core/ocl.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace cv;
using namespace std::chrono;

int main(int argc, char** argv) {
    bool gpu = (argc > 1 && std::string(argv[1]) == "gpu");
    int iters = (argc > 2) ? atoi(argv[2]) : 100;
    if (iters < 1) iters = 100;

    if (gpu) {
        if (!cv::ocl::haveOpenCL()) { printf("NO_OPENCL\n"); return 2; }
        cv::ocl::setUseOpenCL(true);
        printf("OpenCL device: %s\n", cv::ocl::Device::getDefault().name().c_str());
    } else {
        cv::ocl::setUseOpenCL(false);
    }

    // ---- 卷积负载 512x512x3, 3x3 ----
    Mat kernel = Mat::ones(3, 3, CV_32F) / 9.0f;
    Mat src(512, 512, CV_32FC(3));
    randu(src, Scalar(0, 0, 0), Scalar(1, 1, 1));

    // ---- GEMM 负载 512x512 ----
    Mat A(512, 512, CV_32F), B(512, 512, CV_32F);
    randu(A, Scalar(0), Scalar(1)); randu(B, Scalar(0), Scalar(1));

    if (gpu) {
        UMat usrc, udst, ukernel, uA, uB, uC;
        src.copyTo(usrc); kernel.copyTo(ukernel);
        A.copyTo(uA); B.copyTo(uB);

        filter2D(usrc, udst, -1, ukernel);          // warmup
        gemm(uA, uB, 1.0, noArray(), 0.0, uC);       // warmup

        auto t0 = high_resolution_clock::now();
        for (int i = 0; i < iters; i++) filter2D(usrc, udst, -1, ukernel);
        auto t1 = high_resolution_clock::now();

        auto t2 = high_resolution_clock::now();
        for (int i = 0; i < iters; i++) gemm(uA, uB, 1.0, noArray(), 0.0, uC);
        auto t3 = high_resolution_clock::now();

        double conv_ms = duration<double, std::milli>(t1 - t0).count() / iters;
        double gemm_ms = duration<double, std::milli>(t3 - t2).count() / iters;
        double conv_g = (2.0 * 9.0 * 512.0 * 512.0 * 3.0) / (conv_ms / 1000.0) / 1e9;
        double gemm_g = (2.0 * 512.0 * 512.0 * 512.0) / (gemm_ms / 1000.0) / 1e9;
        printf("RESULT mode=gpu conv_ms=%.3f conv_GFLOPS=%.1f gemm_ms=%.3f gemm_GFLOPS=%.1f\n",
               conv_ms, conv_g, gemm_ms, gemm_g);
    } else {
        Mat dst, C;
        filter2D(src, dst, -1, kernel);             // warmup
        gemm(A, B, 1.0, noArray(), 0.0, C);          // warmup

        auto t0 = high_resolution_clock::now();
        for (int i = 0; i < iters; i++) filter2D(src, dst, -1, kernel);
        auto t1 = high_resolution_clock::now();

        auto t2 = high_resolution_clock::now();
        for (int i = 0; i < iters; i++) gemm(A, B, 1.0, noArray(), 0.0, C);
        auto t3 = high_resolution_clock::now();

        double conv_ms = duration<double, std::milli>(t1 - t0).count() / iters;
        double gemm_ms = duration<double, std::milli>(t3 - t2).count() / iters;
        double conv_g = (2.0 * 9.0 * 512.0 * 512.0 * 3.0) / (conv_ms / 1000.0) / 1e9;
        double gemm_g = (2.0 * 512.0 * 512.0 * 512.0) / (gemm_ms / 1000.0) / 1e9;
        printf("RESULT mode=cpu conv_ms=%.3f conv_GFLOPS=%.1f gemm_ms=%.3f gemm_GFLOPS=%.1f\n",
               conv_ms, conv_g, gemm_ms, gemm_g);
    }
    return 0;
}
