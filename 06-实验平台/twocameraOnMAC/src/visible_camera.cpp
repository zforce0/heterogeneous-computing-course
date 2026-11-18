#include "visible_camera.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/time.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(TWOCAMERA_NO_JPEG)
#include <cmath>
#endif

namespace {

int retry_ioctl(int fd, unsigned long request, void* argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

uint64_t monotonic_time_us()
{
    const std::chrono::steady_clock::duration now =
        std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

bool read_hex_file(const std::string& path, unsigned int& value)
{
    FILE* file = std::fopen(path.c_str(), "r");
    if (file == NULL) {
        return false;
    }
    const bool ok = std::fscanf(file, "%x", &value) == 1;
    std::fclose(file);
    return ok;
}

bool is_capture_device(const std::string& path,
                       uint32_t requested_format)
{
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }

    v4l2_capability capability = {};
    if (retry_ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
        ::close(fd);
        return false;
    }
    const uint32_t caps =
        (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? capability.device_caps
            : capability.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
        (caps & V4L2_CAP_STREAMING) == 0) {
        ::close(fd);
        return false;
    }

    bool supports_format = false;
    for (uint32_t index = 0; ; ++index) {
        v4l2_fmtdesc description = {};
        description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        description.index = index;
        if (retry_ioctl(fd, VIDIOC_ENUM_FMT, &description) < 0) {
            break;
        }
        if (description.pixelformat == requested_format ||
            (requested_format == V4L2_PIX_FMT_MJPEG &&
             description.pixelformat == V4L2_PIX_FMT_JPEG)) {
            supports_format = true;
            break;
        }
    }
    ::close(fd);
    return supports_format;
}

bool path_matches_usb_id(std::string path, unsigned int vendor_id,
                         unsigned int product_id)
{
    for (int depth = 0; depth < 12 && path.size() > 4; ++depth) {
        unsigned int vendor = 0;
        unsigned int product = 0;
        if (read_hex_file(path + "/idVendor", vendor) &&
            read_hex_file(path + "/idProduct", product)) {
            return vendor == vendor_id && product == product_id;
        }
        const size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) {
            break;
        }
        path.erase(slash);
    }
    return false;
}

bool has_jpeg_markers(const uint8_t* data, size_t size)
{
    if (data == NULL || size < 4 ||
        data[0] != 0xff || data[1] != 0xd8) {
        return false;
    }

    const size_t search_start = size > 64 ? size - 64 : 2;
    for (size_t i = size - 2; ; --i) {
        if (data[i] == 0xff && data[i + 1] == 0xd9) {
            return true;
        }
        if (i == search_start) {
            break;
        }
    }
    return false;
}

uint32_t sampled_checksum(const uint8_t* data, size_t size)
{
    uint32_t checksum = 2166136261u;
    const size_t step = std::max<size_t>(1, size / 256);
    for (size_t i = 0; i < size; i += step) {
        checksum ^= data[i];
        checksum *= 16777619u;
    }
    checksum ^= data[size - 1];
    return checksum * 16777619u;
}

#if defined(TWOCAMERA_NO_JPEG)
void convert_yuyv_to_bgr(const uint8_t* yuyv, int width,
                         int height, BgrFrame& output)
{
    output.resize(width, height);
    for (int y = 0; y < height; ++y) {
        const uint8_t* src =
            yuyv + static_cast<size_t>(y) * width * 2;
        uint8_t* dst = output.data.data() +
                       static_cast<size_t>(y) * output.stride;
        for (int x = 0; x < width; x += 2) {
            const int y0 = src[x * 2];
            const int u = src[x * 2 + 1] - 128;
            const int y1 = src[x * 2 + 2];
            const int v = src[x * 2 + 3] - 128;

            const int c0 = 298 * (y0 - 16);
            const int b0 = std::max(
                0, std::min(255, (c0 + 516 * u + 128) >> 8));
            const int g0 = std::max(
                0, std::min(255,
                            (c0 - 100 * u - 208 * v + 128) >> 8));
            const int r0 = std::max(
                0, std::min(255, (c0 + 409 * v + 128) >> 8));

            dst[x * 3] = static_cast<uint8_t>(b0);
            dst[x * 3 + 1] = static_cast<uint8_t>(g0);
            dst[x * 3 + 2] = static_cast<uint8_t>(r0);

            const int c1 = 298 * (y1 - 16);
            const int b1 = std::max(
                0, std::min(255, (c1 + 516 * u + 128) >> 8));
            const int g1 = std::max(
                0, std::min(255,
                            (c1 - 100 * u - 208 * v + 128) >> 8));
            const int r1 = std::max(
                0, std::min(255, (c1 + 409 * v + 128) >> 8));

            dst[(x + 1) * 3] = static_cast<uint8_t>(b1);
            dst[(x + 1) * 3 + 1] = static_cast<uint8_t>(g1);
            dst[(x + 1) * 3 + 2] = static_cast<uint8_t>(r1);
        }
    }
}
#endif

} // namespace

VisibleCamera::VisibleCamera()
    : fd_(-1),
      actual_width_(0),
      actual_height_(0),
      actual_fps_(0),
      stream_started_(false),
      running_(false),
      sequence_(0),
      capture_sequence_(0)
{
}

VisibleCamera::~VisibleCamera()
{
    close();
}

bool VisibleCamera::open(const VisibleCameraConfig& config)
{
    close();
    config_ = config;

    std::string path = config_.device_path;
    if (path.empty() && !find_usb_video_device(path)) {
        std::fprintf(stderr,
                     "No matching streaming V4L2 camera "
                     "was found\n");
        return false;
    }

    if (!open_device(path) || !configure_device() || !map_buffers()) {
        close();
        return false;
    }
    device_path_ = path;
    std::printf("V4L2 camera: %s, %s %dx%d@%d\n",
                device_path_.c_str(),
                config_.format == V4l2CaptureFormat::Mjpeg
                    ? "MJPG"
                    : "YUYV",
                actual_width_, actual_height_, actual_fps_);
    return true;
}

bool VisibleCamera::find_usb_video_device(std::string& path) const
{
    DIR* directory = opendir("/sys/class/video4linux");
    if (directory == NULL) {
        return false;
    }

    const bool filter_by_usb_id =
        config_.vendor_id != 0 && config_.product_id != 0;
    std::vector<std::string> preferred_candidates;
    std::vector<std::string> fallback_candidates;
    dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        if (std::strncmp(entry->d_name, "video", 5) != 0) {
            continue;
        }

        const std::string device_path =
            std::string("/dev/") + entry->d_name;
        fallback_candidates.push_back(device_path);

        const std::string sysfs_link =
            std::string("/sys/class/video4linux/") +
            entry->d_name + "/device";
        char resolved[PATH_MAX] = {};
        if (realpath(sysfs_link.c_str(), resolved) == NULL) {
            continue;
        }
        if (filter_by_usb_id &&
            path_matches_usb_id(resolved, config_.vendor_id,
                                config_.product_id)) {
            preferred_candidates.push_back(device_path);
        }
    }
    closedir(directory);

    const uint32_t requested_format =
        config_.format == V4l2CaptureFormat::Mjpeg
            ? V4L2_PIX_FMT_MJPEG
            : V4L2_PIX_FMT_YUYV;
    std::sort(preferred_candidates.begin(),
              preferred_candidates.end());
    std::sort(fallback_candidates.begin(),
              fallback_candidates.end());

    for (size_t i = 0; i < preferred_candidates.size(); ++i) {
        if (is_capture_device(preferred_candidates[i],
                              requested_format)) {
            path = preferred_candidates[i];
            return true;
        }
    }

    for (size_t i = 0; i < fallback_candidates.size(); ++i) {
        if (std::binary_search(preferred_candidates.begin(),
                               preferred_candidates.end(),
                               fallback_candidates[i])) {
            continue;
        }
        if (is_capture_device(fallback_candidates[i],
                              requested_format)) {
            if (filter_by_usb_id) {
                std::fprintf(
                    stderr,
                    "Visible camera %04x:%04x was not found; using %s\n",
                    config_.vendor_id, config_.product_id,
                    fallback_candidates[i].c_str());
            }
            path = fallback_candidates[i];
            return true;
        }
    }
    return false;
}

bool VisibleCamera::open_device(const std::string& path)
{
    fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        std::fprintf(stderr, "Cannot open %s: %s\n",
                     path.c_str(), std::strerror(errno));
        return false;
    }

    v4l2_capability capability = {};
    if (retry_ioctl(fd_, VIDIOC_QUERYCAP, &capability) < 0) {
        std::fprintf(stderr, "VIDIOC_QUERYCAP failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    const uint32_t caps =
        (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? capability.device_caps
            : capability.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
        (caps & V4L2_CAP_STREAMING) == 0) {
        std::fprintf(stderr, "%s is not a streaming capture device\n",
                     path.c_str());
        return false;
    }
    return true;
}

bool VisibleCamera::configure_device()
{
    const uint32_t requested_format =
        config_.format == V4l2CaptureFormat::Mjpeg
            ? V4L2_PIX_FMT_MJPEG
            : V4L2_PIX_FMT_YUYV;
    v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = config_.width;
    format.fmt.pix.height = config_.height;
    format.fmt.pix.pixelformat = requested_format;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    if (retry_ioctl(fd_, VIDIOC_S_FMT, &format) < 0) {
        std::fprintf(stderr, "VIDIOC_S_FMT failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    if (format.fmt.pix.pixelformat != requested_format &&
        !(requested_format == V4L2_PIX_FMT_MJPEG &&
          format.fmt.pix.pixelformat == V4L2_PIX_FMT_JPEG)) {
        std::fprintf(stderr,
                     "Camera did not accept requested format\n");
        return false;
    }

    actual_width_ = static_cast<int>(format.fmt.pix.width);
    actual_height_ = static_cast<int>(format.fmt.pix.height);

    v4l2_streamparm parameters = {};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameters.parm.capture.timeperframe.numerator = 1;
    parameters.parm.capture.timeperframe.denominator =
        static_cast<uint32_t>(config_.fps);
    if (retry_ioctl(fd_, VIDIOC_S_PARM, &parameters) == 0 &&
        parameters.parm.capture.timeperframe.numerator != 0) {
        actual_fps_ = static_cast<int>(
            parameters.parm.capture.timeperframe.denominator /
            parameters.parm.capture.timeperframe.numerator);
    } else {
        actual_fps_ = config_.fps;
    }
    return true;
}

bool VisibleCamera::map_buffers()
{
    v4l2_requestbuffers request = {};
    request.count = config_.buffer_count;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (retry_ioctl(fd_, VIDIOC_REQBUFS, &request) < 0 ||
        request.count < 2) {
        std::fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n",
                     std::strerror(errno));
        return false;
    }

    buffers_.resize(request.count);
    for (uint32_t i = 0; i < request.count; ++i) {
        v4l2_buffer buffer = {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (retry_ioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
            std::fprintf(stderr, "VIDIOC_QUERYBUF failed: %s\n",
                         std::strerror(errno));
            return false;
        }

        void* address = mmap(NULL, buffer.length,
                             PROT_READ | PROT_WRITE, MAP_SHARED,
                             fd_, buffer.m.offset);
        if (address == MAP_FAILED) {
            std::fprintf(stderr, "mmap video buffer failed: %s\n",
                         std::strerror(errno));
            return false;
        }
        buffers_[i].address = address;
        buffers_[i].length = buffer.length;
    }
    return true;
}

bool VisibleCamera::start()
{
    if (fd_ < 0 || stream_started_) {
        return false;
    }

    for (uint32_t i = 0; i < buffers_.size(); ++i) {
        v4l2_buffer buffer = {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (retry_ioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
            std::fprintf(stderr, "VIDIOC_QBUF failed: %s\n",
                         std::strerror(errno));
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (retry_ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        std::fprintf(stderr, "VIDIOC_STREAMON failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    stream_started_ = true;
    running_.store(true);
    capture_thread_ = std::thread(&VisibleCamera::capture_loop, this);
    return true;
}

void VisibleCamera::capture_loop()
{
    while (running_.load()) {
        pollfd descriptor = {};
        descriptor.fd = fd_;
        descriptor.events = POLLIN;
        const int poll_result = poll(&descriptor, 1, 250);
        if (!running_.load()) {
            break;
        }
        if (poll_result < 0) {
            if (errno != EINTR) {
                std::fprintf(stderr, "poll camera failed: %s\n",
                             std::strerror(errno));
            }
            continue;
        }
        if (poll_result == 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }

        v4l2_buffer buffer = {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (retry_ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno != EAGAIN) {
                std::fprintf(stderr, "VIDIOC_DQBUF failed: %s\n",
                             std::strerror(errno));
            }
            continue;
        }

        const uint64_t capture_timestamp_us = monotonic_time_us();
        if (buffer.index < buffers_.size() &&
            buffer.bytesused > 0 &&
            buffer.bytesused <= buffers_[buffer.index].length) {
            const uint8_t* compressed_data =
                static_cast<const uint8_t*>(
                    buffers_[buffer.index].address);
            VisibleCaptureStats stats;
            stats.timestamp_us = capture_timestamp_us;
            stats.sequence = ++capture_sequence_;
            stats.bytes_used = buffer.bytesused;
            stats.sample_checksum =
                sampled_checksum(compressed_data, buffer.bytesused);
            if (config_.format ==
                V4l2CaptureFormat::Mjpeg) {
                stats.jpeg_markers_valid =
                    has_jpeg_markers(
                        compressed_data, buffer.bytesused);
                stats.payload_valid =
                    stats.jpeg_markers_valid;
            } else {
                stats.payload_valid =
                    buffer.bytesused >=
                    static_cast<size_t>(actual_width_) *
                        actual_height_ * 2;
            }
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                latest_stats_ = stats;
            }
#if defined(TWOCAMERA_NO_JPEG)
            if (config_.format ==
                    V4l2CaptureFormat::Yuyv &&
                stats.payload_valid) {
                convert_yuyv_to_bgr(
                    compressed_data, actual_width_,
                    actual_height_, convert_frame_);
                if (convert_frame_.valid()) {
                    convert_frame_.timestamp_us =
                        capture_timestamp_us;
                    convert_frame_.sequence = ++sequence_;
                    std::lock_guard<std::mutex> lock(
                        latest_mutex_);
                    std::swap(convert_frame_,
                              latest_frame_);
                }
            }
#else
            if (config_.format ==
                    V4l2CaptureFormat::Mjpeg &&
                stats.jpeg_markers_valid &&
                decoder_.decode(
                    compressed_data, buffer.bytesused,
                    decode_frame_)) {
                decode_frame_.timestamp_us =
                    capture_timestamp_us;
                decode_frame_.sequence = ++sequence_;
                std::lock_guard<std::mutex> lock(latest_mutex_);
                std::swap(decode_frame_, latest_frame_);
            }
#endif
        }

        if (retry_ioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
            std::fprintf(stderr, "VIDIOC_QBUF failed: %s\n",
                         std::strerror(errno));
            break;
        }
    }
}

bool VisibleCamera::copy_latest(BgrFrame& destination)
{
    std::lock_guard<std::mutex> lock(latest_mutex_);
    if (!latest_frame_.valid()) {
        return false;
    }
    destination = latest_frame_;
    return true;
}

bool VisibleCamera::copy_latest_stats(
    VisibleCaptureStats& destination)
{
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (latest_stats_.sequence == 0) {
        return false;
    }
    destination = latest_stats_;
    return true;
}

void VisibleCamera::unmap_buffers()
{
    for (size_t i = 0; i < buffers_.size(); ++i) {
        if (buffers_[i].address != NULL &&
            buffers_[i].address != MAP_FAILED) {
            munmap(buffers_[i].address, buffers_[i].length);
        }
    }
    buffers_.clear();
}

void VisibleCamera::close()
{
    running_.store(false);
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    if (stream_started_ && fd_ >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        retry_ioctl(fd_, VIDIOC_STREAMOFF, &type);
        stream_started_ = false;
    }
    unmap_buffers();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    device_path_.clear();
    actual_width_ = 0;
    actual_height_ = 0;
    actual_fps_ = 0;
#if !defined(TWOCAMERA_NO_JPEG)
    decode_frame_ = BgrFrame();
#else
    convert_frame_ = BgrFrame();
#endif
    {
        std::lock_guard<std::mutex> lock(latest_mutex_);
        latest_frame_ = BgrFrame();
    }
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        latest_stats_ = VisibleCaptureStats();
    }
    sequence_ = 0;
    capture_sequence_ = 0;
}
