#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// GestureRecognizer — 手势识别算法
//
// 核心算法: 方向编码法
//   1. 将鼠标轨迹实时分解为方向段 (U/D/L/R/UL/UR/DL/DR)
//   2. 每个方向段必须满足最小移动距离（防抖）
//   3. 角度容差 ±22.5° 范围内归类为同一方向
//   4. 最终生成方向编码字符串 (如 "L-U-R") 进行匹配
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_GESTURERECOGNIZER_H
#define TOOLS3000_GESTURE_GESTURERECOGNIZER_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <cmath>
#include <optional>

namespace tools3000::gesture {

/// 八方向枚举
enum class Direction : uint8_t {
    None = 0,
    Up,         // ↑
    Down,       // ↓
    Left,       // ←
    Right,      // →
    UpLeft,     // ↖
    UpRight,    // ↗
    DownLeft,   // ↙
    DownRight   // ↘
};

using DirectionCodeBits = uint64_t;

/// 将最多 15 段方向序列高效打包为单个 64 位无符号整数
/// [bit 0..3]: 段数 count (0~15)
/// [bit 4..7]: 段 0 方向 (Direction 0~8)
/// [bit 8..11]: 段 1 方向
/// ...
inline DirectionCodeBits packDirections(const std::vector<Direction>& dirs) noexcept {
    if (dirs.empty()) return 0;
    const size_t count = (std::min)(dirs.size(), size_t(15));
    DirectionCodeBits bits = static_cast<DirectionCodeBits>(count & 0x0F);
    for (size_t i = 0; i < count; ++i) {
        bits |= (static_cast<DirectionCodeBits>(dirs[i]) & 0x0F) << (4 * (i + 1));
    }
    return bits;
}

inline std::vector<Direction> unpackDirections(DirectionCodeBits bits) {
    std::vector<Direction> dirs;
    const size_t count = bits & 0x0F;
    dirs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        dirs.push_back(static_cast<Direction>((bits >> (4 * (i + 1))) & 0x0F));
    }
    return dirs;
}

/// 方向 → 字符串编码
inline std::string directionToCode(Direction dir) {
    switch (dir) {
        case Direction::Up:        return "U";
        case Direction::Down:      return "D";
        case Direction::Left:      return "L";
        case Direction::Right:     return "R";
        case Direction::UpLeft:    return "UL";
        case Direction::UpRight:   return "UR";
        case Direction::DownLeft:  return "DL";
        case Direction::DownRight: return "DR";
        default:                   return "";
    }
}

/// 方向 → 显示用箭头符号
inline std::string directionToArrow(Direction dir) {
    switch (dir) {
        case Direction::Up:        return "↑";
        case Direction::Down:      return "↓";
        case Direction::Left:      return "←";
        case Direction::Right:     return "→";
        case Direction::UpLeft:    return "↖";
        case Direction::UpRight:   return "↗";
        case Direction::DownLeft:  return "↙";
        case Direction::DownRight: return "↘";
        default:                   return "";
    }
}

/// 方向序列 → 字符串编码 (如 "L-D")
inline std::string directionsToCode(const std::vector<Direction>& dirs) {
    std::string code;
    for (size_t i = 0; i < dirs.size(); ++i) {
        if (i > 0) code += "-";
        code += directionToCode(dirs[i]);
    }
    return code;
}

inline Direction tokenToDirection(std::string_view tok) noexcept {
    if (tok == "U") return Direction::Up;
    if (tok == "D") return Direction::Down;
    if (tok == "L") return Direction::Left;
    if (tok == "R") return Direction::Right;
    if (tok == "UL") return Direction::UpLeft;
    if (tok == "UR") return Direction::UpRight;
    if (tok == "DL") return Direction::DownLeft;
    if (tok == "DR") return Direction::DownRight;
    return Direction::None;
}

inline std::vector<Direction> codeToDirections(const std::string& code) {
    std::vector<Direction> out;
    size_t start = 0;
    while (start < code.size()) {
        const size_t dash = code.find('-', start);
        const std::string tok = code.substr(start, dash == std::string::npos ? std::string::npos : dash - start);
        const Direction dir = tokenToDirection(tok);
        if (dir != Direction::None) out.push_back(dir);
        if (dash == std::string::npos) break;
        start = dash + 1;
    }
    return out;
}

/// 方向序列 → 显示用箭头组合 (如 "←↓")
inline std::string directionsToArrowString(const std::vector<Direction>& dirs) {
    std::string result;
    for (const auto& dir : dirs) {
        result += directionToArrow(dir);
    }
    return result;
}

/// 单段对角编码在未单独映射时，等价于对应的直角两段（↘ ≈ ↓→）。
inline std::optional<std::string> expandSingleDiagonalCode(const std::string& code) {
    if (code == "DR") return std::string{"D-R"};
    if (code == "DL") return std::string{"D-L"};
    if (code == "UR") return std::string{"U-R"};
    if (code == "UL") return std::string{"U-L"};
    return std::nullopt;
}

/// 轨迹点
struct TrackPoint {
    int x;
    int y;
};

/// 单段直角结果若轨迹存在明显转角第二段，则升级为两段编码（安全网，不把斜线误升成 L）。
std::string refineCodeWithPath(const std::string& code,
                               const std::vector<TrackPoint>& pts,
                               int minSegmentDistance) noexcept;

/// 手势识别结果
struct GestureResult {
    std::string code;                          // 方向编码 (如 "L-U-R")
    std::vector<Direction> directions;          // 方向序列
    std::vector<TrackPoint> rawPoints;          // 原始轨迹点
    double totalDistance = 0.0;                 // 轨迹总长度（像素）

    bool isValid() const { return !directions.empty(); }

    /// 人类可读的箭头表示
    std::string toArrowString() const {
        std::string result;
        for (const auto& dir : directions) {
            result += directionToArrow(dir);
        }
        return result;
    }
};

/// 识别参数配置
struct RecognizerConfig {
    int minSegmentDistance   = 14;      // 最小方向段移动距离（像素，14px 极速跟手响应）
    int samplingInterval     = 2;       // 采样间隔（像素，2px 细腻平滑捕获）
    double angleToleranceDeg = 22.5;   // 基础角度容差；实际对基准方向额外留出自然手抖余量
    int maxDirections        = 10;      // 最大方向段数量（超过视为无效手势）
    bool enableScribbleCancel = true;   // 是否开启快速乱晃/打圈反悔取消手势
};

class GestureRecognizer {
public:
    explicit GestureRecognizer(const RecognizerConfig& config = {});

    /// 重置识别器（新手势开始时调用）
    void reset();

    /// 添加轨迹点（鼠标移动时调用）
    void addPoint(int x, int y);

    /// 完成识别（鼠标释放时调用）
    /// @return 识别结果，如果轨迹太短或无效返回 nullopt
    std::optional<GestureResult> finalize();

    /// 获取当前已识别的方向序列（常引用返回，彻底消除 vector 堆分配拷贝）
    const std::vector<Direction>& currentDirections() const;

    /// 获取 64 位紧凑定长数值方向编码
    DirectionCodeBits currentPackedDirections() const noexcept;

    /// 检测当前轨迹是否属于乱晃/原地打圈反悔取消行为
    bool isScribbleCanceled() const;

    /// 获取当前轨迹点数量
    size_t pointCount() const { return m_points.size(); }

    /// 更新配置
    void setConfig(const RecognizerConfig& config) { m_config = config; }

    /// 高阶手势方向段平滑与转弯圆弧折叠算法 (RDP & Fillet Simplification)
    static std::vector<Direction> simplifyDirections(const std::vector<Direction>& raw);

    const std::vector<TrackPoint>& points() const { return m_points; }
    const RecognizerConfig& config() const { return m_config; }

private:
    /// 计算两点之间的角度（弧度，0 = 正右，逆时针为正）
    static double calculateAngle(int x1, int y1, int x2, int y2);

    /// 角度 → 方向
    Direction angleToDirection(double angleRad) const;

    /// 计算两点之间的距离
    static double calculateDistance(int x1, int y1, int x2, int y2);

    /// 检查从拐点到当前点是否仍落在当前方向的扇区内。
    /// 不能只用单轴是否继续增大：自然的「下再右」横扫时 Y 往往还在慢慢增加，
    /// 旧判定会把整笔锁死在 D 上。
    bool isAdvancing(Direction dir, const TrackPoint& peak, const TrackPoint& current) const noexcept;

    /// 处理累积的点，提取方向段
    void processPoints();

    static constexpr size_t kMaxPoints = 4096;

    RecognizerConfig m_config;
    std::vector<TrackPoint> m_points;              // 所有轨迹点 (预分配 4096 点 0 堆扩容)
    std::vector<Direction> m_directions;           // 已识别的方向段序列
    std::vector<double> m_segmentLengths;          // 与 m_directions 对齐的段长

    // 当前方向段的累积状态 (基于拐点检测模型)
    TrackPoint m_segmentStart{0, 0};               // 当前段起点
    TrackPoint m_peakPoint{0, 0};                  // 当前段沿前进方向的最远极值点 (拐点)
    Direction m_currentDirection = Direction::None; // 当前段方向
    bool m_hasSegmentStart = false;

    // 增量式方向计算缓存 (微步位移无需重新全量计算 RDP 与几何拟合)
    mutable bool m_dirsDirty = true;
    mutable std::vector<Direction> m_cachedDirections;
    mutable DirectionCodeBits m_cachedPackedDirections{0};
    mutable TrackPoint m_lastPeakPoint{-1, -1};
};

}  // namespace tools3000::gesture

#endif  // TOOLS3000_GESTURE_GESTURERECOGNIZER_H
