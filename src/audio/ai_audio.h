// ============================================================================
// ai_audio.h — AI 音频处理链（每会话一个实例）：GTCRN 降噪 → Silero VAD
//
// 架构决策（替代 webrtc APM 的 NS+VAD）：
//   1. **先降噪**（GTCRN，gtcrn_simple.onnx）：AI 模型，对真机非平稳噪声
//      （街道/电视/AGC 泵起）远优于 webrtc 谱减法 NS；
//   2. **后 VAD**（Silero，silero_vad.onnx）：在净语音上判有声/断句，
//      噪声幻听几乎为零——真机断句卡死（回复等 60s 强制断句）的根治。
//
// 延迟预算：GTCRN 为离线模型（逐段处理），按 ~200ms 块滚动喂入，
// 上行额外延迟 ≈ 200ms（ASR 断句场景可忽略）。
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

namespace mediator::audio {

class AiAudioChain {
public:
    struct Out {
        std::vector<int16_t> pcm; // 降噪后 PCM（16k，与输入等长）
        bool speech = false;      // 本块是否处于语音中
        bool endpoint = false;    // 本块完成一次断句（语音段闭合）
    };

    AiAudioChain() = default;
    ~AiAudioChain() {
        if (m_vad) SherpaOnnxDestroyVoiceActivityDetector(m_vad);
        if (m_sd) SherpaOnnxDestroyOfflineSpeechDenoiser(m_sd);
    }
    AiAudioChain(const AiAudioChain&) = delete;
    AiAudioChain& operator=(const AiAudioChain&) = delete;

    bool Init(const std::string& gtcrn_model, const std::string& silero_model,
              std::string* err) {
        // GTCRN 降噪
        SherpaOnnxOfflineSpeechDenoiserConfig dcfg;
        std::memset(&dcfg, 0, sizeof(dcfg));
        dcfg.model.gtcrn.model = gtcrn_model.c_str();
        dcfg.model.num_threads = 1;
        dcfg.model.provider = "cpu";
        m_sd = SherpaOnnxCreateOfflineSpeechDenoiser(&dcfg);
        if (!m_sd) {
            if (err) *err = "create denoiser failed: " + gtcrn_model;
            return false;
        }
        // Silero VAD：0.7s 最小静音断句（普通话句内停顿余量，
        // 0.5s 会把带自然停顿的一句话切成两段→碎片打断正解），
        // 12s 最大语音强制切分（防 VAD 卡死永不闭合；真机会话开始 AGC
        // 爬坡期噪声把语音概率维持在阈值上，分段 28s 不闭合、首问无
        // 响应的根因——阈值 0.5→0.55 让弱噪声不再续命语音态）
        SherpaOnnxVadModelConfig vcfg;
        std::memset(&vcfg, 0, sizeof(vcfg));
        vcfg.silero_vad.model = silero_model.c_str();
        vcfg.silero_vad.threshold = 0.55f;
        vcfg.silero_vad.min_silence_duration = 0.7f;
        vcfg.silero_vad.min_speech_duration = 0.1f;
        vcfg.silero_vad.window_size = 512;
        vcfg.silero_vad.max_speech_duration = 12.0f;
        vcfg.sample_rate = 16000;
        vcfg.num_threads = 1;
        vcfg.provider = "cpu";
        m_vad = SherpaOnnxCreateVoiceActivityDetector(&vcfg, 30.0f);
        if (!m_vad) {
            if (err) *err = "create vad failed: " + silero_model;
            return false;
        }
        return true;
    }

    bool Valid() const { return m_sd && m_vad; }

    // 20ms 上行块（16k int16 mono）喂入；内部按 200ms 块降噪+VAD
    Out Process(const std::vector<int16_t>& pcm) {
        Out out;
        if (!Valid()) {
            out.pcm = pcm;
            return out;
        }
        m_pending.insert(m_pending.end(), pcm.begin(), pcm.end());
        constexpr size_t kBlock = 3200; // 200ms@16k
        if (m_pending.size() < kBlock) {
            out.speech = m_lastSpeech;
            return out;
        }
        std::vector<int16_t> block(m_pending.begin(), m_pending.begin() + kBlock);
        m_pending.erase(m_pending.begin(), m_pending.begin() + kBlock);

        // 1. GTCRN 降噪
        std::vector<float> f(block.size());
        for (size_t i = 0; i < block.size(); ++i) f[i] = block[i] / 32768.0f;
        const auto* den = SherpaOnnxOfflineSpeechDenoiserRun(
            m_sd, f.data(), static_cast<int32_t>(f.size()), 16000);
        if (den) {
            out.pcm.resize(std::min<size_t>(den->n, block.size()));
            for (size_t i = 0; i < out.pcm.size(); ++i) {
                float v = den->samples[i];
                v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                out.pcm[i] = static_cast<int16_t>(v * 32767.0f);
            }
            SherpaOnnxDestroyDenoisedAudio(const_cast<SherpaOnnxDenoisedAudio*>(den));
        } else {
            out.pcm = block; // 降噪失败透传
        }

        // 2. Silero VAD（降噪后）
        std::vector<float> vf(out.pcm.size());
        for (size_t i = 0; i < out.pcm.size(); ++i) vf[i] = out.pcm[i] / 32768.0f;
        SherpaOnnxVoiceActivityDetectorAcceptWaveform(m_vad, vf.data(),
                                                      static_cast<int32_t>(vf.size()));
        out.speech = SherpaOnnxVoiceActivityDetectorDetected(m_vad) != 0;
        m_lastSpeech = out.speech;
        // 段闭合 = 断句事件（silero 内置 0.5s 静音滞后）
        if (!SherpaOnnxVoiceActivityDetectorEmpty(m_vad)) {
            while (!SherpaOnnxVoiceActivityDetectorEmpty(m_vad)) {
                const auto* seg = SherpaOnnxVoiceActivityDetectorFront(m_vad);
                if (seg) SherpaOnnxDestroySpeechSegment(seg);
                SherpaOnnxVoiceActivityDetectorPop(m_vad);
            }
            out.endpoint = true;
        }
        return out;
    }

private:
    const SherpaOnnxOfflineSpeechDenoiser* m_sd = nullptr;
    const SherpaOnnxVoiceActivityDetector* m_vad = nullptr;
    std::deque<int16_t> m_pending;
    bool m_lastSpeech = false;
};

} // namespace mediator::audio
