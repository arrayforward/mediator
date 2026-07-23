// ============================================================================
// watermark.h — AEC3 参考信号对齐：双音水印标定法（设计文档 §6.5）
//
// 问题背景：
//   AEC3 要求录音(capture)与放音参考(render)严格对齐。音频经 WSS 往返
//   端侧，端侧 DAC/ADC 时钟独立，存在未知环路延迟与时钟漂移，网关无法
//   用系统时间对齐，导致 AEC 收敛差。
//
// 实现思路：
//   1. 会话开始录音前，下行注入"多脉冲 chirp 水印"（1k→4kHz 扫频，8 声、
//      固定间隔 GAP，总时长 2.26s）。选 chirp 是因为宽带信号互相关峰尖锐，
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

// 双音水印标定（§6.5）：chirp 模板生成 + NCC 匹配滤波检测 + 延迟/漂移估计
// 注：生成侧发出 chirp_count 个等间隔（gap_ms）脉冲，检测端只需识别其中
//     任意相邻双峰（真机扬声器→麦克风回环损耗大，多脉冲冗余提高命中率；
//     脉冲越多水印越长越可闻，标定成功率越高）
struct WatermarkConfig {
    int sample_rate = 16000;
    double chirp_f0 = 1000.0;     // 起始频率 Hz
    double chirp_f1 = 4000.0;     // 结束频率 Hz
    int chirp_ms = 20;            // 单声时长
    int gap_ms = 320;             // 相邻脉冲固定间隔（起点到起点）。
                                  // 注：skew 测量分辨率 ≈ 1/间隔采样数，间隔越长
                                  // 精度越高；320ms@16k=5120采样，配合抛物线
                                  // 亚采样插值可达 <20ppm（80ms 物理上做不到）
    double amplitude = 0.9;       // 接近满幅，int16 留 ~10% 余量防削波
    double ncc_threshold = 0.6;   // 检测阈值
    int interval_tolerance_ms = 2;
    int chirp_count = 8;          // 脉冲数（仅生成侧用）：8×320ms → 总时长 2.26s
};

struct WatermarkDetectResult {
    bool detected = false;
    int64_t p1 = 0;          // 第一声峰值采样位置
    int64_t p2 = 0;          // 第二声峰值采样位置
    double skew = 0.0;       // 时钟漂移 = 实测间隔/期望间隔 - 1
    double peak_ncc = 0.0;
    // 诊断（超时 bypass WARN 用）：NCC 峰数 / 最优候选栅格命中槽数
    int debug_peaks = 0;
    int debug_matched_slots = 0;
};

// 生成双 chirp 水印 PCM
std::vector<int16_t> GenerateWatermark(const WatermarkConfig& cfg);

// 在录音 PCM 中检测水印（滑动归一化互相关 + 双峰间隔校验）
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
