/* ─────────────────────────────────────────────────────────────────────────────
 * clipboardSelection — 剪贴板历史列表桌面级多选纯函数状态机
 *
 * 设计准则（对标 Windows 资源管理器 / Everything 肌肉记忆 + 合并粘贴保序）:
 *   1. 固定锚点 (anchorIndex)：Shift+点击 与 Shift+↑/↓ 围绕同一锚点精确扩选或回缩；
 *   2. Ctrl/Meta+点击：随时逐条加选或反选并重置锚点，无论是否显式开启「多选」模式；
 *   3. Ctrl+Shift+点击：保留现有全部不连续勾选，并将 [anchor..idx] 区间追加并入；
 *   4. 严格保序：selectedIds 数组维持勾选先后顺序，确保「合并粘贴」按用户选择顺序拼接；
 *   5. 全纯函数零副作用，便于确定性回归测试守护。
 * ───────────────────────────────────────────────────────────────────────────── */

export interface ClipboardSelectionState {
  selectedIds: number[];
  focusIndex: number;
  anchorIndex: number | null;
  shiftBaseIds: number[] | null;
}

export interface ClipboardClickModifiers {
  shiftKey: boolean;
  ctrlKey: boolean;
  metaKey: boolean;
  multiSelectMode: boolean;
}

export function createInitialClipboardSelectionState(): ClipboardSelectionState {
  return {
    selectedIds: [],
    focusIndex: 0,
    anchorIndex: null,
    shiftBaseIds: null,
  };
}

function clampIndex(index: number, length: number): number {
  if (length <= 0) return 0;
  return Math.max(0, Math.min(length - 1, index));
}

/** 按从 anchor 到 target 的选择方向构建保序区间 ID 列表。 */
function buildDirectionalRangeIds(
  itemIds: readonly number[],
  anchorIdx: number,
  targetIdx: number,
): number[] {
  if (itemIds.length === 0) return [];
  const start = clampIndex(anchorIdx, itemIds.length);
  const end = clampIndex(targetIdx, itemIds.length);
  const step = start <= end ? 1 : -1;
  const out: number[] = [];
  for (let i = start; ; i += step) {
    out.push(itemIds[i]);
    if (i === end) break;
  }
  return out;
}

function mergeUniqueIds(base: readonly number[], addition: readonly number[]): number[] {
  const seen = new Set<number>(base);
  const merged = [...base];
  for (const id of addition) {
    if (!seen.has(id)) {
      seen.add(id);
      merged.push(id);
    }
  }
  return merged;
}

/**
 * 计算点击第 idx 条记录后的多选状态。
 * - Shift (+可选 Ctrl)：以固定锚点做范围连选/回缩；
 * - Ctrl/Meta 或已开启 multiSelectMode：切换勾选该项并更新锚点；
 * - 常规普通单点：切回单焦点（若此前有临时 Ctrl/Shift 多选则清空退出），并设新锚点。
 */
export function applyClipboardItemClick(
  state: ClipboardSelectionState,
  itemIds: readonly number[],
  idx: number,
  modifiers: ClipboardClickModifiers,
): ClipboardSelectionState {
  if (itemIds.length === 0 || idx < 0 || idx >= itemIds.length) {
    return state;
  }

  const targetId = itemIds[idx];
  const isAdditiveModifier = modifiers.ctrlKey || modifiers.metaKey;

  if (modifiers.shiftKey) {
    const anchor = clampIndex(
      state.anchorIndex !== null ? state.anchorIndex : state.focusIndex,
      itemIds.length,
    );
    const rangeIds = buildDirectionalRangeIds(itemIds, anchor, idx);

    let nextBase: number[];
    if (isAdditiveModifier) {
      // Ctrl+Shift+点击：在已有选区上叠加新区间，不丢弃此前不连续勾选
      nextBase = [...state.selectedIds];
    } else if (state.shiftBaseIds !== null) {
      // 同一锚点连续 Shift 调整：始终基于首次 Shift 前的基准计算，实现丝滑扩选与回缩
      nextBase = [...state.shiftBaseIds];
    } else if (modifiers.multiSelectMode) {
      // 显式「多选」模式下首次 Shift：保留此前勾选项（去掉锚点本身避免重复顺序错位，再由 rangeIds 按方向补入）
      const anchorId = itemIds[anchor];
      nextBase = state.selectedIds.filter((id) => id !== anchorId);
    } else {
      // 常规模式下首次 Shift：标准资源管理器语义，仅选中 [anchor..idx]
      nextBase = [];
    }

    return {
      selectedIds: mergeUniqueIds(nextBase, rangeIds),
      focusIndex: idx,
      anchorIndex: anchor,
      shiftBaseIds: nextBase,
    };
  }

  if (isAdditiveModifier || modifiers.multiSelectMode) {
    const exists = state.selectedIds.includes(targetId);
    const nextSelected = exists
      ? state.selectedIds.filter((id) => id !== targetId)
      : [...state.selectedIds, targetId];
    return {
      selectedIds: nextSelected,
      focusIndex: idx,
      anchorIndex: idx,
      shiftBaseIds: null,
    };
  }

  // 非多选模式普通左键单点：切换焦点、清空临时多选并重置锚点
  return {
    selectedIds: [],
    focusIndex: idx,
    anchorIndex: idx,
    shiftBaseIds: null,
  };
}

/**
 * 键盘 Shift + ↑/↓ 连续范围扩选 / 回缩（不循环越界，贴合原生桌面列表习惯）。
 */
export function applyClipboardShiftArrow(
  state: ClipboardSelectionState,
  itemIds: readonly number[],
  delta: -1 | 1,
  multiSelectMode: boolean,
): ClipboardSelectionState {
  if (itemIds.length === 0) return state;
  const currentFocus = clampIndex(state.focusIndex, itemIds.length);
  const nextFocus = clampIndex(currentFocus + delta, itemIds.length);
  const anchor = clampIndex(
    state.anchorIndex !== null ? state.anchorIndex : currentFocus,
    itemIds.length,
  );
  const rangeIds = buildDirectionalRangeIds(itemIds, anchor, nextFocus);

  const nextBase =
    state.shiftBaseIds !== null
      ? [...state.shiftBaseIds]
      : multiSelectMode
        ? state.selectedIds.filter((id) => id !== itemIds[anchor])
        : [];

  return {
    selectedIds: mergeUniqueIds(nextBase, rangeIds),
    focusIndex: nextFocus,
    anchorIndex: anchor,
    shiftBaseIds: nextBase,
  };
}

/**
 * 键盘 Space 快速切换勾选当前焦点项。
 */
export function applyClipboardToggleAtFocus(
  state: ClipboardSelectionState,
  itemIds: readonly number[],
): ClipboardSelectionState {
  if (itemIds.length === 0) return state;
  const idx = clampIndex(state.focusIndex, itemIds.length);
  const targetId = itemIds[idx];
  const exists = state.selectedIds.includes(targetId);
  const nextSelected = exists
    ? state.selectedIds.filter((id) => id !== targetId)
    : [...state.selectedIds, targetId];
  return {
    selectedIds: nextSelected,
    focusIndex: idx,
    anchorIndex: idx,
    shiftBaseIds: null,
  };
}

/**
 * 键盘 Ctrl+A 全选当前列表所有可见项。
 */
export function applyClipboardSelectAll(
  state: ClipboardSelectionState,
  itemIds: readonly number[],
): ClipboardSelectionState {
  if (itemIds.length === 0) return state;
  return {
    selectedIds: [...itemIds],
    focusIndex: clampIndex(state.focusIndex, itemIds.length),
    anchorIndex: 0,
    shiftBaseIds: null,
  };
}

/**
 * 当列表刷新 / 过滤 / 删除后裁剪已消失的勾选 ID 并夹紧焦点与锚点。
 */
export function pruneClipboardSelection(
  state: ClipboardSelectionState,
  itemIds: readonly number[],
): ClipboardSelectionState {
  if (itemIds.length === 0) {
    return {
      selectedIds: [],
      focusIndex: 0,
      anchorIndex: null,
      shiftBaseIds: null,
    };
  }
  const alive = new Set<number>(itemIds);
  const filteredSelected = state.selectedIds.filter((id) => alive.has(id));
  const filteredShiftBase =
    state.shiftBaseIds !== null ? state.shiftBaseIds.filter((id) => alive.has(id)) : null;
  const nextFocus = clampIndex(state.focusIndex, itemIds.length);
  const nextAnchor = state.anchorIndex !== null ? clampIndex(state.anchorIndex, itemIds.length) : null;

  const unchanged =
    filteredSelected.length === state.selectedIds.length &&
    nextFocus === state.focusIndex &&
    nextAnchor === state.anchorIndex &&
    (state.shiftBaseIds === null
      ? filteredShiftBase === null
      : filteredShiftBase !== null && filteredShiftBase.length === state.shiftBaseIds.length);

  if (unchanged) return state;
  return {
    selectedIds: filteredSelected,
    focusIndex: nextFocus,
    anchorIndex: nextAnchor,
    shiftBaseIds: filteredShiftBase,
  };
}
