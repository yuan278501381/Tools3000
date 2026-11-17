#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// VoiceEnhancer.h — 播客级实时麦克风人声降噪与动态均衡增强引擎 (48kHz Stereo DSP)
//
// 处理链路 (严格仅作用于麦克风输入通道，电脑系统声音 100% 原样无损直通):
//   1. 85Hz 二阶巴特沃斯高通滤波 (HPF)：滤除空调低频嗡鸣、桌面敲击震动与喷麦噗声
//   2. 自适应底噪本底追踪与平滑降噪门 (Noise Suppressor)：静音间隙衰减环境风扇嘶嘶声，2ms 瞬态开启零吞字
//   3. 双二阶播客均衡器 (Biquad EQ)：260Hz (-2.0dB) 削减鼻音浑浊 + 3.4kHz (+3.2dB) 提升人声齿音穿透力
//   4. 广播级自动增益稳幅与软限幅 (AGC Compressor + -1.0dBFS Tanh Soft Limiter)：小声提亮、大声防爆音
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_VOICEENHANCER_H
#define TOOLS3000_CAPTURE_VOICEENHANCER_H

#include <cstddef>

namespace tools3000::capture {

class VoiceEnhancer {
public:
    explicit VoiceEnhancer(int sampleRate = 48000) noexcept;

    void reset() noexcept;

    /// 原地处理交错立体声浮点采样 (interleaved L/R, [-1.0, 1.0])，返回当前块语音活跃度 [0.0, 1.0]
    float processInterleavedStereo(float* samples, int frameCount) noexcept;

    /// 系统音频直通增益乘数（恒为 1.0，电脑系统声音 100% 原样保留不做闪避压低）
    float currentSystemDuckingGain() const noexcept { return 1.0f; }

private:
    struct BiquadState {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a1 = 0.0, a2 = 0.0;
        double z1L = 0.0, z2L = 0.0;
        double z1R = 0.0, z2R = 0.0;

        void resetState() noexcept {
            z1L = z2L = z1R = z2R = 0.0;
        }

        void processFrame(float& left, float& right) noexcept {
            const double inL = static_cast<double>(left);
            const double outL = b0 * inL + z1L;
            z1L = b1 * inL - a1 * outL + z2L;
            z2L = b2 * inL - a2 * outL;
            left = static_cast<float>(outL);

            const double inR = static_cast<double>(right);
            const double outR = b0 * inR + z1R;
            z1R = b1 * inR - a1 * outR + z2R;
            z2R = b2 * inR - a2 * outR;
            right = static_cast<float>(outR);
        }
    };

    void initFilters() noexcept;

    int m_sampleRate = 48000;
    BiquadState m_highPass85Hz{};
    BiquadState m_mudCut260Hz{};
    BiquadState m_presenceBoost3400Hz{};

    float m_fastEnv = 0.0f;
    float m_noiseFloor = 0.0018f;
    float m_gateGain = 1.0f;
    float m_compEnv = 0.0f;
    float m_voiceHoldSec = 0.0f;
    float m_duckingGain = 1.0f;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_VOICEENHANCER_H
