#pragma once

/* 新目标提示音组件 (camerayolo.md 3.4.8):
 * ByteTrack 确认"新目标稳定存在"时通过回调触发, 播放一声短提示音。
 *
 * 后端探测 (启动时按序探测, 全部失败则降级为日志提示):
 *   1. ALSA default 设备 (dlopen libasound, 无编译/链接依赖):
 *      8kHz 单声道 U8 方波, 如 880Hz / 120ms
 *   2. /dev/console KIOCSOUND (PC 蜂鸣器, 需 root/udev 权限)
 *   3. 仅日志 "[ALERT] new target #<id>"
 *
 * 非阻塞: 播放在分离线程中执行, 不阻塞跟踪/显示主循环;
 * 限流: 距上次播放不足 min_interval_ms 时合并为一次 (多目标同时
 * 确认、或目标反复确认时避免连续鸣叫)。 */
class AlertNotifier {
public:
    AlertNotifier();
    ~AlertNotifier();

    /* 探测可用音频后端, 返回是否存在可用后端 (false=日志降级) */
    bool start();
    void stop();

    /* 非阻塞播放一声提示音 (内部限流) */
    void play_new_target_tone();

    /* 参数 (须在 start()/播放前设置) */
    void set_tone(int freq_hz, int duration_ms);
    void set_min_interval_ms(int ms);

private:
    AlertNotifier(const AlertNotifier&);            /* 禁止拷贝 */
    AlertNotifier& operator=(const AlertNotifier&);

    struct Impl;
    Impl* impl_;
};
