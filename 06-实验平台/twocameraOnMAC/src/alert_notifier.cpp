#include "alert_notifier.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

/* ---------- ALSA 动态加载 (无编译期依赖) ----------
 * 仅用 5 个 C 接口函数 + 固定枚举值, 不引入 alsa 头文件:
 *   SND_PCM_STREAM_PLAYBACK = 0
 *   SND_PCM_FORMAT_U8       = 0
 *   SND_PCM_ACCESS_RW_INTERLEAVED = 3 */
typedef void snd_pcm_t;

namespace {

typedef int (*snd_pcm_open_fn)(snd_pcm_t**, const char*, int, int);
typedef int (*snd_pcm_set_params_fn)(snd_pcm_t*, int, int,
                                     unsigned, unsigned, int,
                                     unsigned);
typedef long (*snd_pcm_writei_fn)(snd_pcm_t*, const void*,
                                  unsigned long);
typedef int (*snd_pcm_drain_fn)(snd_pcm_t*);
typedef int (*snd_pcm_close_fn)(snd_pcm_t*);

/* 进程级只加载一次, 不 dlclose (分离线程可能仍在使用) */
snd_pcm_open_fn p_open = NULL;
snd_pcm_set_params_fn p_set_params = NULL;
snd_pcm_writei_fn p_writei = NULL;
snd_pcm_drain_fn p_drain = NULL;
snd_pcm_close_fn p_close = NULL;
bool g_alsa_loaded = false;

bool load_alsa()
{
    if (g_alsa_loaded) {
        return p_open != NULL;
    }
    g_alsa_loaded = true;
    void* handle = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        handle = dlopen("libasound.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (handle == NULL) {
        return false;
    }
    p_open = reinterpret_cast<snd_pcm_open_fn>(
        dlsym(handle, "snd_pcm_open"));
    p_set_params = reinterpret_cast<snd_pcm_set_params_fn>(
        dlsym(handle, "snd_pcm_set_params"));
    p_writei = reinterpret_cast<snd_pcm_writei_fn>(
        dlsym(handle, "snd_pcm_writei"));
    p_drain = reinterpret_cast<snd_pcm_drain_fn>(
        dlsym(handle, "snd_pcm_drain"));
    p_close = reinterpret_cast<snd_pcm_close_fn>(
        dlsym(handle, "snd_pcm_close"));
    if (!p_open || !p_set_params || !p_writei || !p_drain ||
        !p_close) {
        p_open = NULL;
        return false;
    }
    return true;
}

/* KIOCSOUND (PC 蜂鸣器): 频率分频值 = 1193180 / freq */
#ifndef KIOCSOUND
#define KIOCSOUND 0x4B2F
#endif
const unsigned long kPitBaseFreq = 1193180;

enum AlertBackend {
    ALERT_BACKEND_NONE = 0,    /* 仅日志 */
    ALERT_BACKEND_ALSA = 1,    /* ALSA default 设备 */
    ALERT_BACKEND_CONSOLE = 2  /* /dev/console 蜂鸣器 */
};

const char* backend_name(int backend)
{
    switch (backend) {
    case ALERT_BACKEND_ALSA:
        return "ALSA(default)";
    case ALERT_BACKEND_CONSOLE:
        return "console(KIOCSOUND)";
    default:
        return "log-only";
    }
}

/* 播放参数 (纯值拷贝进线程, 不触碰 AlertNotifier 成员,
 * 分离线程生命周期与对象解耦) */
struct ToneParams {
    int backend;
    int freq_hz;
    int duration_ms;
};

/* ALSA: 8kHz 单声道方波; S16_LE 优先 (U8 部分设备不支持, 兜底) */
bool play_alsa_tone(int freq_hz, int duration_ms)
{
    if (!load_alsa() || p_open == NULL) {
        return false;
    }
    /* 枚举值: SND_PCM_FORMAT_S16_LE=2, SND_PCM_FORMAT_U8=0 */
    static const int kFormats[] = {2, 0};
    const int rate = 8000;
    const int total = rate * duration_ms / 1000;
    const int period = std::max(2, rate / freq_hz);

    for (int fi = 0; fi < 2; ++fi) {
        const int format = kFormats[fi];
        snd_pcm_t* pcm = NULL;
        /* device="default", stream=PLAYBACK(0), mode=BLOCK(0) */
        if (p_open(&pcm, "default", 0, 0) != 0 ||
            pcm == NULL) {
            return false;
        }
        bool ok = false;
        do {
            /* access=RW_INTERLEAVED(3), ch=1, rate=8000,
             * soft_resample=1(允许), latency=50000us */
            if (p_set_params(pcm, format, 3, 1, 8000, 1,
                             50000) != 0) {
                break;
            }
            long written = -1;
            if (format == 2) { /* S16_LE */
                /* 方波 ±12000 (避免满幅爆音) */
                std::vector<short> buf(
                    static_cast<size_t>(total));
                for (int i = 0; i < total; ++i) {
                    buf[i] = (i % period) < (period / 2)
                                 ? 12000
                                 : -12000;
                }
                written = p_writei(
                    pcm, buf.data(),
                    static_cast<unsigned long>(total));
            } else { /* U8 */
                std::vector<unsigned char> buf(
                    static_cast<size_t>(total));
                for (int i = 0; i < total; ++i) {
                    buf[i] =
                        (i % period) < (period / 2) ? 240 : 16;
                }
                written = p_writei(
                    pcm, buf.data(),
                    static_cast<unsigned long>(total));
            }
            ok = (written == total);
        } while (false);
        if (ok) {
            p_drain(pcm);
            p_close(pcm);
            return true;
        }
        p_close(pcm);
        /* set_params/write 失败 → 换下一个格式重试 */
    }
    return false;
}

/* /dev/console 蜂鸣器 */
bool play_console_tone(int freq_hz, int duration_ms)
{
    const int fd = open("/dev/console", O_WRONLY);
    if (fd < 0) {
        return false;
    }
    int div = static_cast<int>(kPitBaseFreq /
                               std::max(1, freq_hz));
    bool ok = ioctl(fd, KIOCSOUND, div) == 0;
    if (ok) {
        /* 休眠 duration_ms 后关蜂鸣 */
        std::this_thread::sleep_for(
            std::chrono::milliseconds(duration_ms));
        ioctl(fd, KIOCSOUND, 0);
    }
    close(fd);
    return ok;
}

void play_tone_thread(ToneParams p)
{
    bool ok = false;
    if (p.backend == ALERT_BACKEND_ALSA) {
        ok = play_alsa_tone(p.freq_hz, p.duration_ms);
        if (!ok) {
            /* ALSA 打开失败 (设备被占/拔出) → 尝试蜂鸣器 */
            ok = play_console_tone(p.freq_hz,
                                   p.duration_ms);
        }
    } else if (p.backend == ALERT_BACKEND_CONSOLE) {
        ok = play_console_tone(p.freq_hz, p.duration_ms);
    }
    if (!ok) {
        std::fprintf(stderr,
                     "[ALERT] (no audio backend) new target\n");
    }
}

} // namespace

struct AlertNotifier::Impl {
    std::mutex mutex;
    std::chrono::steady_clock::time_point last_play;
    bool has_last_play = false;
    int backend = ALERT_BACKEND_NONE;
    int freq_hz = 880;
    int duration_ms = 120;
    int min_interval_ms = 500;
    bool stopped = false;
};

AlertNotifier::AlertNotifier() : impl_(new Impl()) {}

AlertNotifier::~AlertNotifier()
{
    impl_->stopped = true;
    delete impl_;
}

void AlertNotifier::set_tone(int freq_hz, int duration_ms)
{
    impl_->freq_hz = std::max(50, freq_hz);
    impl_->duration_ms = std::max(20, duration_ms);
}

void AlertNotifier::set_min_interval_ms(int ms)
{
    impl_->min_interval_ms = std::max(0, ms);
}

bool AlertNotifier::start()
{
    /* 1. ALSA (dlopen) */
    if (load_alsa() && p_open != NULL) {
        /* 空放一次探测 default 设备可打开 */
        snd_pcm_t* probe = NULL;
        if (p_open(&probe, "default", 0, 0) == 0 &&
            probe != NULL) {
            p_close(probe);
            impl_->backend = ALERT_BACKEND_ALSA;
        }
    }
    /* 2. /dev/console 蜂鸣器 */
    if (impl_->backend == ALERT_BACKEND_NONE) {
        const int fd = open("/dev/console", O_WRONLY);
        if (fd >= 0) {
            close(fd);
            impl_->backend = ALERT_BACKEND_CONSOLE;
        }
    }
    std::fprintf(stderr,
                 "[STEP] alert notifier backend: %s\n",
                 backend_name(impl_->backend));
    return impl_->backend != ALERT_BACKEND_NONE;
}

void AlertNotifier::stop()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopped = true;
}

void AlertNotifier::play_new_target_tone()
{
    ToneParams p;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stopped) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (impl_->has_last_play &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - impl_->last_play)
                    .count() < impl_->min_interval_ms) {
            return; /* 限流: 间隔内合并为一次 */
        }
        impl_->last_play = now;
        impl_->has_last_play = true;
        p.backend = impl_->backend;
        p.freq_hz = impl_->freq_hz;
        p.duration_ms = impl_->duration_ms;
    }
    /* 后端全无时直接日志 (不开线程) */
    if (p.backend == ALERT_BACKEND_NONE) {
        std::fprintf(stderr, "[ALERT] new target\n");
        return;
    }
    /* 分离线程播放, 不阻塞主循环 */
    std::thread(play_tone_thread, p).detach();
}
