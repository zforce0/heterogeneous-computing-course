#include "display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* 全志 DISP 图层配置结构体 - 用于设置 zorder 和 alpha */
/* 参考: T113-S3 显示驱动头文件 */
struct disp_layer_config {
    unsigned int layer_id;
    unsigned int enable;
    unsigned int priority;    /* zorder: 数值越大优先级越高 */
    unsigned int alpha_mode;  /* 0=不使用alpha, 1=使用alpha */
    unsigned int alpha_value; /* 0x00 ~ 0xFF */
};

/* /dev/disp ioctl 命令 */
#define DISP_LAYER_SET_CONFIG _IOW('D', 0x20, struct disp_layer_config)

namespace {

class FramebufferDisplay : public IDisplay {
public:
    FramebufferDisplay()
        : fd_(-1), disp_fd_(-1), fb_mmap_(NULL), fb_size_(0),
          width_(0), height_(0), stride_(0),
          bytes_per_pixel_(0),
          red_offset_(0), green_offset_(0),
          blue_offset_(0), alpha_offset_(0),
          frame_count_(0),
          opened_(false) {}

    ~FramebufferDisplay() override
    {
        close();
    }

    bool open(const DisplayConfig& config) override
    {
        close();

        fd_ = ::open(config.device.c_str(), O_RDWR);
        if (fd_ < 0) {
            std::fprintf(stderr,
                         "Cannot open framebuffer %s: %s\n",
                         config.device.c_str(),
                         std::strerror(errno));
            return false;
        }

        fb_fix_screeninfo finfo = {};
        if (ioctl(fd_, FBIOGET_FSCREENINFO, &finfo) < 0) {
            std::fprintf(stderr,
                         "FBIOGET_FSCREENINFO failed: %s\n",
                         std::strerror(errno));
            close();
            return false;
        }

        fb_var_screeninfo vinfo = {};
        if (ioctl(fd_, FBIOGET_VSCREENINFO, &vinfo) < 0) {
            std::fprintf(stderr,
                         "FBIOGET_VSCREENINFO failed: %s\n",
                         std::strerror(errno));
            close();
            return false;
        }

        width_ = static_cast<int>(vinfo.xres);
        height_ = static_cast<int>(vinfo.yres);
        stride_ = static_cast<int>(finfo.line_length);
        bytes_per_pixel_ = vinfo.bits_per_pixel / 8;
        fb_size_ = static_cast<size_t>(stride_) *
                   static_cast<size_t>(height_);
        red_offset_ = vinfo.red.offset;
        green_offset_ = vinfo.green.offset;
        blue_offset_ = vinfo.blue.offset;
        alpha_offset_ = vinfo.transp.offset;

        std::printf("[DISPLAY] FB info: %s, %dx%d, %dbpp, "
                    "stride=%d, smem_len=%u\n",
                    config.device.c_str(), width_, height_,
                    vinfo.bits_per_pixel, stride_,
                    finfo.smem_len);
        std::printf("[DISPLAY] Channel offsets: R=%u G=%u B=%u A=%u\n",
                    red_offset_, green_offset_,
                    blue_offset_, alpha_offset_);

        /* 使用 mmap 映射 framebuffer 内存 */
        fb_mmap_ = static_cast<uint8_t*>(
            mmap(NULL, fb_size_, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd_, 0));
        if (fb_mmap_ == MAP_FAILED) {
            std::fprintf(stderr,
                         "[DISPLAY] mmap fb failed: %s, "
                         "falling back to write()\n",
                         std::strerror(errno));
            fb_mmap_ = NULL;
            /* write() 回退模式需要分配缓冲区 */
            fb_buf_ = static_cast<uint8_t*>(
                std::calloc(1, fb_size_));
            if (!fb_buf_) {
                std::fprintf(stderr,
                             "[DISPLAY] Cannot allocate fb buffer\n");
                close();
                return false;
            }
        } else {
            std::printf("[DISPLAY] mmap fb succeeded, size=%zu bytes\n",
                        fb_size_);
        }

        /* 尝试设置 fb0 图层的 zorder 为最高优先级 */
        configure_fb_layer();

        opened_ = true;
        std::printf("[DISPLAY] Framebuffer opened successfully\n");
        return true;
    }

    void configure_fb_layer()
    {
        /* 尝试打开 /dev/disp 设置图层优先级 */
        disp_fd_ = ::open("/dev/disp", O_RDWR);
        if (disp_fd_ < 0) {
            std::printf("[DISPLAY] /dev/disp not available, "
                        "skipping zorder config\n");
            std::printf("[DISPLAY] If video layer covers fb0, "
                        "you may need to use disp ioctl\n");
            return;
        }

        /* 尝试设置 fb0 图层 (layer_id=0) 为最高优先级 */
        struct disp_layer_config cfg = {};
        cfg.layer_id = 0;       /* fb0 通常对应 layer 0 */
        cfg.enable = 1;         /* 启用图层 */
        cfg.priority = 0xF;     /* 最高 zorder 优先级 */
        cfg.alpha_mode = 1;     /* 使用 alpha */
        cfg.alpha_value = 0xFF; /* 完全不透明 */

        int ret = ioctl(disp_fd_, DISP_LAYER_SET_CONFIG, &cfg);
        if (ret < 0) {
            std::fprintf(stderr,
                         "[DISPLAY] DISP_LAYER_SET_CONFIG failed "
                         "(layer=0): %s\n",
                         std::strerror(errno));
            /* 尝试其他可能的图层 ID */
            cfg.layer_id = 1;
            ret = ioctl(disp_fd_, DISP_LAYER_SET_CONFIG, &cfg);
            if (ret < 0) {
                std::fprintf(stderr,
                             "[DISPLAY] DISP_LAYER_SET_CONFIG failed "
                             "(layer=1): %s\n",
                             std::strerror(errno));
            } else {
                std::printf("[DISPLAY] Layer 1 set to "
                            "highest priority (zorder=F)\n");
            }
        } else {
            std::printf("[DISPLAY] Layer 0 set to "
                        "highest priority (zorder=F, alpha=0xFF)\n");
        }

        ::close(disp_fd_);
        disp_fd_ = -1;
    }

    bool present(const BgrFrame& frame) override
    {
        if (!opened_ || !frame.valid()) {
            std::fprintf(stderr,
                         "[DISPLAY] present failed: "
                         "opened=%d, frame_valid=%d, "
                         "width=%d, height=%d\n",
                         opened_, frame.valid(),
                         frame.width, frame.height);
            return false;
        }

        const int copy_width =
            std::min(frame.width, width_);
        const int copy_height =
            std::min(frame.height, height_);

        uint8_t* dst_base = fb_mmap_;
        if (!dst_base) {
            dst_base = fb_buf_;
        }
        if (!dst_base) {
            std::fprintf(stderr,
                         "[DISPLAY] No framebuffer buffer\n");
            return false;
        }

        /* 将 BGR24 转换为 framebuffer 的 32 位 BGRA 格式,
         * 逐行写入缓冲区 */
        for (int y = 0; y < copy_height; ++y) {
            const uint8_t* src =
                frame.data.data() +
                static_cast<size_t>(y) * frame.stride;
            uint8_t* dst =
                dst_base +
                static_cast<size_t>(y) * stride_;

            if (bytes_per_pixel_ == 4) {
                /* BGR24 -> BGRA32:
                 *   byte 0 = B, byte 1 = G,
                 *   byte 2 = R, byte 3 = 0xFF */
                for (int x = 0; x < copy_width; ++x) {
                    dst[x * 4 + 0] = src[x * 3 + 0];
                    dst[x * 4 + 1] = src[x * 3 + 1];
                    dst[x * 4 + 2] = src[x * 3 + 2];
                    dst[x * 4 + 3] = 0xFF;
                }
            } else if (bytes_per_pixel_ == 2) {
                /* BGR24 -> RGB565 */
                uint16_t* dst16 =
                    reinterpret_cast<uint16_t*>(dst);
                for (int x = 0; x < copy_width; ++x) {
                    const uint32_t b = src[x * 3] >> 3;
                    const uint32_t g =
                        src[x * 3 + 1] >> 2;
                    const uint32_t r =
                        src[x * 3 + 2] >> 3;
                    dst16[x] = static_cast<uint16_t>(
                        (r << 11) | (g << 5) | b);
                }
            } else {
                std::memcpy(dst, src,
                            static_cast<size_t>(copy_width) *
                            3);
            }
        }

        /* 如果使用 mmap，数据已经直接写入 framebuffer 内存
         * 如果使用 write() 回退模式，需要写入 */
        if (fb_mmap_) {
            /* mmap 模式: 数据已经写入 framebuffer，无需额外操作 */
            frame_count_++;
            if (frame_count_ % 30 == 0) {
                std::printf("[DISPLAY] Frame %llu displayed "
                            "(mmap mode, %dx%d)\n",
                            static_cast<unsigned long long>(frame_count_),
                            copy_width, copy_height);
            }
            return true;
        } else if (fb_buf_) {
            /* write() 回退模式 */
            if (lseek(fd_, 0, SEEK_SET) < 0) {
                std::fprintf(stderr,
                             "[DISPLAY] lseek fb failed: %s\n",
                             std::strerror(errno));
                return false;
            }

            size_t written = 0;
            while (written < fb_size_) {
                ssize_t n = ::write(fd_,
                                    fb_buf_ + written,
                                    fb_size_ - written);
                if (n < 0) {
                    std::fprintf(stderr,
                                 "[DISPLAY] write fb failed: %s\n",
                                 std::strerror(errno));
                    return false;
                }
                if (n == 0) break;
                written += static_cast<size_t>(n);
            }
            frame_count_++;
            if (frame_count_ % 30 == 0) {
                std::printf("[DISPLAY] Frame %llu displayed "
                            "(write mode, %zu bytes written)\n",
                            static_cast<unsigned long long>(frame_count_),
                            written);
            }
            return written == fb_size_;
        }

        return false;
    }

    void close() override
    {
        if (fb_mmap_ && fb_mmap_ != MAP_FAILED) {
            munmap(fb_mmap_, fb_size_);
            fb_mmap_ = NULL;
        }
        if (fb_buf_) {
            std::free(fb_buf_);
            fb_buf_ = NULL;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (disp_fd_ >= 0) {
            ::close(disp_fd_);
            disp_fd_ = -1;
        }
        opened_ = false;
        width_ = 0;
        height_ = 0;
        stride_ = 0;
        fb_size_ = 0;
        std::printf("[DISPLAY] Framebuffer closed, total frames: %llu\n",
                    static_cast<unsigned long long>(frame_count_));
    }

    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    int fd_;
    int disp_fd_;
    uint8_t* fb_mmap_;
    uint8_t* fb_buf_;
    size_t fb_size_;
    int width_;
    int height_;
    int stride_;
    int bytes_per_pixel_;
    int red_offset_;
    int green_offset_;
    int blue_offset_;
    int alpha_offset_;
    uint64_t frame_count_;
    bool opened_;
};

} // namespace

std::unique_ptr<IDisplay> create_display()
{
    return std::unique_ptr<IDisplay>(
        new FramebufferDisplay());
}
