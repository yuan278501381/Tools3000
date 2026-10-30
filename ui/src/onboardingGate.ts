/* ─────────────────────────────────────────────────────────────────────────────
 * onboardingGate.ts — 首次引导判定单一事实源决策门禁
 * ───────────────────────────────────────────────────────────────────────────── */

export interface OnboardingDecisionParams {
  completed?: boolean | null;
  explicitShow?: boolean | null;
}

/**
 * 依据单一事实源裁决是否应当展示新手引导向导：
 * 1. 显式开启优先：用户在设置中显式手动开启（explicitShow === true）-> true
 * 2. 已完成则跳过：已完成首次向导（completed === true）-> false
 * 3. 首次运行（未完成）：全新安装等其他情况 -> true
 */
export function shouldShowOnboarding({
  completed,
  explicitShow,
}: OnboardingDecisionParams): boolean {
  if (explicitShow === true) {
    return true;
  }
  if (completed === true) {
    return false;
  }
  return true;
}
