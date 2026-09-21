// ─────────────────────────────────────────────────────────────────────────────
// GestureRecognizer.cpp — 手势识别算法实现
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/GestureRecognizer.h"
#include "core/logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace tools3000::gesture {

namespace {

inline bool isCornerFillet(Direction a, Direction b, Direction c) noexcept {
    // b 是在直角转弯 (a -> c) 过程中由手腕/手指产生的自然圆角对角过渡
    if ((a == Direction::Down && c == Direction::Right && b == Direction::DownRight) ||
        (a == Direction::Right && c == Direction::Down && b == Direction::DownRight) ||
        (a == Direction::Down && c == Direction::Left  && b == Direction::DownLeft)  ||
        (a == Direction::Left  && c == Direction::Down && b == Direction::DownLeft)  ||
        (a == Direction::Up    && c == Direction::Right && b == Direction::UpRight)   ||
        (a == Direction::Right && c == Direction::Up    && b == Direction::UpRight)   ||
        (a == Direction::Up    && c == Direction::Left  && b == Direction::UpLeft)    ||
        (a == Direction::Left  && c == Direction::Up    && b == Direction::UpLeft)) {
        return true;
    }
    return false;
}

constexpr double kPi = std::numbers::pi;

double circularDeltaDeg(double a, double b) noexcept {
    double d = std::fabs(a - b);
    d = std::fmod(d, 360.0);
    if (d > 180.0) d = 360.0 - d;
    return d;
}

double directionCenterDeg(Direction dir) noexcept {
    switch (dir) {
        case Direction::Right:     return 0.0;
        case Direction::UpRight:   return 45.0;
        case Direction::Up:        return 90.0;
        case Direction::UpLeft:    return 135.0;
        case Direction::Left:      return 180.0;
        case Direction::DownLeft:  return 225.0;
        case Direction::Down:      return 270.0;
        case Direction::DownRight: return 315.0;
        default:                   return 0.0;
    }
}

inline bool isJitterRebound(Direction a, Direction b, Direction c) noexcept {
    if (a != c) return false;
    // 1. 180度反向回弹抖动 (如 Down -> Up -> Down, Left -> Right -> Left)
    if ((a == Direction::Left && b == Direction::Right) ||
        (a == Direction::Right && b == Direction::Left) ||
        (a == Direction::Up && b == Direction::Down) ||
        (a == Direction::Down && b == Direction::Up)) {
        return true;
    }
    // 2. 45度微小对角抖动 (如 Down -> DownRight -> Down, Right -> UpRight -> Right)
    if ((a == Direction::Down && (b == Direction::DownRight || b == Direction::DownLeft)) ||
        (a == Direction::Up   && (b == Direction::UpRight   || b == Direction::UpLeft))   ||
        (a == Direction::Left && (b == Direction::UpLeft    || b == Direction::DownLeft)) ||
        (a == Direction::Right&& (b == Direction::UpRight   || b == Direction::DownRight))) {
        return true;
    }
    return false;
}

inline std::optional<Direction> completeIncompleteFillet(Direction known, Direction diagonal) noexcept {
    if (diagonal == Direction::DownRight) {
        if (known == Direction::Down) return Direction::Right;
        if (known == Direction::Right) return Direction::Down;
    } else if (diagonal == Direction::DownLeft) {
        if (known == Direction::Down) return Direction::Left;
        if (known == Direction::Left) return Direction::Down;
    } else if (diagonal == Direction::UpRight) {
        if (known == Direction::Up) return Direction::Right;
        if (known == Direction::Right) return Direction::Up;
    } else if (diagonal == Direction::UpLeft) {
        if (known == Direction::Up) return Direction::Left;
        if (known == Direction::Left) return Direction::Up;
    }
    return std::nullopt;
}

struct DirectionSegment {
    Direction dir = Direction::None;
    double length = 1.0;
};

void compressSegments(std::vector<DirectionSegment>& segs) {
    std::vector<DirectionSegment> out;
    for (auto& s : segs) {
        if (s.dir == Direction::None || s.length <= 0.0) continue;
        if (!out.empty() && out.back().dir == s.dir) {
            out.back().length += s.length;
        } else {
            out.push_back(s);
        }
    }
    segs = std::move(out);
}

std::vector<Direction> dirsFromSegments(const std::vector<DirectionSegment>& segs) {
    std::vector<Direction> out;
    out.reserve(segs.size());
    for (const auto& s : segs) out.push_back(s.dir);
    return out;
}

std::vector<Direction> simplifySegments(std::vector<DirectionSegment> current) {
    compressSegments(current);

    bool modified = true;
    while (modified && current.size() >= 3) {
        modified = false;
        std::vector<DirectionSegment> next;
        for (size_t i = 0; i < current.size(); ) {
            if (i + 2 < current.size()) {
                const Direction a = current[i].dir;
                const Direction b = current[i + 1].dir;
                const Direction c = current[i + 2].dir;
                if (isJitterRebound(a, b, c)) {
                    next.push_back(current[i]);
                    i += 3;
                    modified = true;
                    continue;
                }
                if (isCornerFillet(a, b, c)) {
                    next.push_back(current[i]);
                    next.push_back(current[i + 2]);
                    i += 3;
                    modified = true;
                    continue;
                }
            }
            next.push_back(current[i]);
            ++i;
        }
        current = std::move(next);
        compressSegments(current);
    }

    bool filletModified = true;
    while (filletModified && current.size() >= 2) {
        filletModified = false;
        if (current.size() == 2) {
            // 两段未走完的转角：D-DR → D-R。用户常在对角圆角上松手。
            if (auto completed = completeIncompleteFillet(current[0].dir, current[1].dir)) {
                current[1].dir = *completed;
                filletModified = true;
            }
        } else if (completeIncompleteFillet(current[current.size() - 2].dir, current.back().dir)) {
            // 三段以上：末尾对角是前一段的收笔圆角，不能发明新的直角段
            // （否则 D-R 末尾下垂会变成 D-R-D，截图手势被误触）。
            current.pop_back();
            filletModified = true;
        }
        if (current.size() >= 2) {
            if (auto invented = completeIncompleteFillet(current[1].dir, current[0].dir)) {
                if (current[0].length < current[1].length * 0.4) {
                    current.erase(current.begin());
                } else {
                    current[0].dir = *invented;
                }
                filletModified = true;
            }
        }
        compressSegments(current);
    }

    return dirsFromSegments(current);
}

} // namespace

std::string refineCodeWithPath(const std::string& code,
                               const std::vector<TrackPoint>& pts,
                               int minSegmentDistance) noexcept {
    if (code.empty() || code.find('-') != std::string::npos || pts.size() < 3) {
        return code;
    }
    if (code != "U" && code != "D" && code != "L" && code != "R") {
        return code;
    }

    const int minSeg = (std::max)(minSegmentDistance, 1);
    const int n = static_cast<int>(pts.size());
    const int sx = pts.front().x;
    const int sy = pts.front().y;
    const int ex = pts.back().x;
    const int ey = pts.back().y;

    struct TurnCandidate {
        int score = -1;
        int secondOrtho = 0;
    };
    TurnCandidate best;

    // 单纯倾斜的直线不应被“补全”为组合手势。真正的 L 形必须同时满足：
    // 1. 第一段已经走够；2. 第二段长度显著；3. 第二段由正交轴主导。
    // 第 3 条是关键：它把持续略向左的「下」与明确转向左的「下-左」分开。
    auto considerTurn = [minSeg, &best](int firstPrimary, int firstOrtho,
                                        int secondPrimary, int secondOrtho) noexcept {
        if (firstPrimary < minSeg * 2) return;
        const int secondLength = std::abs(secondOrtho);
        const int requiredLength = (std::max)(minSeg * 2, firstPrimary * 24 / 100);
        if (secondLength < requiredLength) return;

        // 第一段可以自然倾斜，但不能本来就是一条明显对角线。
        if (std::abs(firstOrtho) * 2 > firstPrimary) return;
        // 第二段必须真正转向正交轴，而不是沿原斜率继续漂移。
        if (secondLength * 4 < std::abs(secondPrimary) * 5) return;

        const int score = firstPrimary + secondLength -
            std::abs(firstOrtho) - std::abs(secondPrimary);
        if (score > best.score) best = {score, secondOrtho};
    };

    if (code == "D" || code == "U") {
        for (int i = 1; i < n - 1; ++i) {
            const int firstPrimary = (code == "D") ? (pts[i].y - sy) : (sy - pts[i].y);
            const int firstOrtho = pts[i].x - sx;
            const int secondPrimary = (code == "D") ? (ey - pts[i].y) : (pts[i].y - ey);
            const int secondOrtho = ex - pts[i].x;
            considerTurn(firstPrimary, firstOrtho, secondPrimary, secondOrtho);
        }
        if (best.score >= 0) {
            return (best.secondOrtho >= 0) ? (code + std::string("-R"))
                                           : (code + std::string("-L"));
        }
    } else {
        for (int i = 1; i < n - 1; ++i) {
            const int firstPrimary = (code == "R") ? (pts[i].x - sx) : (sx - pts[i].x);
            const int firstOrtho = pts[i].y - sy;
            const int secondPrimary = (code == "R") ? (ex - pts[i].x) : (pts[i].x - ex);
            const int secondOrtho = ey - pts[i].y;
            considerTurn(firstPrimary, firstOrtho, secondPrimary, secondOrtho);
        }
        if (best.score >= 0) {
            return (best.secondOrtho >= 0) ? (code + std::string("-D"))
                                           : (code + std::string("-U"));
        }
    }
    return code;
}

bool GestureRecognizer::isAdvancing(Direction dir, const TrackPoint& peak, const TrackPoint& current) const noexcept {
    const double dist = calculateDistance(peak.x, peak.y, current.x, current.y);
    if (dist <= 0.5) return true;
    double angle = calculateAngle(peak.x, peak.y, current.x, current.y);
    if (angle < 0.0) angle += 2.0 * kPi;
    const double deg = angle * 180.0 / kPi;
    // 比 45° 分箱更宽：自然下笔常偏 25–35°，不能因此切出假的第二段。
    const double cone = (std::clamp)(m_config.angleToleranceDeg * 1.75, 32.0, 42.0);
    return circularDeltaDeg(deg, directionCenterDeg(dir)) <= cone;
}

GestureRecognizer::GestureRecognizer(const RecognizerConfig& config)
    : m_config(config) {
    m_points.reserve(kMaxPoints);
    m_directions.reserve(16);
    m_segmentLengths.reserve(16);
    m_cachedDirections.reserve(16);
}

void GestureRecognizer::reset() {
    m_points.clear();
    m_directions.clear();
    m_segmentLengths.clear();
    m_currentDirection = Direction::None;
    m_hasSegmentStart = false;
    m_segmentStart = {0, 0};
    m_peakPoint = {0, 0};
    m_dirsDirty = true;
    m_cachedDirections.clear();
    m_cachedPackedDirections = 0;
    m_lastPeakPoint = {-1, -1};
}

void GestureRecognizer::addPoint(int x, int y) {
    // 采样过滤: 与上一个点距离太近则跳过 (默认 2px)
    if (!m_points.empty()) {
        double dist = calculateDistance(m_points.back().x, m_points.back().y, x, y);
        if (dist < m_config.samplingInterval) {
            return;
        }
    }

    if (m_points.size() < kMaxPoints) {
        m_points.push_back({x, y});
    } else {
        m_points.back() = {x, y};
    }

    // 初始化段起点
    if (!m_hasSegmentStart) {
        m_segmentStart = {x, y};
        m_peakPoint = {x, y};
        m_hasSegmentStart = true;
        m_dirsDirty = true;
        return;
    }

    // 实时处理方向段
    processPoints();
}

void GestureRecognizer::processPoints() {
    if (m_points.size() < 2 || !m_hasSegmentStart) return;

    const auto& current = m_points.back();
    const double distFromPeak = calculateDistance(
        m_peakPoint.x, m_peakPoint.y, current.x, current.y);
    const double deadzone =
        static_cast<double>((std::max)(m_config.samplingInterval, 2)) * 2.0;

    // 阶段 1: 尚未确定初始方向段
    if (m_currentDirection == Direction::None) {
        double dist = calculateDistance(m_segmentStart.x, m_segmentStart.y, current.x, current.y);
        if (dist < m_config.minSegmentDistance) return;

        double angle = calculateAngle(m_segmentStart.x, m_segmentStart.y, current.x, current.y);
        Direction dir = angleToDirection(angle);
        if (dir != Direction::None) {
            m_currentDirection = dir;
            m_peakPoint = current;
            m_dirsDirty = true;
        }
        return;
    }

    // 微步不更新拐点：否则 2px 采样会把峰值顺着圆角爬进第二段，L 形被锁成单段。
    if (distFromPeak < deadzone) {
        return;
    }

    // 阶段 2: 已有当前方向，判断是否沿当前方向继续推进
    if (isAdvancing(m_currentDirection, m_peakPoint, current)) {
        m_peakPoint = current;
        return;
    }

    // 阶段 3: 偏折拐弯检测 (从最远拐点计算偏折矢量)
    if (distFromPeak >= m_config.minSegmentDistance) {
        double turnAngle = calculateAngle(m_peakPoint.x, m_peakPoint.y, current.x, current.y);
        Direction turnDir = angleToDirection(turnAngle);

        if (turnDir != Direction::None && turnDir != m_currentDirection) {
            if (m_directions.size() < static_cast<size_t>(m_config.maxDirections)) {
                const double len = calculateDistance(
                    m_segmentStart.x, m_segmentStart.y, m_peakPoint.x, m_peakPoint.y);
                m_directions.push_back(m_currentDirection);
                m_segmentLengths.push_back((std::max)(len, 1.0));
            }
            m_segmentStart = m_peakPoint;
            m_currentDirection = turnDir;
            m_peakPoint = current;
            m_dirsDirty = true;
        }
    }
}

std::vector<Direction> GestureRecognizer::simplifyDirections(const std::vector<Direction>& raw) {
    std::vector<DirectionSegment> segs;
    segs.reserve(raw.size());
    for (auto d : raw) segs.push_back({d, 1.0});
    return simplifySegments(std::move(segs));
}

namespace {

std::vector<DirectionSegment> segmentsFrom(const std::vector<Direction>& dirs,
                                           const std::vector<double>& lengths,
                                           Direction currentDir,
                                           double currentLen) {
    std::vector<DirectionSegment> segs;
    segs.reserve(dirs.size() + 1);
    for (size_t i = 0; i < dirs.size(); ++i) {
        segs.push_back({dirs[i], i < lengths.size() ? lengths[i] : 1.0});
    }
    if (currentDir != Direction::None && (segs.empty() || segs.back().dir != currentDir)) {
        segs.push_back({currentDir, (std::max)(currentLen, 1.0)});
    }
    return segs;
}

}  // namespace

std::optional<GestureResult> GestureRecognizer::finalize() {
    if (isScribbleCanceled()) {
        LOG_INFO("手势识别: 检测到乱晃/原地反悔操作，已自动取消手势执行");
        return std::nullopt;
    }

    const double currentLen = calculateDistance(
        m_segmentStart.x, m_segmentStart.y, m_peakPoint.x, m_peakPoint.y);
    auto simplified = simplifySegments(
        segmentsFrom(m_directions, m_segmentLengths, m_currentDirection, currentLen));

    if (simplified.empty()) {
        LOG_TRACE("手势识别: 轨迹太短或无有效方向段, 点数={}", m_points.size());
        return std::nullopt;
    }

    GestureResult result;
    result.rawPoints = m_points;
    for (size_t i = 1; i < m_points.size(); ++i) {
        result.totalDistance += calculateDistance(
            m_points[i - 1].x, m_points[i - 1].y,
            m_points[i].x, m_points[i].y
        );
    }

    const std::string rawCode = directionsToCode(simplified);
    result.code = refineCodeWithPath(rawCode, m_points, m_config.minSegmentDistance);
    result.directions = (result.code == rawCode) ? std::move(simplified)
                                                 : codeToDirections(result.code);

    LOG_DEBUG("手势识别完成: code={}, arrows={}, 点数={}, 总距离={:.1f}px",
              result.code, result.toArrowString(), m_points.size(), result.totalDistance);

    return result;
}

const std::vector<Direction>& GestureRecognizer::currentDirections() const {
    if (!m_dirsDirty && m_lastPeakPoint.x != -1) {
        const double delta = calculateDistance(m_lastPeakPoint.x, m_lastPeakPoint.y, m_peakPoint.x, m_peakPoint.y);
        if (delta < 4.0) {
            return m_cachedDirections;
        }
    }

    const double currentLen = calculateDistance(
        m_segmentStart.x, m_segmentStart.y, m_peakPoint.x, m_peakPoint.y);
    auto simplified = simplifySegments(
        segmentsFrom(m_directions, m_segmentLengths, m_currentDirection, currentLen));

    // 仅在单段主轴方向且具备轨迹转折时进行细化，避免无谓的字符串双向编解码
    if (simplified.size() == 1 && m_points.size() >= 3) {
        const std::string rawCode = directionToCode(simplified[0]);
        const std::string refined = refineCodeWithPath(rawCode, m_points, m_config.minSegmentDistance);
        if (refined != rawCode) {
            simplified = codeToDirections(refined);
        }
    }

    const DirectionCodeBits newPacked = packDirections(simplified);
    if (newPacked != m_cachedPackedDirections || m_dirsDirty) {
        m_cachedPackedDirections = newPacked;
        m_cachedDirections = std::move(simplified);
    }
    m_lastPeakPoint = m_peakPoint;
    m_dirsDirty = false;
    return m_cachedDirections;
}

DirectionCodeBits GestureRecognizer::currentPackedDirections() const noexcept {
    currentDirections();
    return m_cachedPackedDirections;
}

bool GestureRecognizer::isScribbleCanceled() const {
    if (!m_config.enableScribbleCancel || m_points.size() < 6) return false;

    // 1. 计算总移动路程与包围盒大小
    double totalDist = 0.0;
    int minX = m_points[0].x, maxX = m_points[0].x;
    int minY = m_points[0].y, maxY = m_points[0].y;

    for (size_t i = 1; i < m_points.size(); ++i) {
        totalDist += calculateDistance(m_points[i - 1].x, m_points[i - 1].y, m_points[i].x, m_points[i].y);
        minX = std::min(minX, m_points[i].x);
        maxX = std::max(maxX, m_points[i].x);
        minY = std::min(minY, m_points[i].y);
        maxY = std::max(maxY, m_points[i].y);
    }

    double bboxDiag = calculateDistance(minX, minY, maxX, maxY);

    // 2. 检测方向频繁反转次数（乱晃：左<->右 或 上<->下 往复振荡）
    int reversals = 0;
    for (size_t i = 2; i < m_directions.size(); ++i) {
        Direction prev = m_directions[i - 2];
        Direction curr = m_directions[i - 1];
        Direction next = m_directions[i];
        if (prev == next && prev != curr) {
            reversals++;
        }
    }

    // 乱晃反悔判定准则：
    // 条件 A: 往复反转 >= 3 次 (如左-右-左-右)
    // 条件 B: 轨迹总距离长 (>100px) 但局限在极小包围盒内 (<50px) 且有反转 >= 2 次 (原地乱涂/打圈)
    if (reversals >= 3) return true;
    if (totalDist > 100.0 && bboxDiag < 50.0 && reversals >= 2) return true;

    return false;
}

double GestureRecognizer::calculateAngle(int x1, int y1, int x2, int y2) {
    // atan2 返回 [-π, π]，以正右方为 0，逆时针为正
    // 注意: 屏幕坐标 Y 轴向下，所以 y 取反
    return std::atan2(-(y2 - y1), x2 - x1);
}

Direction GestureRecognizer::angleToDirection(double angleRad) const {
    // 将角度归一化到 [0, 2π)
    if (angleRad < 0) angleRad += 2 * std::numbers::pi;

    double angleDeg = angleRad * 180.0 / std::numbers::pi;

    // 人画水平/垂直线时比画对角线更容易产生小角度偏差。给四个基准方向
    // 约 ±30° 的“磁吸区”，剩余区域才归为对角线；45° 对角手势仍完整保留。
    const double cardinalTolerance =
        (std::clamp)(m_config.angleToleranceDeg + 7.5, 22.5, 35.0);
    if (circularDeltaDeg(angleDeg, 0.0)   <= cardinalTolerance) return Direction::Right;
    if (circularDeltaDeg(angleDeg, 90.0)  <= cardinalTolerance) return Direction::Up;
    if (circularDeltaDeg(angleDeg, 180.0) <= cardinalTolerance) return Direction::Left;
    if (circularDeltaDeg(angleDeg, 270.0) <= cardinalTolerance) return Direction::Down;

    if (angleDeg < 90.0)  return Direction::UpRight;
    if (angleDeg < 180.0) return Direction::UpLeft;
    if (angleDeg < 270.0) return Direction::DownLeft;
    return Direction::DownRight;
}

double GestureRecognizer::calculateDistance(int x1, int y1, int x2, int y2) {
    double dx = static_cast<double>(x2 - x1);
    double dy = static_cast<double>(y2 - y1);
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace tools3000::gesture
