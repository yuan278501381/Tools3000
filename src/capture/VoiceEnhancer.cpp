#include "capture/VoiceEnhancer.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tools3000::capture {

namespace {

constexpr double kPi = std::numbers::pi;

void configureHighPass(int sr, double freqHz, double q, auto& bq) noexcept {
    const double w0 = 2.0 * kPi * freqHz / static_cast<double>(sr);
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);
    const double alpha = sinw / (2.0 * q);
    const double a0 = 1.0 + alpha;
    bq.b0 = ((1.0 + cosw) * 0.5) / a0;
    bq.b1 = -(1.0 + cosw) / a0;
    bq.b2 = ((1.0 + cosw) * 0.5) / a0;
    bq.a1 = (-2.0 * cosw) / a0;
    bq.a2 = (1.0 - alpha) / a0;
}

void configurePeakingEq(int sr, double freqHz, double q, double gainDb, auto& bq) noexcept {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * freqHz / static_cast<double>(sr);
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);
    const double alpha = sinw / (2.0 * q);
    const double a0 = 1.0 + alpha / A;
    bq.b0 = (1.0 + alpha * A) / a0;
    bq.b1 = (-2.0 * cosw) / a0;
    bq.b2 = (1.0 - alpha * A) / a0;
    bq.a1 = (-2.0 * cosw) / a0;
    bq.a2 = (1.0 - alpha / A) / a0;
}

} // namespace

VoiceEnhancer::VoiceEnhancer(int sampleRate) noexcept
    : m_sampleRate((std::max)(8000, sampleRate)) {
    initFilters();
}

void VoiceEnhancer::initFilters() noexcept {
    // 1. 85Hz Butterworth HPF (Q = 0.7071): 滤除桌面低频震动、空调轰鸣与喷麦爆破音
    configureHighPass(m_sampleRate, 85.0, 0.7071, m_highPass85Hz);
    // 2. 260Hz 削减 -2.2dB: 消除桌面反射与廉价麦箱体闷罐感
    configurePeakingEq(m_sampleRate, 260.0, 1.15, -2.2, m_mudCut260Hz);
    // 3. 3400Hz 提亮 +3.2dB: 增强中英文辅音清晰度与播客质感
    configurePeakingEq(m_sampleRate, 3400.0, 0.90, 3.2, m_presenceBoost3400Hz);
}

void VoiceEnhancer::reset() noexcept {
    m_highPass85Hz.resetState();
    m_mudCut260Hz.resetState();
    m_presenceBoost3400Hz.resetState();
    m_fastEnv = 0.0f;
    m_noiseFloor = 0.0018f;
    m_gateGain = 1.0f;
    m_compEnv = 0.0f;
    m_voiceHoldSec = 0.0f;
    m_duckingGain = 1.0f;
}

float VoiceEnhancer::processInterleavedStereo(float* samples, int frameCount) noexcept {
    if (!samples || frameCount <= 0) {
        return 0.0f;
    }

    const float sr = static_cast<float>(m_sampleRate);
    const float dt = 1.0f / sr;
    const float envAttack = std::exp(-1.0f / (0.0025f * sr));   // 2.5ms 极速包络捕捉字头
    const float envRelease = std::exp(-1.0f / (0.045f * sr));   // 45ms 平滑释放
    const float gateOpenCoeff = std::exp(-1.0f / (0.0020f * sr));
    const float gateCloseCoeff = std::exp(-1.0f / (0.140f * sr));
    const float compAttack = std::exp(-1.0f / (0.008f * sr));
    const float compRelease = std::exp(-1.0f / (0.160f * sr));

    constexpr float kGateFloorGain = 0.16f; // -16dB 自然柔化底噪，杜绝死寂断层感 (Pumping)
    constexpr float kMakeupGain = 1.32f;    // +2.4dB 播客响度补偿
    constexpr float kCompThresh = 0.22f;    // -13dBFS 启动柔和稳幅压缩 (Ratio ~ 3:1)
    constexpr float kLimiterCeiling = 0.89f; // -1.0dBFS 防爆音天花板

    int activeSpeechFrames = 0;

    for (int i = 0; i < frameCount; ++i) {
        float L = samples[i * 2];
        float R = samples[i * 2 + 1];

        // Step 1: 85Hz 高通除震动喷麦
        m_highPass85Hz.processFrame(L, R);

        // Step 2: 自适应风扇底噪追踪与平滑扩展降噪门
        const float absPeak = (std::max)(std::abs(L), std::abs(R));
        if (absPeak > m_fastEnv) {
            m_fastEnv = envAttack * m_fastEnv + (1.0f - envAttack) * absPeak;
        } else {
            m_fastEnv = envRelease * m_fastEnv + (1.0f - envRelease) * absPeak;
        }

        // 慢速更新环境底噪本底 (钳制在合理物理区间 [-72dBFS, -36dBFS])
        if (m_fastEnv < m_noiseFloor) {
            m_noiseFloor = 0.996f * m_noiseFloor + 0.004f * m_fastEnv;
        } else if (m_fastEnv < m_noiseFloor * 2.1f) {
            m_noiseFloor = 0.99985f * m_noiseFloor + 0.00015f * m_fastEnv;
        }
        m_noiseFloor = std::clamp(m_noiseFloor, 0.00025f, 0.016f);

        const float speechThreshold = (std::max)(0.0065f, m_noiseFloor * 2.85f);
        const bool isSpeech = (m_fastEnv >= speechThreshold);
        if (isSpeech) {
            m_voiceHoldSec = 0.16f; // 160ms 字尾保持，绝不切碎轻声韵母
            ++activeSpeechFrames;
        } else if (m_voiceHoldSec > 0.0f) {
            m_voiceHoldSec = (std::max)(0.0f, m_voiceHoldSec - dt);
        }

        const float targetGate = (isSpeech || m_voiceHoldSec > 0.0f) ? 1.0f : kGateFloorGain;
        if (targetGate > m_gateGain) {
            m_gateGain = gateOpenCoeff * m_gateGain + (1.0f - gateOpenCoeff) * targetGate;
        } else {
            m_gateGain = gateCloseCoeff * m_gateGain + (1.0f - gateCloseCoeff) * targetGate;
        }

        L *= m_gateGain;
        R *= m_gateGain;

        // Step 3: 人声双二阶清亮均衡 (去闷 + 齿音清晰度提亮)
        m_mudCut260Hz.processFrame(L, R);
        m_presenceBoost3400Hz.processFrame(L, R);

        // Step 4: 广播级自动增益压缩与软限幅
        L *= kMakeupGain;
        R *= kMakeupGain;
        const float postEqPeak = (std::max)(std::abs(L), std::abs(R));
        if (postEqPeak > m_compEnv) {
            m_compEnv = compAttack * m_compEnv + (1.0f - compAttack) * postEqPeak;
        } else {
            m_compEnv = compRelease * m_compEnv + (1.0f - compRelease) * postEqPeak;
        }

        float compGain = 1.0f;
        if (m_compEnv > kCompThresh) {
            // 3:1 柔和压缩曲线: out = thresh + (env - thresh) / 3.0
            const float targetOut = kCompThresh + (m_compEnv - kCompThresh) * 0.34f;
            compGain = targetOut / (m_compEnv + 1e-6f);
        }
        L *= compGain;
        R *= compGain;

        // 无削波平滑软限幅 (-1.0 dBFS)
        auto softLimit = [](float x) noexcept -> float {
            const float ax = std::abs(x);
            if (ax <= 0.75f) return x;
            const float sign = (x >= 0.0f) ? 1.0f : -1.0f;
            const float excess = (ax - 0.75f) / 0.25f;
            return sign * (0.75f + 0.14f * std::tanh(excess));
        };

        samples[i * 2] = std::clamp(softLimit(L), -kLimiterCeiling, kLimiterCeiling);
        samples[i * 2 + 1] = std::clamp(softLimit(R), -kLimiterCeiling, kLimiterCeiling);
    }

    m_duckingGain = 1.0f;
    return static_cast<float>(activeSpeechFrames) / static_cast<float>(frameCount);
}

} // namespace tools3000::capture
