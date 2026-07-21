// ============================================================================
// apm_wrapper.h — WebRTC AudioProcessing 封装（设计文档 §2/§6.1）
//
// 实现思路：
//   每会话一个实例，16kHz 单声道 10ms 帧（160 采样）处理：
//   - ProcessRender：下行 PCM 回灌 reverse stream（AEC 参考信号）。
//     调用前按 §6.5 水印标定结果做 DelayLine 延迟补偿 + ResampleLinear
//     漂移校正（skew）。
//   - ProcessCapture：上行 PCM → AEC 回声消除 → NS 降噪 → 输出净语音，
//     附 VAD 语音概率（stream_has_voice）。
//
// 版本说明：系统包 webrtc-audio-processing 0.3.1（Ubuntu 22.04），
//   回声消除为 AECM（移动级）；AEC3 需要 webrtc-audio-processing 1.x，
//   接口已按"先 reverse 后 stream + 外部延迟补偿"的 AEC3 用法设计，
//   升级包后仅改初始化参数。
// ============================================================================
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "audio/align.h"

namespace webrtc { class AudioProcessing; }

namespace mediator::audio {

struct ApmCalib {
    int32_t delay_samples = 0; // 水印标定环路延迟
    double skew = 0.0;         // 时钟漂移
};

struct ApmResult {
    std::vector<int16_t> pcm;  // 处理后的净语音
    bool has_voice = false;    // APM VAD 判定
};

class ApmWrapper {
public:
    explicit ApmWrapper(ApmCalib calib = {});
    ~ApmWrapper();

    static constexpr int kSampleRate = 16000;
    static constexpr int kFrameSamples = 160; // 10ms

    void SetCalib(ApmCalib c);

    // 下行参考（TTS PCM，任意长度，内部按 10ms 切块入队）
    void ProcessRender(const std::vector<int16_t>& pcm);

    // 上行采集（任意长度，内部按 10ms 切块；不足一块缓存待用）
    ApmResult ProcessCapture(const std::vector<int16_t>& pcm);

private:
    void FeedRenderFrames();
    // webrtc::AudioProcessing 非线程安全：reverse/capture 可能分别来自
    // 线程池与网络读线程，所有公开入口必须经此锁串行化
    std::mutex m_mtx;
    ApmCalib m_calib;
    std::unique_ptr<webrtc::AudioProcessing> m_apm;
    std::unique_ptr<DelayLine> m_delay;
    std::deque<int16_t> m_renderQ;  // 对齐后的参考信号队列
    std::deque<int16_t> m_captureQ; // 上行凑帧缓冲
    bool m_hasVoice = false;
};

} // namespace mediator::audio
