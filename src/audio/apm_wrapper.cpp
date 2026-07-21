// ============================================================================
// apm_wrapper.cpp — WebRTC APM 封装实现（0.3.1 API）
//
// 关键逻辑：
//   1. 初始化：EchoCancellation(AECM) + NoiseSuppression(高抑制) + VoiceDetection
//      set_stream_delay_ms(delay_samples/16) 告知 AEC 参考-采集延迟
//   2. 下行：skew 重采样 → DelayLine → 10ms 块 ProcessReverseStream
//   3. 上行：凑满 10ms 块 ProcessStream → 输出净语音 + VAD 概率
//   帧长硬约束：0.3.1 要求 10ms 帧，非 10ms 倍数缓存在队列中。
// ============================================================================
#include "audio/apm_wrapper.h"

#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/interface/module_common_types.h> // webrtc::AudioFrame

#include "core/log.h"

namespace mediator::audio {

ApmWrapper::ApmWrapper(ApmCalib calib) : m_calib(calib) {
    m_apm.reset(webrtc::AudioProcessing::Create());
    webrtc::Config config;
    // 0.3.1：AECM 开启舒适噪声；延迟由 set_stream_delay_ms 动态给
    config.Set<webrtc::ExtendedFilter>(new webrtc::ExtendedFilter(true));
    m_apm->SetExtraOptions(config);
    m_apm->echo_cancellation()->enable_drift_compensation(false); // 漂移自管理（skew）
    m_apm->echo_cancellation()->Enable(true);
    m_apm->noise_suppression()->set_level(webrtc::NoiseSuppression::kHigh);
    m_apm->noise_suppression()->Enable(true);
    m_apm->voice_detection()->Enable(true);
    m_apm->voice_detection()->set_likelihood(webrtc::VoiceDetection::kModerateLikelihood);
    // 0.3.1：AGC 开启时每帧必须 set_stream_analog_level，否则 ProcessStream
    // 报 kStreamParameterNotSetError(-11)。网关侧不需要 AGC，直接关闭。
    m_apm->gain_control()->Enable(false);
    m_apm->high_pass_filter()->Enable(true); // 高通滤除低频轰声，利于 AEC/VAD
    m_apm->Initialize(kSampleRate, kSampleRate, kSampleRate,
                      webrtc::AudioProcessing::kMono, webrtc::AudioProcessing::kMono,
                      webrtc::AudioProcessing::kMono);
    m_delay = std::make_unique<DelayLine>(calib.delay_samples);
}

ApmWrapper::~ApmWrapper() = default;

void ApmWrapper::SetCalib(ApmCalib c) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_calib = c;
    m_delay = std::make_unique<DelayLine>(c.delay_samples);
}

void ApmWrapper::ProcessRender(const std::vector<int16_t>& pcm) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto aligned = (m_calib.skew != 0.0) ? ResampleLinear(pcm, m_calib.skew) : pcm;
    auto delayed = m_delay->Process(aligned);
    m_renderQ.insert(m_renderQ.end(), delayed.begin(), delayed.end());
    FeedRenderFrames();
}

void ApmWrapper::FeedRenderFrames() {
    while (m_renderQ.size() >= static_cast<size_t>(kFrameSamples)) {
        webrtc::AudioFrame frame;
        frame.sample_rate_hz_ = kSampleRate;
        frame.num_channels_ = 1;
        frame.samples_per_channel_ = kFrameSamples;
        for (int i = 0; i < kFrameSamples; ++i) {
            frame.data_[i] = m_renderQ.front();
            m_renderQ.pop_front();
        }
        const int err = m_apm->ProcessReverseStream(&frame);
        if (err != 0) MDT_DEBUG("apm ProcessReverseStream err={}", err);
    }
}

ApmResult ApmWrapper::ProcessCapture(const std::vector<int16_t>& pcm) {
    std::lock_guard<std::mutex> lk(m_mtx);
    ApmResult res;
    m_captureQ.insert(m_captureQ.end(), pcm.begin(), pcm.end());
    m_apm->set_stream_delay_ms(m_calib.delay_samples / (kSampleRate / 1000));
    while (m_captureQ.size() >= static_cast<size_t>(kFrameSamples)) {
        webrtc::AudioFrame frame;
        frame.sample_rate_hz_ = kSampleRate;
        frame.num_channels_ = 1;
        frame.samples_per_channel_ = kFrameSamples;
        int16_t raw[kFrameSamples];
        for (int i = 0; i < kFrameSamples; ++i) {
            raw[i] = m_captureQ.front();
            frame.data_[i] = raw[i];
            m_captureQ.pop_front();
        }
        const int err = m_apm->ProcessStream(&frame);
        if (err != 0) {
            MDT_DEBUG("apm ProcessStream err={}", err);
            res.pcm.insert(res.pcm.end(), raw, raw + kFrameSamples); // 失败透传
        } else {
            res.pcm.insert(res.pcm.end(), frame.data_, frame.data_ + kFrameSamples);
        }
        if (m_apm->voice_detection()->stream_has_voice()) m_hasVoice = true;
    }
    res.has_voice = m_hasVoice;
    m_hasVoice = false; // 帧级语义：本次块内是否有声
    return res;
}

} // namespace mediator::audio
