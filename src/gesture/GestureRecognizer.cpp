// ─────────────────────────────────────────────────────────────────────────────
// GestureRecognizer.cpp — 手势识别算法实现
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/GestureRecognizer.h"
#include "core/logger/Logger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>

namespace tools3000::gesture {

namespace {

inline bool isDiagonalDirection(Direction d) noexcept {
    return d == Direction::UpLeft || d == Direction::UpRight ||
           d == Direction::DownLeft || d == Direction::DownRight;
}

inline Direction oppositeCardinal(Direction d) noexcept {
    switch (d) {
        case Direction::Up:    return Direction::Down;
        case Direction::Down:  return Direction::Up;
        case Direction::Left:  return Direction::Right;
        case Direction::Right: return Direction::Left;
        default:               return Direction::None;
    }
}

inline Direction sharedCardinalBetweenDiagonals(Direction d1, Direction d2) noexcept {
    if ((d1 == Direction::DownRight && d2 == Direction::DownLeft) ||
        (d1 == Direction::DownLeft  && d2 == Direction::DownRight)) {
        return Direction::Down;
    }
    if ((d1 == Direction::UpRight && d2 == Direction::UpLeft) ||
        (d1 == Direction::UpLeft  && d2 == Direction::UpRight)) {
        return Direction::Up;
    }
    if ((d1 == Direction::DownRight && d2 == Direction::UpRight) ||
        (d1 == Direction::UpRight   && d2 == Direction::DownRight)) {
        return Direction::Right;
    }
    if ((d1 == Direction::DownLeft && d2 == Direction::UpLeft) ||
        (d1 == Direction::UpLeft   && d2 == Direction::DownLeft)) {
        return Direction::Left;
    }
    return Direction::None;
}

inline bool isDiagonalCornerFillet(Direction a, Direction b, Direction c) noexcept {
    if (!isDiagonalDirection(a) || !isDiagonalDirection(c) || a == c) return false;
    const Direction shared = sharedCardinalBetweenDiagonals(a, c);
    return shared != Direction::None && b == shared;
}

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
    // 2. 45度微小对角抖动 (正交 -> 邻近对角 -> 正交)
    if ((a == Direction::Down && (b == Direction::DownRight || b == Direction::DownLeft)) ||
        (a == Direction::Up   && (b == Direction::UpRight   || b == Direction::UpLeft))   ||
        (a == Direction::Left && (b == Direction::UpLeft    || b == Direction::DownLeft)) ||
        (a == Direction::Right&& (b == Direction::UpRight   || b == Direction::DownRight))) {
        return true;
    }
    // 3. 45度对角邻近正交抖动 (对角 -> 邻近正交 -> 对角，如 DR-D-DR, UR-R-UR；严防误伤 DR-UR-DR)
    if ((a == Direction::DownRight && (b == Direction::Down || b == Direction::Right)) ||
        (a == Direction::DownLeft  && (b == Direction::Down || b == Direction::Left))  ||
        (a == Direction::UpRight   && (b == Direction::Up   || b == Direction::Right)) ||
        (a == Direction::UpLeft    && (b == Direction::Up   || b == Direction::Left))) {
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
    double dx = 0.0;
    double dy = 0.0;
};

constexpr double kSharpDiagonalPairMinTiltDeg = 6.0;
constexpr double kArcArmDiagonalMergeMinTiltDeg = 12.0;
constexpr double kArcArmShortBendMergeMinTiltDeg = 8.0;
constexpr double kArcArmShortBendMaxRatio = 0.45;
constexpr double kNarrowChevronEachMinTiltDeg = 15.0;
constexpr double kNarrowChevronTotalSpreadDeg = 36.0;
constexpr int kMaxTurnSegmentDistancePx = 16;

inline Direction sharpDiagonalPartner(Direction cardinal, Direction diagonal) noexcept {
    // 仅针对夹角严格等于 135° 的「正交 + 跨象限对角」尖角对（如 V: D+UR -> DR+UR）
    switch (cardinal) {
        case Direction::Down:
            if (diagonal == Direction::UpRight) return Direction::DownRight;
            if (diagonal == Direction::UpLeft)  return Direction::DownLeft;
            return Direction::None;
        case Direction::Up:
            if (diagonal == Direction::DownRight) return Direction::UpRight;
            if (diagonal == Direction::DownLeft)  return Direction::UpLeft;
            return Direction::None;
        case Direction::Right:
            if (diagonal == Direction::DownLeft) return Direction::DownRight;
            if (diagonal == Direction::UpLeft)   return Direction::UpRight;
            return Direction::None;
        case Direction::Left:
            if (diagonal == Direction::DownRight) return Direction::DownLeft;
            if (diagonal == Direction::UpRight)   return Direction::UpLeft;
            return Direction::None;
        default:
            return Direction::None;
    }
}

inline double tiltTowardDiagonalDeg(const DirectionSegment& seg,
                                    Direction cardinal,
                                    Direction targetDiag) noexcept {
    if (std::hypot(seg.dx, seg.dy) < 1.0) return 0.0;
    double primary = 0.0;
    double side = 0.0;
    switch (cardinal) {
        case Direction::Down:
            primary = seg.dy;
            side = (targetDiag == Direction::DownRight) ? seg.dx
                 : (targetDiag == Direction::DownLeft ? -seg.dx : 0.0);
            break;
        case Direction::Up:
            primary = -seg.dy;
            side = (targetDiag == Direction::UpRight) ? seg.dx
                 : (targetDiag == Direction::UpLeft ? -seg.dx : 0.0);
            break;
        case Direction::Right:
            primary = seg.dx;
            side = (targetDiag == Direction::DownRight) ? seg.dy
                 : (targetDiag == Direction::UpRight ? -seg.dy : 0.0);
            break;
        case Direction::Left:
            primary = -seg.dx;
            side = (targetDiag == Direction::DownLeft) ? seg.dy
                 : (targetDiag == Direction::UpLeft ? -seg.dy : 0.0);
            break;
        default:
            return 0.0;
    }
    if (primary <= 1e-6 || side <= 0.0) return 0.0;
    return std::atan2(side, primary) * 180.0 / kPi;
}

void compressSegments(std::vector<DirectionSegment>& segs) {
    std::vector<DirectionSegment> out;
    for (auto& s : segs) {
        if (s.dir == Direction::None || s.length <= 0.0) continue;
        if (!out.empty() && out.back().dir == s.dir) {
            out.back().length += s.length;
            out.back().dx += s.dx;
            out.back().dy += s.dy;
        } else {
            out.push_back(s);
        }
    }
    segs = std::move(out);
}

void resolveSharpDiagonalSegments(std::vector<DirectionSegment>& current) {
    if (current.size() < 2) return;

    // 1. 正交 + 135° 反向跨象限对角（如手写 V 左笔稍陡初判为 D-UR，或右笔稍陡初判为 DR-U）：
    //    若正交段自身已向对应尖角侧倾斜 >= 6°，直接还原为双对角尖角（DR-UR 等），支持 V/∧/</>/W/M。
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i + 1 < current.size(); ++i) {
            auto& a = current[i];
            auto& b = current[i + 1];
            if (!isDiagonalDirection(a.dir) && isDiagonalDirection(b.dir)) {
                const Direction partner = sharpDiagonalPartner(a.dir, b.dir);
                if (partner != Direction::None &&
                    tiltTowardDiagonalDeg(a, a.dir, partner) >= kSharpDiagonalPairMinTiltDeg) {
                    a.dir = partner;
                    changed = true;
                }
            } else if (isDiagonalDirection(a.dir) && !isDiagonalDirection(b.dir)) {
                const Direction partner = sharpDiagonalPartner(b.dir, a.dir);
                if (partner != Direction::None &&
                    tiltTowardDiagonalDeg(b, b.dir, partner) >= kSharpDiagonalPairMinTiltDeg) {
                    b.dir = partner;
                    changed = true;
                }
            }
        }
    }

    // 2. 两段 180° 反向正交且向同一侧大幅张开的对称窄尖角（如窄印刷体 V：两边各偏 >= 15°、总张角 >= 36°）：
    //    往返直线（如 U-D 新建标签页）通常近平行或异侧偏折，不会满足同侧双臂大幅张开条件。
    if (current.size() == 2 && oppositeCardinal(current[0].dir) == current[1].dir) {
        auto tryPromoteNarrowChevron = [&](Direction diag0, Direction diag1) -> bool {
            const double t0 = tiltTowardDiagonalDeg(current[0], current[0].dir, diag0);
            const double t1 = tiltTowardDiagonalDeg(current[1], current[1].dir, diag1);
            if (t0 >= kNarrowChevronEachMinTiltDeg &&
                t1 >= kNarrowChevronEachMinTiltDeg &&
                (t0 + t1) >= kNarrowChevronTotalSpreadDeg) {
                current[0].dir = diag0;
                current[1].dir = diag1;
                return true;
            }
            return false;
        };

        const Direction d0 = current[0].dir;
        if (d0 == Direction::Down) {
            if (!tryPromoteNarrowChevron(Direction::DownRight, Direction::UpRight)) {
                tryPromoteNarrowChevron(Direction::DownLeft, Direction::UpLeft);
            }
        } else if (d0 == Direction::Up) {
            if (!tryPromoteNarrowChevron(Direction::UpRight, Direction::DownRight)) {
                tryPromoteNarrowChevron(Direction::UpLeft, Direction::DownLeft);
            }
        } else if (d0 == Direction::Right) {
            if (!tryPromoteNarrowChevron(Direction::DownRight, Direction::DownLeft)) {
                tryPromoteNarrowChevron(Direction::UpRight, Direction::UpLeft);
            }
        } else if (d0 == Direction::Left) {
            if (!tryPromoteNarrowChevron(Direction::DownLeft, Direction::DownRight)) {
                tryPromoteNarrowChevron(Direction::UpLeft, Direction::UpRight);
            }
        }
    }
}

std::vector<Direction> dirsFromSegments(const std::vector<DirectionSegment>& segs) {
    std::vector<Direction> out;
    out.reserve(segs.size());
    for (const auto& s : segs) out.push_back(s.dir);
    return out;
}

std::vector<DirectionSegment> simplifySegmentsDetailed(std::vector<DirectionSegment> current) {
    compressSegments(current);

    bool modified = true;
    while (modified && current.size() >= 3) {
        modified = false;
        std::vector<DirectionSegment> next;
        for (size_t i = 0; i < current.size(); ) {
            // 1. 四段平滑半圆弧还原：R-DR-DL-L -> R-D-L（覆盖全部 8 种对称圆弧）
            if (i + 3 < current.size()) {
                const Direction a = current[i].dir;
                const Direction d1 = current[i + 1].dir;
                const Direction d2 = current[i + 2].dir;
                const Direction c = current[i + 3].dir;
                const Direction shared = sharedCardinalBetweenDiagonals(d1, d2);
                if (shared != Direction::None &&
                    isCornerFillet(a, d1, shared) &&
                    isCornerFillet(shared, d2, c)) {
                    next.push_back(current[i]);
                    next.push_back({shared,
                                    current[i + 1].length + current[i + 2].length,
                                    current[i + 1].dx + current[i + 2].dx,
                                    current[i + 1].dy + current[i + 2].dy});
                    next.push_back(current[i + 3]);
                    i += 4;
                    modified = true;
                    continue;
                }
            }
            // 1b. 180° 急折发卡弯吸收（专治 L-R-D / R-L-D 等折返接正交第三笔的微弯毛刺）：
            //     人手做 L -> R 180° 折返时难以走出 0px 尖角，常在折点产生极短的 UR/DR/U/D 过渡段，
            //     形成 4 段序列 a -> b(极短) -> opp(a) -> d(垂直于 opp(a))。
            //     仅在明确存在第 4 段正交笔 d 且 b 明显为微小发卡弯时吸收 b，对 3 段 L-U-R / R-D-L 零影响。
            if (i + 3 < current.size()) {
                const Direction a = current[i].dir;
                const Direction b = current[i + 1].dir;
                const Direction c = current[i + 2].dir;
                const Direction d = current[i + 3].dir;
                const Direction oppA = oppositeCardinal(a);
                if (oppA != Direction::None && c == oppA && d != a && d != c) {
                    const bool bIsMinorBend =
                        (isDiagonalDirection(b) &&
                         (completeIncompleteFillet(a, b).has_value() ||
                          completeIncompleteFillet(c, b).has_value())) ||
                        (!isDiagonalDirection(b) && b != a && b != c);
                    const double armMin = (std::min)(current[i].length, current[i + 2].length);
                    const double dLen = current[i + 3].length;
                    const bool dIsSubstantialOrtho =
                        (!isDiagonalDirection(d)) ||
                        (completeIncompleteFillet(c, d).has_value() && dLen >= 22.0);
                    if (bIsMinorBend && dIsSubstantialOrtho &&
                        current[i + 1].length <= 36.0 &&
                        current[i + 1].length < armMin * 0.42 &&
                        current[i + 1].length < dLen * 0.50) {
                        next.push_back(current[i]);
                        next.push_back(current[i + 2]);
                        next.push_back(current[i + 3]);
                        i += 4;
                        modified = true;
                        continue;
                    }
                }
            }
            if (i + 2 < current.size()) {
                const Direction a = current[i].dir;
                const Direction b = current[i + 1].dir;
                const Direction c = current[i + 2].dir;
                if (isJitterRebound(a, b, c)) {
                    next.push_back({a,
                                    current[i].length + current[i + 1].length + current[i + 2].length,
                                    current[i].dx + current[i + 1].dx + current[i + 2].dx,
                                    current[i].dy + current[i + 1].dy + current[i + 2].dy});
                    i += 3;
                    modified = true;
                    continue;
                }
                if (isCornerFillet(a, b, c) || isDiagonalCornerFillet(a, b, c)) {
                    next.push_back(current[i]);
                    next.push_back(current[i + 2]);
                    i += 3;
                    modified = true;
                    continue;
                }
                // 回中点锐角对角过渡吸收（实机 T 形 L-R-D 典型轨迹 L-R-DL-D）：
                // 例如 R -> DL -> D：DL 由 opp(R)=L 与 D 合成，属于横杠画完向左下折回中点再竖下的自然过渡，
                // 吸收 DL 并入 D 化简为 R -> D（绝不影响第三段仍为 R 的 Z 形 R-DL-R）。
                {
                    const Direction oppA = oppositeCardinal(a);
                    if (oppA != Direction::None &&
                        !isDiagonalDirection(c) && c != a && c != oppA &&
                        isDiagonalDirection(b) &&
                        isCornerFillet(oppA, b, c)) {
                        next.push_back(current[i]);
                        next.push_back({c,
                                        current[i + 1].length + current[i + 2].length,
                                        current[i + 1].dx + current[i + 2].dx,
                                        current[i + 1].dy + current[i + 2].dy});
                        i += 3;
                        modified = true;
                        continue;
                    }
                }
                // 反向夹心单对角圆弧：如 R-DR-L 或 R-DL-L -> R-D-L
                const Direction oppA = oppositeCardinal(a);
                if (oppA != Direction::None && c == oppA) {
                    if (auto mid = completeIncompleteFillet(a, b)) {
                        next.push_back(current[i]);
                        next.push_back({*mid, current[i + 1].length, current[i + 1].dx, current[i + 1].dy});
                        next.push_back(current[i + 2]);
                        i += 3;
                        modified = true;
                        continue;
                    }
                }
                // 三段未拉平圆弧双对角：如 R-DR-DL -> R-D-DL（随后由收尾回折规则补全为 R-D-L）；
                // 但若首段正交已经明显朝 b 对角倾斜（如弧臂 V: D(18°)->DR->UR），则 a 与 b 实为同一斜臂，合并为 b。
                const Direction sharedTail = sharedCardinalBetweenDiagonals(b, c);
                if (sharedTail != Direction::None && isCornerFillet(a, b, sharedTail)) {
                    const double tiltA = tiltTowardDiagonalDeg(current[i], a, b);
                    const bool isArcArmBend =
                        tiltA >= kArcArmDiagonalMergeMinTiltDeg ||
                        (tiltA >= kArcArmShortBendMergeMinTiltDeg &&
                         current[i + 1].length < current[i + 2].length * kArcArmShortBendMaxRatio);
                    if (isArcArmBend) {
                        next.push_back({b,
                                        current[i].length + current[i + 1].length,
                                        current[i].dx + current[i + 1].dx,
                                        current[i].dy + current[i + 1].dy});
                        next.push_back(current[i + 2]);
                    } else {
                        next.push_back(current[i]);
                        next.push_back({sharedTail, current[i + 1].length, current[i + 1].dx, current[i + 1].dy});
                        next.push_back(current[i + 2]);
                    }
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
        } else if (auto completed = completeIncompleteFillet(
                       current[current.size() - 2].dir, current.back().dir)) {
            const Direction prevPrev = current[current.size() - 3].dir;
            const Direction oppPrevPrev = oppositeCardinal(prevPrev);
            if (oppPrevPrev != Direction::None && *completed == oppPrevPrev) {
                // 反向回折第三笔（如反 C: R-D-DL -> R-D-L、U 形: D-R-UR -> D-R-U），
                // 方向与首段相反绝非惯性下垂，补全为标准正交收笔段。
                current.back().dir = *completed;
                filletModified = true;
            } else if (*completed == prevPrev) {
                // 同象限收笔惯性微垂（如 D-R-DR -> D-R），必须丢弃以防误触 D-R-D 截图。
                current.pop_back();
                filletModified = true;
            } else if (oppPrevPrev != Direction::None &&
                       current[current.size() - 2].dir == oppPrevPrev &&
                       current.back().length >= 22.0 &&
                       current.back().length >= current[current.size() - 2].length * 0.25) {
                // 180° 折返后拐出的明确第三笔（如 L-R-DR -> L-R-D），放宽末笔比例门槛，
                // 防止用户画 T 形 L-R-D 手快偏斜时被截断降级为 L-R 误触 Ctrl+Z 撤销。
                current.back().dir = *completed;
                filletModified = true;
            } else if (current.back().length >= 24.0 &&
                       current.back().length >= current[current.size() - 2].length * 0.45) {
                current.back().dir = *completed;
                filletModified = true;
            } else {
                current.pop_back();
                filletModified = true;
            }
        }
        if (current.size() >= 2) {
            if (auto invented = completeIncompleteFillet(current[1].dir, current[0].dir)) {
                const bool isReversingThreeSeg =
                    current.size() >= 3 &&
                    oppositeCardinal(current[2].dir) != Direction::None &&
                    *invented == oppositeCardinal(current[2].dir);
                const double dropRatio = isReversingThreeSeg ? 0.22 : 0.40;
                if (current[0].length < current[1].length * dropRatio) {
                    current.erase(current.begin());
                } else {
                    current[0].dir = *invented;
                }
                filletModified = true;
            } else if (current.size() == 3 &&
                       isDiagonalDirection(current[0].dir) &&
                       !isDiagonalDirection(current[1].dir) &&
                       !isDiagonalDirection(current[2].dir)) {
                // 首段微斜折返归一（实机轨迹 DL-R-D -> L-R-D）：
                // 首笔向左略偏下进入 DL，紧接 135° 反向水平 R 与正交垂直 D 时，将首笔还原为 opp(R)=L。
                const Direction oppSecond = oppositeCardinal(current[1].dir);
                if (oppSecond != Direction::None &&
                    isCornerFillet(oppSecond, current[0].dir, current[2].dir)) {
                    current[0].dir = oppSecond;
                    filletModified = true;
                }
            }
        }
        compressSegments(current);
    }

    resolveSharpDiagonalSegments(current);
    compressSegments(current);

    return current;
}

std::vector<Direction> simplifySegments(std::vector<DirectionSegment> current) {
    return dirsFromSegments(simplifySegmentsDetailed(std::move(current)));
}

// ── 连续几何形状相似度匹配器（方向敏感，支持折线 + 平滑圆弧双原型） ────────────

constexpr size_t kShapeSampleCount = 32;

struct ShapePt {
    double x = 0.0;
    double y = 0.0;
};

double shapePtDist(const ShapePt& a, const ShapePt& b) noexcept {
    return std::hypot(b.x - a.x, b.y - a.y);
}

double polylineLength(const std::vector<ShapePt>& pts) noexcept {
    double total = 0.0;
    for (size_t i = 1; i < pts.size(); ++i) {
        total += shapePtDist(pts[i - 1], pts[i]);
    }
    return total;
}

bool resampleAndNormalize(const std::vector<ShapePt>& raw,
                          std::array<ShapePt, kShapeSampleCount>& out) noexcept {
    if (raw.size() < 2) return false;
    const double totalLen = polylineLength(raw);
    if (totalLen < 1e-4) return false;

    const double step = totalLen / static_cast<double>(kShapeSampleCount - 1);
    out[0] = raw.front();
    size_t outIdx = 1;
    double accum = 0.0;
    ShapePt prev = raw[0];

    for (size_t i = 1; i < raw.size() && outIdx < kShapeSampleCount; ++i) {
        ShapePt curr = raw[i];
        double segLen = shapePtDist(prev, curr);
        while (segLen > 1e-9 && accum + segLen >= step && outIdx < kShapeSampleCount) {
            const double remain = step - accum;
            const double t = remain / segLen;
            ShapePt interp{
                prev.x + (curr.x - prev.x) * t,
                prev.y + (curr.y - prev.y) * t
            };
            out[outIdx++] = interp;
            prev = interp;
            segLen = shapePtDist(prev, curr);
            accum = 0.0;
        }
        accum += segLen;
        prev = curr;
    }
    while (outIdx < kShapeSampleCount) {
        out[outIdx++] = raw.back();
    }

    double cx = 0.0;
    double cy = 0.0;
    for (const auto& p : out) {
        cx += p.x;
        cy += p.y;
    }
    cx /= static_cast<double>(kShapeSampleCount);
    cy /= static_cast<double>(kShapeSampleCount);

    double sqSum = 0.0;
    for (auto& p : out) {
        p.x -= cx;
        p.y -= cy;
        sqSum += p.x * p.x + p.y * p.y;
    }
    const double rms = std::sqrt(sqSum / static_cast<double>(kShapeSampleCount));
    if (rms < 1e-4) return false;

    for (auto& p : out) {
        p.x /= rms;
        p.y /= rms;
    }
    return true;
}

ShapePt directionUnitVectorScreen(Direction d) noexcept {
    constexpr double kInvSqrt2 = 0.7071067811865475244;
    switch (d) {
        case Direction::Right:     return { 1.0,  0.0};
        case Direction::Left:      return {-1.0,  0.0};
        case Direction::Up:        return { 0.0, -1.0};
        case Direction::Down:      return { 0.0,  1.0};
        case Direction::UpRight:   return { kInvSqrt2, -kInvSqrt2};
        case Direction::UpLeft:    return {-kInvSqrt2, -kInvSqrt2};
        case Direction::DownRight: return { kInvSqrt2,  kInvSqrt2};
        case Direction::DownLeft:  return {-kInvSqrt2,  kInvSqrt2};
        default:                   return { 0.0,  0.0};
    }
}

std::vector<ShapePt> chaikinSmooth(std::vector<ShapePt> pts, int iterations) {
    for (int iter = 0; iter < iterations && pts.size() >= 3; ++iter) {
        std::vector<ShapePt> next;
        next.reserve(pts.size() * 2);
        next.push_back(pts.front());
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const auto& p0 = pts[i];
            const auto& p1 = pts[i + 1];
            next.push_back({0.75 * p0.x + 0.25 * p1.x, 0.75 * p0.y + 0.25 * p1.y});
            next.push_back({0.25 * p0.x + 0.75 * p1.x, 0.25 * p0.y + 0.75 * p1.y});
        }
        next.push_back(pts.back());
        pts = std::move(next);
    }
    return pts;
}

double scoreNormalizedShapes(const std::array<ShapePt, kShapeSampleCount>& u,
                             const std::array<ShapePt, kShapeSampleCount>& t) noexcept {
    double distSum = 0.0;
    for (size_t i = 0; i < kShapeSampleCount; ++i) {
        distSum += shapePtDist(u[i], t[i]);
    }
    const double meanDist = distSum / static_cast<double>(kShapeSampleCount);
    const double posScore = (std::clamp)(1.0 - meanDist / 1.45, 0.0, 1.0);

    double cosSum = 0.0;
    int validTangents = 0;
    for (size_t i = 0; i + 1 < kShapeSampleCount; ++i) {
        const double ux = u[i + 1].x - u[i].x;
        const double uy = u[i + 1].y - u[i].y;
        const double tx = t[i + 1].x - t[i].x;
        const double ty = t[i + 1].y - t[i].y;
        const double uLen = std::hypot(ux, uy);
        const double tLen = std::hypot(tx, ty);
        if (uLen > 1e-6 && tLen > 1e-6) {
            cosSum += (ux * tx + uy * ty) / (uLen * tLen);
            ++validTangents;
        }
    }
    const double avgCos = validTangents > 0 ? (cosSum / static_cast<double>(validTangents)) : 0.0;
    const double dirScore = (std::clamp)(avgCos, 0.0, 1.0);

    // 端头方向一致性（前 1/6 起笔矢量 + 后 1/6 收笔矢量）：防止 180° 反 C 圆弧误匹配 90° L 形 R-D
    auto endpointCos = [&](size_t a, size_t b) noexcept -> double {
        const double ux = u[b].x - u[a].x;
        const double uy = u[b].y - u[a].y;
        const double tx = t[b].x - t[a].x;
        const double ty = t[b].y - t[a].y;
        const double uLen = std::hypot(ux, uy);
        const double tLen = std::hypot(tx, ty);
        if (uLen <= 1e-6 || tLen <= 1e-6) return 0.0;
        return (std::clamp)((ux * tx + uy * ty) / (uLen * tLen), 0.0, 1.0);
    };
    const size_t span = 3;
    const double startCos = endpointCos(0, span);
    const double endCos = endpointCos(kShapeSampleCount - 1 - span, kShapeSampleCount - 1);
    const double endpointScore = 0.5 * (startCos + endCos);

    return (0.44 * posScore + 0.36 * dirScore + 0.20 * endpointScore) * 100.0;
}

int computeShapeSimilarityFromNormalized(const std::array<ShapePt, kShapeSampleCount>& normUser,
                                         double userStraightness,
                                         const std::string& bareDirectionCode) noexcept {
    const auto dirs = codeToDirections(bareDirectionCode);
    if (dirs.empty()) return 0;

    auto evalPrototype = [&](const std::vector<double>& weights, bool allowArcSmooth) noexcept -> double {
        std::vector<ShapePt> poly;
        poly.reserve(dirs.size() + 1);
        poly.push_back({0.0, 0.0});
        for (size_t i = 0; i < dirs.size(); ++i) {
            const double w = (i < weights.size()) ? weights[i] : 1.0;
            const ShapePt v = directionUnitVectorScreen(dirs[i]);
            poly.push_back({poly.back().x + v.x * w, poly.back().y + v.y * w});
        }
        const double templateLen = polylineLength(poly);
        const double templateStraightness =
            templateLen > 1e-4 ? (shapePtDist(poly.front(), poly.back()) / templateLen) : 1.0;

        std::array<ShapePt, kShapeSampleCount> normPoly{};
        if (!resampleAndNormalize(poly, normPoly)) return 0.0;
        double score = scoreNormalizedShapes(normUser, normPoly);

        if (allowArcSmooth && dirs.size() >= 2) {
            std::array<ShapePt, kShapeSampleCount> normArc{};
            if (resampleAndNormalize(chaikinSmooth(poly, 3), normArc)) {
                score = (std::max)(score, scoreNormalizedShapes(normUser, normArc));
            }
        }

        // 直线/折弯形态防混淆护栏：禁止弯折弧线误配单段直线，亦禁止笔直单线误配明显拐弯的多段模板
        if (dirs.size() == 1 && userStraightness < 0.72) {
            score *= 0.72;
        } else if (dirs.size() >= 2 && templateStraightness < 0.88 && userStraightness > 0.95) {
            score *= 0.72;
        }
        return score;
    };

    std::vector<double> equalWeights(dirs.size(), 1.0);
    double bestScore = evalPrototype(equalWeights, true);

    // 针对 2 段折线手势（如高速连划的「长竖+短横 D-R」），仅评估「长起笔+短拐角」折线原型
    if (dirs.size() == 2) {
        bestScore = (std::max)(bestScore, evalPrototype({1.0, 0.45}, false));
    }
    // 针对 3 段 180° 折返象形手势（如 T 形 L-R-D / R-L-D），追加「左画横 -> 回中点 -> 向下竖」半回折原型
    if (dirs.size() == 3 &&
        oppositeCardinal(dirs[0]) != Direction::None &&
        dirs[1] == oppositeCardinal(dirs[0]) &&
        dirs[2] != dirs[0] && dirs[2] != dirs[1]) {
        bestScore = (std::max)(bestScore, evalPrototype({1.0, 0.5, 1.0}, false));
    }

    return static_cast<int>((std::clamp)(std::lround(bestScore), 0L, 100L));
}

} // namespace

int computeGestureShapeSimilarity(const std::vector<TrackPoint>& rawPoints,
                                  const std::string& bareDirectionCode) noexcept {
    if (rawPoints.size() < 2 || bareDirectionCode.empty()) return 0;
    std::vector<ShapePt> pts;
    pts.reserve(rawPoints.size());
    for (const auto& p : rawPoints) {
        pts.push_back({static_cast<double>(p.x), static_cast<double>(p.y)});
    }
    const double totalLen = polylineLength(pts);
    if (totalLen < 12.0) return 0;

    std::array<ShapePt, kShapeSampleCount> normUser{};
    if (!resampleAndNormalize(pts, normUser)) return 0;

    const double userStraightness = shapePtDist(pts.front(), pts.back()) / totalLen;
    return computeShapeSimilarityFromNormalized(normUser, userStraightness, bareDirectionCode);
}

std::optional<ShapeMatchResult> matchBestGestureShape(
    const std::vector<TrackPoint>& rawPoints,
    const std::vector<ShapeMatchCandidate>& candidates,
    int thresholdPercent,
    int minSegmentDistance,
    ShapeMatchDiagnostics* outDiagnostics) noexcept {
    if (outDiagnostics) {
        *outDiagnostics = ShapeMatchDiagnostics{};
    }
    const int threshold = (std::clamp)(thresholdPercent, 50, 100);
    if (threshold >= 100 || rawPoints.size() < 2 || candidates.empty()) {
        return std::nullopt;
    }

    std::vector<ShapePt> pts;
    pts.reserve(rawPoints.size());
    for (const auto& p : rawPoints) {
        pts.push_back({static_cast<double>(p.x), static_cast<double>(p.y)});
    }
    const double totalLen = polylineLength(pts);
    const double minRequiredLen = static_cast<double>((std::max)(minSegmentDistance, 12));
    if (totalLen < minRequiredLen) {
        return std::nullopt;
    }

    std::array<ShapePt, kShapeSampleCount> normUser{};
    if (!resampleAndNormalize(pts, normUser)) {
        return std::nullopt;
    }
    const double userStraightness = shapePtDist(pts.front(), pts.back()) / totalLen;

    int bestScore = -1;
    int secondBestScore = -1;
    std::string bestCode;
    std::string bestBare;
    std::string secondBestCode;

    for (const auto& cand : candidates) {
        if (cand.code.empty() || cand.bareDirection.empty()) continue;
        // 拓扑保护铁律：绝不允许未命中的多段/圆弧轨迹通过形状相似度降维误触单段直线手势 (U/D/L/R)
        const auto candDirs = codeToDirections(cand.bareDirection);
        if (candDirs.size() <= 1) continue;

        const int score = computeShapeSimilarityFromNormalized(normUser, userStraightness, cand.bareDirection);
        if (score > bestScore) {
            if (!bestCode.empty() && cand.bareDirection != bestBare) {
                secondBestScore = bestScore;
                secondBestCode = bestCode;
            }
            bestScore = score;
            bestCode = cand.code;
            bestBare = cand.bareDirection;
        } else if (cand.bareDirection != bestBare && score > secondBestScore) {
            secondBestScore = score;
            secondBestCode = cand.code;
        }
    }

    if (outDiagnostics) {
        outDiagnostics->top1Code = bestCode;
        outDiagnostics->top1Percent = (std::max)(bestScore, 0);
        outDiagnostics->top2Code = secondBestCode;
        outDiagnostics->top2Percent = (std::max)(secondBestScore, 0);
    }

    if (bestCode.empty() || bestScore < threshold) {
        return std::nullopt;
    }
    // 防擦边歧义：两个不同手势同时紧贴阈值且分差 < 2% 时宁缺毋滥，避免误触
    if (secondBestScore >= threshold &&
        (bestScore - secondBestScore) < 2 &&
        bestScore < threshold + 4) {
        if (outDiagnostics) {
            outDiagnostics->rejectedByAmbiguity = true;
        }
        return std::nullopt;
    }

    return ShapeMatchResult{
        std::move(bestCode),
        bestScore,
        std::move(secondBestCode),
        (std::max)(secondBestScore, 0),
    };
}

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
    const int turnSeg = (std::min)(minSeg, kMaxTurnSegmentDistancePx);
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
    // 1. 第一段已经走够；2. 第二段清晰达到拐弯阈值（与起笔防误触距离解耦）；3. 第二段由正交轴主导。
    // 第 3 条是关键：它把持续略向左的「下」与明确转向左/右的「下-左 / 下-右」分开。
    auto considerTurn = [minSeg, turnSeg, &best](int firstPrimary, int firstOrtho,
                                                 int secondPrimary, int secondOrtho) noexcept {
        if (firstPrimary < (std::max)(turnSeg * 2, minSeg)) return;
        const int secondLength = std::abs(secondOrtho);
        const int requiredLength = (std::max)(turnSeg + 4, firstPrimary * 16 / 100);
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
    // 正交方向保留适度自然手抖宽容度；对角方向收紧推进锥，
    // 防止画反 C / 正 C 圆弧或 V 形转角时对角扇区把正交中段或转折臂吃掉。
    const double cone = isDiagonalDirection(dir)
        ? (std::clamp)(m_config.angleToleranceDeg * 1.25, 24.0, 30.0)
        : (std::clamp)(m_config.angleToleranceDeg * 1.55, 30.0, 36.0);
    return circularDeltaDeg(deg, directionCenterDeg(dir)) <= cone;
}

GestureRecognizer::GestureRecognizer(const RecognizerConfig& config)
    : m_config(config) {
    m_points.reserve(kMaxPoints);
    m_directions.reserve(16);
    m_segmentLengths.reserve(16);
    m_segmentDeltas.reserve(16);
    m_cachedDirections.reserve(16);
}

void GestureRecognizer::reset() {
    m_points.clear();
    m_directions.clear();
    m_segmentLengths.clear();
    m_segmentDeltas.clear();
    m_currentDirection = Direction::None;
    m_hasSegmentStart = false;
    m_segmentStart = {0, 0};
    m_peakPoint = {0, 0};
    m_dirsDirty = true;
    m_cachedDirections.clear();
    m_cachedPackedDirections = 0;
    m_lastPeakPoint = {-1, -1};
    m_lastEndPoint = {-1, -1};
    m_lastPointCount = 0;
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
    // 起笔阶段(阶段1)严格守卫 minSegmentDistance 防单击误触，一旦进入已激活轨迹后，
    // 拐弯切段距离解耦封顶为 16px，确保高速连划 D-R 等短拐角手势不漏切第二段。
    const double turnMinDist = static_cast<double>((std::min)(m_config.minSegmentDistance, kMaxTurnSegmentDistancePx));
    if (distFromPeak >= turnMinDist) {
        double turnAngle = calculateAngle(m_peakPoint.x, m_peakPoint.y, current.x, current.y);
        Direction turnDir = angleToDirection(turnAngle);

        if (turnDir != Direction::None && turnDir != m_currentDirection) {
            const size_t maxDirs = static_cast<size_t>((std::max)(m_config.maxDirections, 1));
            if (m_directions.size() + 1 < maxDirs) {
                const double len = calculateDistance(
                    m_segmentStart.x, m_segmentStart.y, m_peakPoint.x, m_peakPoint.y);
                m_directions.push_back(m_currentDirection);
                m_segmentLengths.push_back((std::max)(len, 1.0));
                m_segmentDeltas.push_back({m_peakPoint.x - m_segmentStart.x, m_peakPoint.y - m_segmentStart.y});
                m_segmentStart = m_peakPoint;
                m_currentDirection = turnDir;
                m_peakPoint = current;
                m_dirsDirty = true;
            }
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
                                           const std::vector<TrackPoint>& deltas,
                                           Direction currentDir,
                                           double currentLen,
                                           TrackPoint currentDelta) {
    std::vector<DirectionSegment> segs;
    segs.reserve(dirs.size() + 1);
    for (size_t i = 0; i < dirs.size(); ++i) {
        const TrackPoint delta = (i < deltas.size()) ? deltas[i] : TrackPoint{0, 0};
        segs.push_back({
            dirs[i],
            i < lengths.size() ? lengths[i] : 1.0,
            static_cast<double>(delta.x),
            static_cast<double>(delta.y),
        });
    }
    if (currentDir != Direction::None && (segs.empty() || segs.back().dir != currentDir)) {
        segs.push_back({
            currentDir,
            (std::max)(currentLen, 1.0),
            static_cast<double>(currentDelta.x),
            static_cast<double>(currentDelta.y),
        });
    }
    return segs;
}

std::vector<RecognizedSegmentInfo> toSegmentDiagnostics(const std::vector<DirectionSegment>& segs) {
    std::vector<RecognizedSegmentInfo> out;
    out.reserve(segs.size());
    for (const auto& s : segs) {
        double angleDeg = directionCenterDeg(s.dir);
        if (std::hypot(s.dx, s.dy) >= 1.0) {
            double rad = std::atan2(-s.dy, s.dx);
            if (rad < 0.0) rad += 2.0 * kPi;
            angleDeg = rad * 180.0 / kPi;
        }
        out.push_back(RecognizedSegmentInfo{s.dir, s.length, angleDeg});
    }
    return out;
}

std::string formatSegmentDiagnosticList(const std::vector<RecognizedSegmentInfo>& segs, bool withAngle) {
    std::string out = "[";
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i > 0) out += ",";
        out += directionToCode(segs[i].dir);
        out += ":";
        out += std::to_string(static_cast<int>(std::lround(segs[i].length)));
        out += "px";
        if (withAngle) {
            out += "(";
            out += std::to_string(static_cast<int>(std::lround(segs[i].angleDeg)));
            out += "deg)";
        }
    }
    out += "]";
    return out;
}

}  // namespace

std::string GestureResult::formatDiagnosticSummary() const {
    std::string summary;
    summary.reserve(160);
    summary += "raw=";
    summary += formatSegmentDiagnosticList(rawSegments, true);
    summary += ", simp=";
    summary += formatSegmentDiagnosticList(simplifiedSegments, false);
    summary += ", bbox=";
    summary += std::to_string(bboxWidth);
    summary += "x";
    summary += std::to_string(bboxHeight);
    summary += ", pathRefined=";
    summary += refinedByPath ? "true" : "false";
    return summary;
}

std::optional<GestureResult> GestureRecognizer::finalize() {
    if (isScribbleCanceled()) {
        LOG_INFO("手势识别: 检测到乱晃/原地反悔操作，已自动取消手势执行");
        return std::nullopt;
    }

    const double currentLen = calculateDistance(
        m_segmentStart.x, m_segmentStart.y, m_peakPoint.x, m_peakPoint.y);
    const TrackPoint currentDelta{m_peakPoint.x - m_segmentStart.x, m_peakPoint.y - m_segmentStart.y};
    auto rawSegs = segmentsFrom(
        m_directions, m_segmentLengths, m_segmentDeltas, m_currentDirection, currentLen, currentDelta);
    auto simplifiedSegs = simplifySegmentsDetailed(rawSegs);
    auto simplified = dirsFromSegments(simplifiedSegs);

    if (simplified.empty()) {
        LOG_TRACE("手势识别: 轨迹太短或无有效方向段, 点数={}", m_points.size());
        return std::nullopt;
    }

    GestureResult result;
    result.rawPoints = m_points;
    result.rawSegments = toSegmentDiagnostics(rawSegs);
    result.simplifiedSegments = toSegmentDiagnostics(simplifiedSegs);

    int minX = m_points[0].x, maxX = m_points[0].x;
    int minY = m_points[0].y, maxY = m_points[0].y;
    for (size_t i = 1; i < m_points.size(); ++i) {
        result.totalDistance += calculateDistance(
            m_points[i - 1].x, m_points[i - 1].y,
            m_points[i].x, m_points[i].y
        );
        minX = (std::min)(minX, m_points[i].x);
        maxX = (std::max)(maxX, m_points[i].x);
        minY = (std::min)(minY, m_points[i].y);
        maxY = (std::max)(maxY, m_points[i].y);
    }
    result.bboxWidth = maxX - minX;
    result.bboxHeight = maxY - minY;

    const std::string rawCode = directionsToCode(simplified);
    result.code = refineCodeWithPath(rawCode, m_points, m_config.minSegmentDistance);
    result.refinedByPath = (result.code != rawCode);
    result.directions = !result.refinedByPath ? std::move(simplified)
                                              : codeToDirections(result.code);

    LOG_DEBUG("手势识别完成: code={}, arrows={}, 点数={}, 总距离={:.1f}px",
              result.code, result.toArrowString(), m_points.size(), result.totalDistance);

    return result;
}

const std::vector<Direction>& GestureRecognizer::currentDirections() const {
    const TrackPoint endPt = m_points.empty() ? TrackPoint{-1, -1} : m_points.back();
    if (!m_dirsDirty && m_lastPeakPoint.x != -1) {
        const double peakDelta = calculateDistance(m_lastPeakPoint.x, m_lastPeakPoint.y, m_peakPoint.x, m_peakPoint.y);
        const double endDelta = (m_lastEndPoint.x != -1 && endPt.x != -1)
            ? calculateDistance(m_lastEndPoint.x, m_lastEndPoint.y, endPt.x, endPt.y)
            : 0.0;
        if (peakDelta < 4.0 && endDelta < 4.0 && m_points.size() == m_lastPointCount) {
            return m_cachedDirections;
        }
    }

    const double currentLen = calculateDistance(
        m_segmentStart.x, m_segmentStart.y, m_peakPoint.x, m_peakPoint.y);
    const TrackPoint currentDelta{m_peakPoint.x - m_segmentStart.x, m_peakPoint.y - m_segmentStart.y};
    auto simplified = simplifySegments(
        segmentsFrom(m_directions, m_segmentLengths, m_segmentDeltas, m_currentDirection, currentLen, currentDelta));

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
    m_lastEndPoint = endPt;
    m_lastPointCount = m_points.size();
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

    // 人画水平/垂直线时比画对角线更容易产生小角度偏差：给四个基准方向 ±28° 磁吸区，
    // 同时给 45° 对角（如 V 手势 DR-UR）留出 ±17° 充足扇区，兼顾抗手抖与斜角易画性。
    const double cardinalTolerance =
        (std::clamp)(m_config.angleToleranceDeg + 5.5, 22.5, 32.0);
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
