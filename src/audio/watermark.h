// ============================================================================
// watermark.h — AEC3 参考信号对齐：双音水印标定法（设计文档 §6.5）
//
// 问题背景：
//   AEC3 要求录音(capture)与放音参考(render)严格对齐。音频经 WSS 往返
//   端侧，端侧 DAC/ADC 时钟独立，存在未知环路延迟与时钟漂移，网关无法
//   用系统时间对齐，导致 AEC 收敛差。
//
// 实现思路：
//   1. 会话开始录音前，下行注入"多脉冲 chirp 水印"（1k→4kHz 扫频，4 声、
//      固定间隔 GAP，总时长 0.98s）。选 chirp 是因为宽带信号互相关峰尖锐，
//      抗失真抗噪；多脉冲冗余应对真机扬声器→麦克风回环的高损耗。
//   2. 在录回的 PCM 中与本地模板做滑动归一化互相关（NCC 匹配滤波），
//      非极大抑制提取峰值；多脉冲一致性投票：候选首脉冲的栅格
//      p+k*GAP 命中 ≥count-2 槽且尾槽在位才判标定成功（拒离群误锁）：
//        环路延迟 D = p1 - 水印播放时刻（按已下发采样计数推算）
//        时钟漂移 skew = 实测间隔/期望间隔 - 1
//   3. 置信度不足（单峰/间隔不符/峰弱）→ 重发，最多 3 次，
//      仍失败则降级：关 AEC 仅 NS+VAD，并上报指标。
//   4. 检测经 G.711A 通路时，模板同样过一遍编解码再相关（调用方职责），
//      避免编解码失真削弱相关峰。
//
// 纯 DSP、无状态、确定性，可直接单元测试（§6.5.4）。
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace mediator::audio {

// 叮咚水印标定：单发双音铃（叮→咚）模板 + NCC 匹配滤波检测 + 延迟估计。
// 真机体验优化：原 chirp 扫频脉冲音刺耳，改为门铃式"叮咚"；
// 抗手机本地降噪：持续纯音是硬件 NS 最易消除的信号（平稳单音），
// 故双音均采用快速扫频（非平稳，NS/AEC 难以锁定），听感仍为"叮咚"，
// 自相关性保持 NCC 峰尖锐。单发播放，检测失败按间隔重发。
struct WatermarkConfig {
    int sample_rate = 16000;
    double ding_f0 = 1400.0;    // "叮"扫频起点 Hz（G.711A 8k 通带内）
    double ding_f1 = 1000.0;    // "叮"扫频终点 Hz
    double dong_f0 = 1000.0;    // "咚"扫频起点 Hz
    double dong_f1 = 600.0;     // "咚"扫频终点 Hz
    int ding_ms = 140;          // 叮时长（指数衰减）
    int tone_gap_ms = 40;       // 两音间隔
    int dong_ms = 260;          // 咚时长（指数衰减，余音自然）
    double amplitude = 0.85;    // int16 留 ~15% 余量防削波
    double ncc_threshold = 0.6; // 检测阈值（模板"叮"前缀自相关 ~0.55，须高于此防截断误判）
    double min_peak_margin = 0.12; // 主峰需超次峰的余量（抗噪声误检）
};

struct WatermarkDetectResult {
    bool detected = false;
    int64_t p1 = 0;          // 峰值采样位置（环路延迟）
    int64_t p2 = 0;          // 兼容字段（单发水印恒等于 p1）
    double skew = 0.0;       // 时钟漂移（单发无法测量，恒 0）
    double peak_ncc = 0.0;
    // 诊断（超时 WARN 用）：过阈值峰数 / 主峰-次峰余量(×1000)
    int debug_peaks = 0;
    int debug_matched_slots = 0;
};

// 生成单发"叮咚"水印 PCM
std::vector<int16_t> GenerateWatermark(const WatermarkConfig& cfg);

// 在录音 PCM 中检测水印（滑动归一化互相关 + 峰显著性校验）
WatermarkDetectResult DetectWatermark(const std::vector<int16_t>& capture,
                                      const WatermarkConfig& cfg);

// 由播放时刻（采样计数）与检测结果计算环路延迟与漂移
struct CalibResult {
    int32_t delay_samples;
    double skew;
};
CalibResult ComputeCalib(const WatermarkDetectResult& det, int64_t play_start_sample,
                         const WatermarkConfig& cfg);

} // namespace mediator::audio
