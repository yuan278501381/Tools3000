export interface VirtualRange {
  start: number;
  end: number;
}

/**
 * Dynamic-height offsets without rebuilding an O(n) prefix array whenever one
 * visible row changes size. Every row starts at the density estimate; the
 * Fenwick tree stores only its measured deviation. Typed arrays make the
 * per-result memory bounded and substantially smaller than a Map of objects.
 */
export class DynamicRowLayout {
  private readonly deltas: Float32Array;
  private readonly tree: Float64Array;
  readonly count: number;
  readonly estimatedHeight: number;

  constructor(count: number, estimatedHeight: number) {
    this.count = count;
    this.estimatedHeight = estimatedHeight;
    this.deltas = new Float32Array(count);
    this.tree = new Float64Array(count + 1);
  }

  offsetOf(index: number): number {
    const clamped = Math.max(0, Math.min(this.count, index));
    return clamped * this.estimatedHeight + this.prefixDelta(clamped);
  }

  totalHeight(): number {
    return this.offsetOf(this.count);
  }

  /** Returns true only if callers need to request a React layout update. */
  updateHeight(index: number, height: number): boolean {
    if (!Number.isInteger(index) || index < 0 || index >= this.count || !Number.isFinite(height) || height <= 0) {
      return false;
    }
    const next = height - this.estimatedHeight;
    const previous = this.deltas[index];
    if (Math.abs(previous - next) <= 1) return false;
    this.deltas[index] = next;
    const delta = next - previous;
    for (let treeIndex = index + 1; treeIndex <= this.count; treeIndex += treeIndex & -treeIndex) {
      this.tree[treeIndex] += delta;
    }
    return true;
  }

  /** Finds the row covering offset; binary search is O(log² n) with no O(n) rebuild. */
  indexAtOffset(offset: number): number {
    if (this.count === 0) return 0;
    const target = Math.max(0, offset);
    let low = 0;
    let high = this.count;
    while (low < high) {
      const middle = low + Math.floor((high - low) / 2);
      if (this.offsetOf(middle + 1) <= target) low = middle + 1;
      else high = middle;
    }
    return low;
  }

  private prefixDelta(endExclusive: number): number {
    let total = 0;
    for (let treeIndex = endExclusive; treeIndex > 0; treeIndex -= treeIndex & -treeIndex) {
      total += this.tree[treeIndex];
    }
    return total;
  }
}

/**
 * 视口外的活动项必须留在 DOM 中供 aria-activedescendant 引用，但绝不能把
 * 它和当前视口之间的所有结果一并渲染。
 */
export function isSelectedOutsideVirtualRange(
  resultCount: number,
  selectedIndex: number,
  range: VirtualRange,
): boolean {
  return selectedIndex >= 0 && selectedIndex < resultCount &&
    (selectedIndex < range.start || selectedIndex >= range.end);
}

export function virtualizedNodeCount(
  resultCount: number,
  selectedIndex: number,
  range: VirtualRange,
): number {
  const visible = Math.max(0, Math.min(resultCount, range.end) - Math.max(0, range.start));
  return visible + (isSelectedOutsideVirtualRange(resultCount, selectedIndex, range) ? 1 : 0);
}
