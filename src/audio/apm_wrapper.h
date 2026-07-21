// ============================================================================
// apm_wrapper.h — WebRTC AudioProcessing 封装（设计文档 §2/§6.1）
//
// 并发模型（优化：无锁 SPSC，替代互斥串行化）：
//   webrtc::AudioProcessing 非线程安全，且 AEC 语义上要求"先 reverse 后
//   capture"的顺序性——因此不做锁，而是**单线程所有权**：
//   - 生产线程（TTS 下行）只调 PushRender：把 PCM 写入 SPSC 无锁环形队列
//     （单写单读、原子下标，零互斥，拷贝即返回）；
//   - 消费线程（上行 capture 路径）调 ProcessCapture：先排空渲染队列
//     （skew 重采样 → DelayLine 对齐 → ProcessReverseStream），再做
//     ProcessStream(AEC/NS/VAD)。APM 全程只被这一个线程触碰。
//   队列溢出（下行积压超容量）丢最老参考帧并计数——AEC 对少量远端
//   参考缺失有容忍度，且表明节奏异常应暴露指标。
//   跨会话天然并行（每会话独立实例+独立队列）。
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
