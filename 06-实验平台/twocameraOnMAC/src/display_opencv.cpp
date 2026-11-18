#include "display.h"

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>

namespace {

class OpenCvDisplay : public IDisplay {
public:
    OpenCvDisplay() : width_(0), height_(0), opened_(false) {}

    ~OpenCvDisplay() override
    {
        close();
    }

    bool open(const DisplayConfig& config) override
    {
        close();
        title_ = config.window_title;
        width_ = config.preferred_width;
        height_ = config.preferred_height;
        cv::namedWindow(title_, cv::WINDOW_NORMAL);
        cv::resizeWindow(title_, width_, height_);
        opened_ = true;
        return true;
    }

    bool present(const BgrFrame& frame) override
    {
        if (!opened_ || !frame.valid()) {
            return false;
        }
        /* 检测窗口是否被用户关闭(点 X 后 highgui 销毁窗口,
         * WND_PROP_VISIBLE 返回 0)。若已关闭则返回 false,
         * 主循环据此 break 退出,避免 imshow 自动重建窗口
         * 造成"关闭后强制重新弹出"。 */
        if (cv::getWindowProperty(title_, cv::WND_PROP_VISIBLE) < 1) {
            return false;
        }
        cv::Mat image(frame.height, frame.width, CV_8UC3,
                      const_cast<uint8_t*>(frame.data.data()),
                      static_cast<size_t>(frame.stride));
        cv::imshow(title_, image);
        const int key = cv::waitKey(1);
        return key != 27 && key != 'q' && key != 'Q';
    }

    void close() override
    {
        if (opened_) {
            cv::destroyWindow(title_);
            opened_ = false;
        }
        width_ = 0;
        height_ = 0;
    }

    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    std::string title_;
    int width_;
    int height_;
    bool opened_;
};

} // namespace

std::unique_ptr<IDisplay> create_display()
{
    return std::unique_ptr<IDisplay>(new OpenCvDisplay());
}
