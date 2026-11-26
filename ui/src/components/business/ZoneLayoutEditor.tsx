/* ─────────────────────────────────────────────────────────────────────────────
 * ZoneLayoutEditor — 可视化矢量 SnapZones 网格与自由画布布局编辑器
 *
 * 特性:
 *   1. 支持 Grid 网格模式（行列步进、分割线直拖调比、均分、多选合并/拆分单元格）
 *   2. 支持 Canvas 自由画布模式（增删/复制区域、自由拖拽位移、角点缩放、坐标微调）
 *   3. 100% @container 响应式，兼容 0..1 / 0..100 / 0..10000 比例坐标无缝归一化
 * ───────────────────────────────────────────────────────────────────────────── */

import { useCallback, useMemo, useRef, useState, type FC, type PointerEvent as ReactPointerEvent } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Columns,
  Copy,
  Grid2x2,
  Layers,
  LayoutGrid,
  Maximize2,
  Merge,
  Minus,
  Plus,
  Rows,
  Scissors,
  SlidersHorizontal,
  Trash2,
} from 'lucide-react';
import { Button, CodeBadge, NumberInput, TextInput } from '../UIKit';
import type { SnapZoneCanvasRect, SnapZoneLayout, SnapZoneLayoutType } from '../../types/windowManager';
import './ZoneLayoutEditor.css';

export interface ZoneLayoutEditorProps {
  layouts: SnapZoneLayout[];
  activeLayoutId: string;
  showZoneNumbers?: boolean;
  zoneGap?: number;
  disabled?: boolean;
  onSelectLayout: (layoutId: string) => void;
  onChangeLayouts: (nextLayouts: SnapZoneLayout[], nextActiveId?: string) => void;
}

interface NormalizedZoneBox {
  zoneId: number;
  displayNumber: number;
  x: number; // 0..100
  y: number; // 0..100
  w: number; // 0..100
  h: number; // 0..100
  minRow?: number;
  maxRow?: number;
  minCol?: number;
  maxCol?: number;
  cellCount?: number;
}

interface PresetTemplate {
  id: string;
  labelKey: string;
  fallback: string;
  rows: number;
  columns: number;
  rowPercents: number[];
  colPercents: number[];
  cellMap: number[][];
}

const QUICK_GRID_TEMPLATES: readonly PresetTemplate[] = [
  {
    id: 'tpl-2col',
    labelKey: 'windowManager.presetTwoCols',
    fallback: '2 Columns',
    rows: 1,
    columns: 2,
    rowPercents: [100],
    colPercents: [50, 50],
    cellMap: [[0, 1]],
  },
  {
    id: 'tpl-priority',
    labelKey: 'windowManager.presetPriority',
    fallback: 'Priority (25/50/25)',
    rows: 1,
    columns: 3,
    rowPercents: [100],
    colPercents: [25, 50, 25],
    cellMap: [[0, 1, 2]],
  },
  {
    id: 'tpl-2x2',
    labelKey: 'windowManager.presetGrid2x2',
    fallback: '2x2 Grid',
    rows: 2,
    columns: 2,
    rowPercents: [50, 50],
    colPercents: [50, 50],
    cellMap: [
      [0, 1],
      [2, 3],
    ],
  },
  {
    id: 'tpl-main-stack',
    labelKey: 'windowManager.presetMainStack',
    fallback: 'Main + Stack',
    rows: 2,
    columns: 2,
    rowPercents: [50, 50],
    colPercents: [60, 40],
    cellMap: [
      [0, 1],
      [0, 2],
    ],
  },
];

const MIN_TRACK_PERCENT = 10;
const MIN_CANVAS_SIZE = 10;
const MAX_GRID_DIM = 6;

function clamp(v: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(hi, v));
}

function normalizePercentsTo100(raw: number[] | undefined, count: number): number[] {
  const safeCount = Math.max(1, count);
  if (!Array.isArray(raw) || raw.length !== safeCount) {
    const base = Math.floor(100 / safeCount);
    const out = Array.from({ length: safeCount }, () => base);
    out[safeCount - 1] += 100 - base * safeCount;
    return out;
  }
  const positive = raw.map((v) => (Number.isFinite(v) && v > 0 ? v : 1));
  const sum = positive.reduce((a, b) => a + b, 0);
  if (sum <= 0) {
    return normalizePercentsTo100(undefined, safeCount);
  }
  const scaled = positive.map((v) => Math.max(1, Math.round((v / sum) * 100)));
  const diff = 100 - scaled.reduce((a, b) => a + b, 0);
  scaled[scaled.length - 1] = Math.max(1, scaled[scaled.length - 1] + diff);
  return scaled;
}

function detectPercentScale(raw?: number[]): number {
  if (!Array.isArray(raw) || raw.length === 0) return 100;
  const sum = raw.reduce((a, b) => a + (Number.isFinite(b) ? b : 0), 0);
  return sum > 200 ? 10000 : 100;
}

function rescalePercents(percents100: number[], targetScale: number): number[] {
  if (targetScale === 100) return percents100;
  const scaled = percents100.map((p) => Math.round((p / 100) * targetScale));
  const diff = targetScale - scaled.reduce((a, b) => a + b, 0);
  scaled[scaled.length - 1] += diff;
  return scaled;
}

function detectCanvasScale(zones?: SnapZoneCanvasRect[]): number {
  if (!Array.isArray(zones) || zones.length === 0) return 100;
  let maxVal = 0;
  for (const z of zones) {
    maxVal = Math.max(maxVal, z.x, z.y, z.w, z.h, z.x + z.w, z.y + z.h);
  }
  if (maxVal <= 1.05) return 1;
  if (maxVal > 120) return 10000;
  return 100;
}

function buildValidCellMap(rows: number, columns: number, rawMap?: number[][]): number[][] {
  if (
    Array.isArray(rawMap) &&
    rawMap.length === rows &&
    rawMap.every((row) => Array.isArray(row) && row.length === columns)
  ) {
    return rawMap.map((row) => row.map((v) => (Number.isFinite(v) ? v : 0)));
  }
  let idx = 0;
  return Array.from({ length: rows }, () =>
    Array.from({ length: columns }, () => idx++),
  );
}

function compactCellMapIds(map: number[][]): number[][] {
  const idToNew = new Map<number, number>();
  let next = 0;
  return map.map((row) =>
    row.map((id) => {
      if (!idToNew.has(id)) {
        idToNew.set(id, next++);
      }
      return idToNew.get(id)!;
    }),
  );
}

function computeLayoutZoneBoxes(layout: SnapZoneLayout): NormalizedZoneBox[] {
  if (layout.type === 'canvas') {
    const zones = Array.isArray(layout.zones) ? layout.zones : [];
    const scale = detectCanvasScale(zones);
    const factor = scale === 1 ? 100 : scale === 10000 ? 0.01 : 1;
    return zones.map((z, idx) => {
      const w = clamp(z.w * factor, 2, 100);
      const h = clamp(z.h * factor, 2, 100);
      const x = clamp(z.x * factor, 0, Math.max(0, 100 - w));
      const y = clamp(z.y * factor, 0, Math.max(0, 100 - h));
      return {
        zoneId: z.id,
        displayNumber: idx + 1,
        x,
        y,
        w,
        h,
      };
    });
  }

  const rows = clamp(layout.rows ?? layout.cellMap?.length ?? 1, 1, MAX_GRID_DIM);
  const cols = clamp(layout.columns ?? layout.cellMap?.[0]?.length ?? 2, 1, MAX_GRID_DIM);
  const rowP = normalizePercentsTo100(layout.rowPercents, rows);
  const colP = normalizePercentsTo100(layout.colPercents, cols);
  const cellMap = buildValidCellMap(rows, cols, layout.cellMap);

  const rowStarts: number[] = [0];
  for (let r = 0; r < rows; r += 1) rowStarts.push(rowStarts[r] + rowP[r]);
  const colStarts: number[] = [0];
  for (let c = 0; c < cols; c += 1) colStarts.push(colStarts[c] + colP[c]);

  const bounds = new Map<
    number,
    { minRow: number; maxRow: number; minCol: number; maxCol: number; cellCount: number }
  >();
  for (let r = 0; r < rows; r += 1) {
    for (let c = 0; c < cols; c += 1) {
      const id = cellMap[r][c];
      const existing = bounds.get(id);
      if (!existing) {
        bounds.set(id, { minRow: r, maxRow: r, minCol: c, maxCol: c, cellCount: 1 });
      } else {
        existing.minRow = Math.min(existing.minRow, r);
        existing.maxRow = Math.max(existing.maxRow, r);
        existing.minCol = Math.min(existing.minCol, c);
        existing.maxCol = Math.max(existing.maxCol, c);
        existing.cellCount += 1;
      }
    }
  }

  const ordered = Array.from(bounds.entries()).sort((a, b) => {
    if (a[1].minRow !== b[1].minRow) return a[1].minRow - b[1].minRow;
    if (a[1].minCol !== b[1].minCol) return a[1].minCol - b[1].minCol;
    return a[0] - b[0];
  });

  return ordered.map(([zoneId, b], idx) => ({
    zoneId,
    displayNumber: idx + 1,
    x: colStarts[b.minCol],
    y: rowStarts[b.minRow],
    w: colStarts[b.maxCol + 1] - colStarts[b.minCol],
    h: rowStarts[b.maxRow + 1] - rowStarts[b.minRow],
    minRow: b.minRow,
    maxRow: b.maxRow,
    minCol: b.minCol,
    maxCol: b.maxCol,
    cellCount: b.cellCount,
  }));
}

function renderMiniPreviewSvg(layout: SnapZoneLayout) {
  const boxes = computeLayoutZoneBoxes(layout);
  return (
    <svg viewBox="0 0 160 90" className="zone-editor__mini-svg" aria-hidden="true">
      {boxes.map((box) => {
        const pad = 2;
        const rx = (box.x / 100) * 160 + pad;
        const ry = (box.y / 100) * 90 + pad;
        const rw = Math.max(4, (box.w / 100) * 160 - pad * 2);
        const rh = Math.max(4, (box.h / 100) * 90 - pad * 2);
        return (
          <rect
            key={box.zoneId}
            x={rx}
            y={ry}
            width={rw}
            height={rh}
            rx={3}
            className="zone-editor__mini-rect"
          />
        );
      })}
    </svg>
  );
}

export const ZoneLayoutEditor: FC<ZoneLayoutEditorProps> = ({
  layouts,
  activeLayoutId,
  showZoneNumbers = true,
  zoneGap = 8,
  disabled = false,
  onSelectLayout,
  onChangeLayouts,
}) => {
  const { t } = useTranslation();
  const stageRef = useRef<HTMLDivElement>(null);
  const [selectionState, setSelectionState] = useState<{ layoutId: string; ids: number[] }>({
    layoutId: activeLayoutId,
    ids: [],
  });
  const [draftLayout, setDraftLayout] = useState<SnapZoneLayout | null>(null);

  const selectedZoneIds = useMemo(
    () => (selectionState.layoutId === activeLayoutId ? selectionState.ids : []),
    [selectionState, activeLayoutId],
  );

  const setSelectedZoneIds = useCallback(
    (updater: number[] | ((prev: number[]) => number[])) => {
      setSelectionState((prev) => {
        const base = prev.layoutId === activeLayoutId ? prev.ids : [];
        const nextIds = typeof updater === 'function' ? updater(base) : updater;
        return { layoutId: activeLayoutId, ids: nextIds };
      });
    },
    [activeLayoutId],
  );

  const persistedActive = useMemo(
    () => layouts.find((l) => l.id === activeLayoutId) ?? layouts[0] ?? null,
    [layouts, activeLayoutId],
  );

  const currentLayout = draftLayout && persistedActive && draftLayout.id === persistedActive.id
    ? draftLayout
    : persistedActive;

  const commitLayoutUpdate = useCallback(
    (updated: SnapZoneLayout) => {
      const next = layouts.map((item) => (item.id === updated.id ? updated : item));
      onChangeLayouts(next, updated.id);
    },
    [layouts, onChangeLayouts],
  );

  // 清除失效的选中区
  const boxes = useMemo(
    () => (currentLayout ? computeLayoutZoneBoxes(currentLayout) : []),
    [currentLayout],
  );

  const validSelectedIds = useMemo(
    () => selectedZoneIds.filter((id) => boxes.some((b) => b.zoneId === id)),
    [selectedZoneIds, boxes],
  );

  const primarySelectedBox = useMemo(
    () => boxes.find((b) => b.zoneId === validSelectedIds[0]) ?? boxes[0] ?? null,
    [boxes, validSelectedIds],
  );

  // ── 网格结构信息 ──────────────────────────────────────────────────────────
  const gridMeta = useMemo(() => {
    if (!currentLayout || currentLayout.type !== 'grid') return null;
    const rows = clamp(currentLayout.rows ?? currentLayout.cellMap?.length ?? 1, 1, MAX_GRID_DIM);
    const columns = clamp(currentLayout.columns ?? currentLayout.cellMap?.[0]?.length ?? 2, 1, MAX_GRID_DIM);
    const rowPercents = normalizePercentsTo100(currentLayout.rowPercents, rows);
    const colPercents = normalizePercentsTo100(currentLayout.colPercents, columns);
    const cellMap = buildValidCellMap(rows, columns, currentLayout.cellMap);
    const rowScale = detectPercentScale(currentLayout.rowPercents);
    const colScale = detectPercentScale(currentLayout.colPercents);
    return { rows, columns, rowPercents, colPercents, cellMap, rowScale, colScale };
  }, [currentLayout]);

  // 判断当前多选区域是否可合法合并为一个连续矩形
  const canMergeSelected = useMemo(() => {
    if (!gridMeta || validSelectedIds.length < 2) return false;
    const selectedSet = new Set(validSelectedIds);
    let minR = gridMeta.rows;
    let maxR = -1;
    let minC = gridMeta.columns;
    let maxC = -1;
    for (let r = 0; r < gridMeta.rows; r += 1) {
      for (let c = 0; c < gridMeta.columns; c += 1) {
        if (selectedSet.has(gridMeta.cellMap[r][c])) {
          minR = Math.min(minR, r);
          maxR = Math.max(maxR, r);
          minC = Math.min(minC, c);
          maxC = Math.max(maxC, c);
        }
      }
    }
    if (maxR < minR || maxC < minC) return false;
    for (let r = minR; r <= maxR; r += 1) {
      for (let c = minC; c <= maxC; c += 1) {
        if (!selectedSet.has(gridMeta.cellMap[r][c])) {
          return false;
        }
      }
    }
    return true;
  }, [gridMeta, validSelectedIds]);

  const canSplitSelected = useMemo(() => {
    if (!gridMeta || validSelectedIds.length !== 1) return false;
    const target = boxes.find((b) => b.zoneId === validSelectedIds[0]);
    return Boolean(target && (target.cellCount ?? 0) > 1);
  }, [gridMeta, validSelectedIds, boxes]);

  // ── 拖拽分割线（网格模式） ────────────────────────────────────────────────
  const startGridDividerDrag = (
    axis: 'col' | 'row',
    dividerIndex: number,
    e: ReactPointerEvent<HTMLDivElement>,
  ) => {
    if (disabled || !currentLayout || !gridMeta || !stageRef.current) return;
    e.preventDefault();
    e.stopPropagation();

    const rect = stageRef.current.getBoundingClientRect();
    const totalSize = axis === 'col' ? rect.width : rect.height;
    if (totalSize <= 0) return;

    const initialPercents = axis === 'col' ? [...gridMeta.colPercents] : [...gridMeta.rowPercents];
    const startPos = axis === 'col' ? e.clientX : e.clientY;
    const pairSum = initialPercents[dividerIndex] + initialPercents[dividerIndex + 1];
    let latestLayout: SnapZoneLayout = currentLayout;

    const onMove = (ev: PointerEvent) => {
      const curPos = axis === 'col' ? ev.clientX : ev.clientY;
      const deltaPercent = ((curPos - startPos) / totalSize) * 100;
      const nextFirst = Math.round(
        clamp(
          initialPercents[dividerIndex] + deltaPercent,
          MIN_TRACK_PERCENT,
          pairSum - MIN_TRACK_PERCENT,
        ),
      );
      const nextSecond = pairSum - nextFirst;
      const updatedPercents = [...initialPercents];
      updatedPercents[dividerIndex] = nextFirst;
      updatedPercents[dividerIndex + 1] = nextSecond;

      latestLayout = {
        ...currentLayout,
        rowPercents: axis === 'row'
          ? rescalePercents(updatedPercents, gridMeta.rowScale)
          : currentLayout.rowPercents,
        colPercents: axis === 'col'
          ? rescalePercents(updatedPercents, gridMeta.colScale)
          : currentLayout.colPercents,
      };
      setDraftLayout(latestLayout);
    };

    const onUp = () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      setDraftLayout(null);
      commitLayoutUpdate(latestLayout);
    };

    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  };

  // ── 自由画布拖拽与角点缩放 ────────────────────────────────────────────────
  const startCanvasZoneDrag = (
    zoneId: number,
    mode: 'move' | 'resize',
    e: ReactPointerEvent<HTMLElement>,
  ) => {
    if (disabled || !currentLayout || currentLayout.type !== 'canvas' || !stageRef.current) return;
    e.preventDefault();
    e.stopPropagation();
    setSelectedZoneIds([zoneId]);

    const stageRect = stageRef.current.getBoundingClientRect();
    if (stageRect.width <= 0 || stageRect.height <= 0) return;

    const rawZones = Array.isArray(currentLayout.zones) ? currentLayout.zones : [];
    const scale = detectCanvasScale(rawZones);
    const toPct = scale === 1 ? 100 : scale === 10000 ? 0.01 : 1;
    const fromPct = scale === 1 ? 0.01 : scale === 10000 ? 100 : 1;

    const startZone = rawZones.find((z) => z.id === zoneId);
    if (!startZone) return;

    const initX = startZone.x * toPct;
    const initY = startZone.y * toPct;
    const initW = startZone.w * toPct;
    const initH = startZone.h * toPct;
    const startClientX = e.clientX;
    const startClientY = e.clientY;
    let latestLayout: SnapZoneLayout = currentLayout;

    const snap2 = (val: number) => Math.round(val / 2) * 2;

    const onMove = (ev: PointerEvent) => {
      const dxPct = ((ev.clientX - startClientX) / stageRect.width) * 100;
      const dyPct = ((ev.clientY - startClientY) / stageRect.height) * 100;

      let nx = initX;
      let ny = initY;
      let nw = initW;
      let nh = initH;

      if (mode === 'move') {
        nx = clamp(snap2(initX + dxPct), 0, Math.max(0, 100 - initW));
        ny = clamp(snap2(initY + dyPct), 0, Math.max(0, 100 - initH));
      } else {
        nw = clamp(snap2(initW + dxPct), MIN_CANVAS_SIZE, Math.max(MIN_CANVAS_SIZE, 100 - initX));
        nh = clamp(snap2(initH + dyPct), MIN_CANVAS_SIZE, Math.max(MIN_CANVAS_SIZE, 100 - initY));
      }

      const nextZones = rawZones.map((z) =>
        z.id === zoneId
          ? {
              ...z,
              x: Number((nx * fromPct).toFixed(scale === 1 ? 4 : 0)),
              y: Number((ny * fromPct).toFixed(scale === 1 ? 4 : 0)),
              w: Number((nw * fromPct).toFixed(scale === 1 ? 4 : 0)),
              h: Number((nh * fromPct).toFixed(scale === 1 ? 4 : 0)),
            }
          : z,
      );
      latestLayout = { ...currentLayout, zones: nextZones };
      setDraftLayout(latestLayout);
    };

    const onUp = () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      setDraftLayout(null);
      commitLayoutUpdate(latestLayout);
    };

    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  };

  // ── 布局 CRUD 操作 ────────────────────────────────────────────────────────
  const handleCreateLayout = (type: SnapZoneLayoutType, preset?: PresetTemplate) => {
    if (disabled) return;
    const id = `custom-${type}-${Date.now()}`;
    const nextLayout: SnapZoneLayout =
      type === 'canvas'
        ? {
            id,
            name: t('windowManager.newCanvasLayoutName', 'Custom Canvas'),
            type: 'canvas',
            builtin: false,
            zones: [
              { id: 0, x: 0, y: 0, w: 50, h: 100 },
              { id: 1, x: 50, y: 0, w: 50, h: 50 },
              { id: 2, x: 50, y: 50, w: 50, h: 50 },
            ],
          }
        : {
            id,
            // eslint-disable-next-line @typescript-eslint/no-explicit-any
            name: preset ? String(t(preset.labelKey as any, preset.fallback)) : t('windowManager.newGridLayoutName', 'Custom Grid'),
            type: 'grid',
            builtin: false,
            rows: preset?.rows ?? 1,
            columns: preset?.columns ?? 3,
            rowPercents: preset?.rowPercents ?? [100],
            colPercents: preset?.colPercents ?? [33, 34, 33],
            cellMap: preset?.cellMap ?? [[0, 1, 2]],
          };
    onChangeLayouts([...layouts, nextLayout], id);
    onSelectLayout(id);
  };

  const handleDuplicateLayout = () => {
    if (disabled || !currentLayout) return;
    const id = `custom-copy-${Date.now()}`;
    const cloned: SnapZoneLayout = {
      ...currentLayout,
      id,
      builtin: false,
      name: `${currentLayout.name} (${t('windowManager.copySuffix', 'Copy')})`,
      rowPercents: currentLayout.rowPercents ? [...currentLayout.rowPercents] : undefined,
      colPercents: currentLayout.colPercents ? [...currentLayout.colPercents] : undefined,
      cellMap: currentLayout.cellMap ? currentLayout.cellMap.map((r) => [...r]) : undefined,
      zones: currentLayout.zones ? currentLayout.zones.map((z) => ({ ...z })) : undefined,
    };
    onChangeLayouts([...layouts, cloned], id);
    onSelectLayout(id);
  };

  const handleDeleteLayout = () => {
    if (disabled || !currentLayout || layouts.length <= 1) return;
    const next = layouts.filter((item) => item.id !== currentLayout.id);
    const fallbackId = next[0]?.id ?? '';
    onChangeLayouts(next, fallbackId);
    if (fallbackId) onSelectLayout(fallbackId);
  };

  const handleSwitchMode = (nextType: SnapZoneLayoutType) => {
    if (disabled || !currentLayout || currentLayout.type === nextType) return;
    if (nextType === 'canvas') {
      const convertedZones: SnapZoneCanvasRect[] = boxes.map((box, idx) => ({
        id: idx,
        x: Math.round(box.x),
        y: Math.round(box.y),
        w: Math.round(box.w),
        h: Math.round(box.h),
      }));
      commitLayoutUpdate({
        ...currentLayout,
        type: 'canvas',
        zones: convertedZones.length > 0 ? convertedZones : [{ id: 0, x: 0, y: 0, w: 50, h: 100 }, { id: 1, x: 50, y: 0, w: 50, h: 100 }],
      });
    } else {
      commitLayoutUpdate({
        ...currentLayout,
        type: 'grid',
        rows: currentLayout.rows ?? 2,
        columns: currentLayout.columns ?? 2,
        rowPercents: normalizePercentsTo100(currentLayout.rowPercents, currentLayout.rows ?? 2),
        colPercents: normalizePercentsTo100(currentLayout.colPercents, currentLayout.columns ?? 2),
        cellMap: buildValidCellMap(currentLayout.rows ?? 2, currentLayout.columns ?? 2, currentLayout.cellMap),
      });
    }
    setSelectedZoneIds([]);
  };

  const handleResizeGridDimensions = (nextRows: number, nextCols: number) => {
    if (disabled || !currentLayout || !gridMeta) return;
    const rows = clamp(nextRows, 1, MAX_GRID_DIM);
    const columns = clamp(nextCols, 1, MAX_GRID_DIM);
    const nextRowP = normalizePercentsTo100(undefined, rows);
    const nextColP = normalizePercentsTo100(undefined, columns);
    let nextId = 0;
    const nextMap = Array.from({ length: rows }, () =>
      Array.from({ length: columns }, () => nextId++),
    );
    commitLayoutUpdate({
      ...currentLayout,
      rows,
      columns,
      rowPercents: rescalePercents(nextRowP, gridMeta.rowScale),
      colPercents: rescalePercents(nextColP, gridMeta.colScale),
      cellMap: nextMap,
    });
    setSelectedZoneIds([]);
  };

  const handleEqualizeGrid = () => {
    if (disabled || !currentLayout || !gridMeta) return;
    commitLayoutUpdate({
      ...currentLayout,
      rowPercents: rescalePercents(normalizePercentsTo100(undefined, gridMeta.rows), gridMeta.rowScale),
      colPercents: rescalePercents(normalizePercentsTo100(undefined, gridMeta.columns), gridMeta.colScale),
    });
  };

  const handleMergeSelected = () => {
    if (disabled || !currentLayout || !gridMeta || !canMergeSelected) return;
    const targetId = Math.min(...validSelectedIds);
    const selectedSet = new Set(validSelectedIds);
    const mergedMap = gridMeta.cellMap.map((row) =>
      row.map((id) => (selectedSet.has(id) ? targetId : id)),
    );
    commitLayoutUpdate({
      ...currentLayout,
      cellMap: compactCellMapIds(mergedMap),
    });
    setSelectedZoneIds([]);
  };

  const handleSplitSelected = () => {
    if (disabled || !currentLayout || !gridMeta || !canSplitSelected) return;
    const splitId = validSelectedIds[0];
    let maxExisting = 0;
    for (const row of gridMeta.cellMap) {
      for (const v of row) maxExisting = Math.max(maxExisting, v);
    }
    let nextId = maxExisting + 1;
    let firstKept = false;
    const nextMap = gridMeta.cellMap.map((row) =>
      row.map((id) => {
        if (id !== splitId) return id;
        if (!firstKept) {
          firstKept = true;
          return id;
        }
        return nextId++;
      }),
    );
    commitLayoutUpdate({
      ...currentLayout,
      cellMap: compactCellMapIds(nextMap),
    });
    setSelectedZoneIds([]);
  };

  const handleAddCanvasZone = () => {
    if (disabled || !currentLayout || currentLayout.type !== 'canvas') return;
    const rawZones = Array.isArray(currentLayout.zones) ? currentLayout.zones : [];
    const scale = detectCanvasScale(rawZones);
    const fromPct = scale === 1 ? 0.01 : scale === 10000 ? 100 : 1;
    const nextId = rawZones.reduce((max, z) => Math.max(max, z.id), -1) + 1;
    const offset = (rawZones.length * 6) % 40;
    const created: SnapZoneCanvasRect = {
      id: nextId,
      x: Math.round((10 + offset) * fromPct),
      y: Math.round((10 + offset) * fromPct),
      w: Math.round(40 * fromPct),
      h: Math.round(50 * fromPct),
    };
    commitLayoutUpdate({
      ...currentLayout,
      zones: [...rawZones, created],
    });
    setSelectedZoneIds([nextId]);
  };

  const handleDeleteCanvasZone = (zoneId: number) => {
    if (disabled || !currentLayout || currentLayout.type !== 'canvas') return;
    const rawZones = Array.isArray(currentLayout.zones) ? currentLayout.zones : [];
    if (rawZones.length <= 1) return;
    const nextZones = rawZones.filter((z) => z.id !== zoneId);
    commitLayoutUpdate({ ...currentLayout, zones: nextZones });
    setSelectedZoneIds(nextZones[0] ? [nextZones[0].id] : []);
  };

  const handleUpdateCanvasZoneField = (
    zoneId: number,
    field: 'x' | 'y' | 'w' | 'h',
    pctValue: number,
  ) => {
    if (disabled || !currentLayout || currentLayout.type !== 'canvas') return;
    const rawZones = Array.isArray(currentLayout.zones) ? currentLayout.zones : [];
    const scale = detectCanvasScale(rawZones);
    const toPct = scale === 1 ? 100 : scale === 10000 ? 0.01 : 1;
    const fromPct = scale === 1 ? 0.01 : scale === 10000 ? 100 : 1;

    const nextZones = rawZones.map((z) => {
      if (z.id !== zoneId) return z;
      let x = z.x * toPct;
      let y = z.y * toPct;
      let w = z.w * toPct;
      let h = z.h * toPct;
      if (field === 'x') x = clamp(pctValue, 0, 100 - w);
      if (field === 'y') y = clamp(pctValue, 0, 100 - h);
      if (field === 'w') w = clamp(pctValue, MIN_CANVAS_SIZE, 100 - x);
      if (field === 'h') h = clamp(pctValue, MIN_CANVAS_SIZE, 100 - y);
      return {
        ...z,
        x: Number((x * fromPct).toFixed(scale === 1 ? 4 : 0)),
        y: Number((y * fromPct).toFixed(scale === 1 ? 4 : 0)),
        w: Number((w * fromPct).toFixed(scale === 1 ? 4 : 0)),
        h: Number((h * fromPct).toFixed(scale === 1 ? 4 : 0)),
      };
    });
    commitLayoutUpdate({ ...currentLayout, zones: nextZones });
  };

  const handleZoneClick = (zoneId: number, multi: boolean) => {
    if (disabled) return;
    if (currentLayout?.type === 'grid' && multi) {
      setSelectedZoneIds((prev) =>
        prev.includes(zoneId) ? prev.filter((id) => id !== zoneId) : [...prev, zoneId],
      );
    } else {
      setSelectedZoneIds((prev) =>
        prev.length === 1 && prev[0] === zoneId ? prev : [zoneId],
      );
    }
  };

  // 计算分割线百分比累计位置
  const colDividerOffsets = useMemo(() => {
    if (!gridMeta) return [];
    const offsets: number[] = [];
    let acc = 0;
    for (let i = 0; i < gridMeta.colPercents.length - 1; i += 1) {
      acc += gridMeta.colPercents[i];
      offsets.push(acc);
    }
    return offsets;
  }, [gridMeta]);

  const rowDividerOffsets = useMemo(() => {
    if (!gridMeta) return [];
    const offsets: number[] = [];
    let acc = 0;
    for (let i = 0; i < gridMeta.rowPercents.length - 1; i += 1) {
      acc += gridMeta.rowPercents[i];
      offsets.push(acc);
    }
    return offsets;
  }, [gridMeta]);

  const gapInsetPx = clamp(Math.round(zoneGap * 0.45), 1, 10);

  return (
    <div className={`zone-editor ${disabled ? 'zone-editor--disabled' : ''}`}>
      {/* ── 1. 布局缩略卡片列表与快捷新建栏 ───────────────────────────────── */}
      <div className="zone-editor__strip" role="listbox" aria-label={t('windowManager.layoutsListAria', 'SnapZones layouts')}>
        {layouts.map((layout) => {
          const isSelected = currentLayout?.id === layout.id;
          return (
            <button
              key={layout.id}
              type="button"
              role="option"
              aria-selected={isSelected}
              className={`zone-editor__layout-card ${isSelected ? 'is-selected' : ''}`}
              disabled={disabled}
              onClick={() => onSelectLayout(layout.id)}
            >
              <div className="zone-editor__mini-preview">
                {renderMiniPreviewSvg(layout)}
              </div>
              <div className="zone-editor__layout-card-meta">
                <span className="zone-editor__layout-card-name">{layout.name}</span>
                <span className="zone-editor__layout-card-kind">
                  {layout.type === 'canvas'
                    ? t('windowManager.modeCanvas', 'Canvas')
                    : t('windowManager.modeGrid', 'Grid')}
                </span>
              </div>
            </button>
          );
        })}

        <div className="zone-editor__new-actions">
          <Button
            variant="secondary"
            size="sm"
            disabled={disabled}
            onClick={() => handleCreateLayout('grid')}
          >
            <Plus size={14} aria-hidden="true" />
            <span>{t('windowManager.newGridBtn', 'New Grid')}</span>
          </Button>
          <Button
            variant="ghost"
            size="sm"
            disabled={disabled}
            onClick={() => handleCreateLayout('canvas')}
          >
            <Layers size={14} aria-hidden="true" />
            <span>{t('windowManager.newCanvasBtn', 'New Canvas')}</span>
          </Button>
        </div>
      </div>

      {/* ── 2. 一键常用模板快选胶囊 ─────────────────────────────────────────── */}
      <div className="zone-editor__presets-bar">
        <span className="zone-editor__presets-label">
          <LayoutGrid size={14} aria-hidden="true" />
          <span>{t('windowManager.quickTemplatesLabel', 'Quick Templates:')}</span>
        </span>
        <div className="zone-editor__presets-pills">
          {QUICK_GRID_TEMPLATES.map((tpl) => (
            <button
              key={tpl.id}
              type="button"
              className="zone-editor__preset-pill"
              disabled={disabled}
              onClick={() => handleCreateLayout('grid', tpl)}
            >
              {/* eslint-disable-next-line @typescript-eslint/no-explicit-any */}
              {t(tpl.labelKey as any, tpl.fallback)}
            </button>
          ))}
        </div>
      </div>

      {/* ── 3. 当前布局属性与工具栏 ─────────────────────────────────────────── */}
      {currentLayout && (
        <div className="zone-editor__workspace">
          <div className="zone-editor__toolbar">
            <div className="zone-editor__name-group">
              <TextInput
                value={currentLayout.name}
                disabled={disabled}
                ariaLabel={t('windowManager.layoutNameAria', 'Layout Name')}
                placeholder={t('windowManager.layoutNamePlaceholder', 'Layout name')}
                onChange={(name) => commitLayoutUpdate({ ...currentLayout, name })}
              />
            </div>

            <div className="zone-editor__mode-switch" role="group" aria-label={t('windowManager.editorModeAria', 'Layout Mode')}>
              <button
                type="button"
                className={`zone-editor__mode-btn ${currentLayout.type === 'grid' ? 'is-active' : ''}`}
                disabled={disabled}
                onClick={() => handleSwitchMode('grid')}
              >
                <Grid2x2 size={14} aria-hidden="true" />
                <span>{t('windowManager.modeGrid', 'Grid')}</span>
              </button>
              <button
                type="button"
                className={`zone-editor__mode-btn ${currentLayout.type === 'canvas' ? 'is-active' : ''}`}
                disabled={disabled}
                onClick={() => handleSwitchMode('canvas')}
              >
                <Layers size={14} aria-hidden="true" />
                <span>{t('windowManager.modeCanvas', 'Canvas')}</span>
              </button>
            </div>

            <div className="zone-editor__toolbar-actions">
              <Button
                variant="ghost"
                size="sm"
                disabled={disabled}
                onClick={handleDuplicateLayout}
                title={t('windowManager.duplicateLayout', 'Duplicate Layout')}
              >
                <Copy size={14} aria-hidden="true" />
                <span>{t('windowManager.duplicateLayout', 'Duplicate')}</span>
              </Button>
              <Button
                variant="ghost"
                size="sm"
                disabled={disabled || layouts.length <= 1}
                onClick={handleDeleteLayout}
                title={t('windowManager.deleteLayout', 'Delete Layout')}
              >
                <Trash2 size={14} aria-hidden="true" />
              </Button>
            </div>
          </div>

          {/* ── 4. 模式专属编辑控制条 ──────────────────────────────────────── */}
          {currentLayout.type === 'grid' && gridMeta && (
            <div className="zone-editor__subbar">
              <div className="zone-editor__stepper">
                <span className="zone-editor__stepper-label">
                  <Rows size={14} aria-hidden="true" />
                  <span>{t('windowManager.gridRows', 'Rows')}</span>
                </span>
                <button
                  type="button"
                  className="zone-editor__step-btn"
                  disabled={disabled || gridMeta.rows <= 1}
                  aria-label={t('windowManager.decreaseRows', 'Decrease rows')}
                  onClick={() => handleResizeGridDimensions(gridMeta.rows - 1, gridMeta.columns)}
                >
                  <Minus size={13} aria-hidden="true" />
                </button>
                <CodeBadge>{gridMeta.rows}</CodeBadge>
                <button
                  type="button"
                  className="zone-editor__step-btn"
                  disabled={disabled || gridMeta.rows >= MAX_GRID_DIM}
                  aria-label={t('windowManager.increaseRows', 'Increase rows')}
                  onClick={() => handleResizeGridDimensions(gridMeta.rows + 1, gridMeta.columns)}
                >
                  <Plus size={13} aria-hidden="true" />
                </button>
              </div>

              <div className="zone-editor__stepper">
                <span className="zone-editor__stepper-label">
                  <Columns size={14} aria-hidden="true" />
                  <span>{t('windowManager.gridCols', 'Columns')}</span>
                </span>
                <button
                  type="button"
                  className="zone-editor__step-btn"
                  disabled={disabled || gridMeta.columns <= 1}
                  aria-label={t('windowManager.decreaseCols', 'Decrease columns')}
                  onClick={() => handleResizeGridDimensions(gridMeta.rows, gridMeta.columns - 1)}
                >
                  <Minus size={13} aria-hidden="true" />
                </button>
                <CodeBadge>{gridMeta.columns}</CodeBadge>
                <button
                  type="button"
                  className="zone-editor__step-btn"
                  disabled={disabled || gridMeta.columns >= MAX_GRID_DIM}
                  aria-label={t('windowManager.increaseCols', 'Increase columns')}
                  onClick={() => handleResizeGridDimensions(gridMeta.rows, gridMeta.columns + 1)}
                >
                  <Plus size={13} aria-hidden="true" />
                </button>
              </div>

              <div className="zone-editor__subbar-actions">
                <Button variant="ghost" size="sm" disabled={disabled} onClick={handleEqualizeGrid}>
                  <SlidersHorizontal size={13} aria-hidden="true" />
                  <span>{t('windowManager.equalizeGrid', 'Equalize')}</span>
                </Button>
                <Button
                  variant="secondary"
                  size="sm"
                  disabled={disabled || !canMergeSelected}
                  onClick={handleMergeSelected}
                >
                  <Merge size={13} aria-hidden="true" />
                  <span>{t('windowManager.mergeZones', 'Merge')}</span>
                </Button>
                <Button
                  variant="secondary"
                  size="sm"
                  disabled={disabled || !canSplitSelected}
                  onClick={handleSplitSelected}
                >
                  <Scissors size={13} aria-hidden="true" />
                  <span>{t('windowManager.splitZone', 'Split')}</span>
                </Button>
              </div>
            </div>
          )}

          {currentLayout.type === 'canvas' && (
            <div className="zone-editor__subbar">
              <div className="zone-editor__subbar-actions">
                <Button variant="secondary" size="sm" disabled={disabled} onClick={handleAddCanvasZone}>
                  <Plus size={14} aria-hidden="true" />
                  <span>{t('windowManager.addZoneBtn', 'Add Zone')}</span>
                </Button>
                {primarySelectedBox && (
                  <Button
                    variant="ghost"
                    size="sm"
                    disabled={disabled || boxes.length <= 1}
                    onClick={() => handleDeleteCanvasZone(primarySelectedBox.zoneId)}
                  >
                    <Trash2 size={14} aria-hidden="true" />
                    <span>{t('windowManager.deleteZoneBtn', 'Delete Zone')}</span>
                  </Button>
                )}
              </div>

              {primarySelectedBox && (
                <div className="zone-editor__canvas-coords">
                  <label className="zone-editor__coord-field">
                    <span>X</span>
                    <NumberInput
                      value={Math.round(primarySelectedBox.x)}
                      min={0}
                      max={90}
                      step={2}
                      unit="%"
                      disabled={disabled}
                      ariaLabel="Zone X"
                      onChange={(val) => handleUpdateCanvasZoneField(primarySelectedBox.zoneId, 'x', val)}
                    />
                  </label>
                  <label className="zone-editor__coord-field">
                    <span>Y</span>
                    <NumberInput
                      value={Math.round(primarySelectedBox.y)}
                      min={0}
                      max={90}
                      step={2}
                      unit="%"
                      disabled={disabled}
                      ariaLabel="Zone Y"
                      onChange={(val) => handleUpdateCanvasZoneField(primarySelectedBox.zoneId, 'y', val)}
                    />
                  </label>
                  <label className="zone-editor__coord-field">
                    <span>W</span>
                    <NumberInput
                      value={Math.round(primarySelectedBox.w)}
                      min={MIN_CANVAS_SIZE}
                      max={100}
                      step={2}
                      unit="%"
                      disabled={disabled}
                      ariaLabel="Zone Width"
                      onChange={(val) => handleUpdateCanvasZoneField(primarySelectedBox.zoneId, 'w', val)}
                    />
                  </label>
                  <label className="zone-editor__coord-field">
                    <span>H</span>
                    <NumberInput
                      value={Math.round(primarySelectedBox.h)}
                      min={MIN_CANVAS_SIZE}
                      max={100}
                      step={2}
                      unit="%"
                      disabled={disabled}
                      ariaLabel="Zone Height"
                      onChange={(val) => handleUpdateCanvasZoneField(primarySelectedBox.zoneId, 'h', val)}
                    />
                  </label>
                </div>
              )}
            </div>
          )}

          {/* ── 5. 16:9 交互式桌面预览画布 ────────────────────────────────── */}
          <div className="zone-editor__monitor-shell">
            <div ref={stageRef} className="zone-editor__stage">
              {boxes.map((box) => {
                const isSelected = validSelectedIds.includes(box.zoneId);
                return (
                  <div
                    key={box.zoneId}
                    role="button"
                    tabIndex={disabled ? -1 : 0}
                    aria-pressed={isSelected}
                    className={`zone-editor__zone ${isSelected ? 'is-selected' : ''} ${
                      currentLayout.type === 'canvas' ? 'zone-editor__zone--draggable' : ''
                    }`}
                    style={{
                      left: `calc(${box.x}% + ${gapInsetPx}px)`,
                      top: `calc(${box.y}% + ${gapInsetPx}px)`,
                      width: `calc(${box.w}% - ${gapInsetPx * 2}px)`,
                      height: `calc(${box.h}% - ${gapInsetPx * 2}px)`,
                    }}
                    onClick={(e) => handleZoneClick(box.zoneId, e.shiftKey || e.ctrlKey || e.metaKey)}
                    onPointerDown={
                      currentLayout.type === 'canvas'
                        ? (e) => startCanvasZoneDrag(box.zoneId, 'move', e)
                        : undefined
                    }
                    onKeyDown={(e) => {
                      if (e.key === 'Enter' || e.key === ' ') {
                        e.preventDefault();
                        handleZoneClick(box.zoneId, e.shiftKey || e.ctrlKey);
                      }
                    }}
                  >
                    {showZoneNumbers && (
                      <span className="zone-editor__zone-number">{box.displayNumber}</span>
                    )}
                    <span className="zone-editor__zone-dim">
                      {Math.round(box.w)}% x {Math.round(box.h)}%
                    </span>
                    {currentLayout.type === 'canvas' && (
                      <span
                        className="zone-editor__resize-handle"
                        aria-label={t('windowManager.resizeZoneAria', 'Resize zone')}
                        onPointerDown={(e) => startCanvasZoneDrag(box.zoneId, 'resize', e)}
                      >
                        <Maximize2 size={11} aria-hidden="true" />
                      </span>
                    )}
                  </div>
                );
              })}

              {/* 网格列分割手柄 */}
              {currentLayout.type === 'grid' &&
                colDividerOffsets.map((offset, idx) => (
                  <div
                    key={`col-div-${idx}`}
                    className="zone-editor__divider zone-editor__divider--col"
                    style={{ left: `${offset}%` }}
                    role="separator"
                    aria-orientation="vertical"
                    aria-label={t('windowManager.dragColDivider', 'Drag to resize column ratio')}
                    onPointerDown={(e) => startGridDividerDrag('col', idx, e)}
                  />
                ))}

              {/* 网格行分割手柄 */}
              {currentLayout.type === 'grid' &&
                rowDividerOffsets.map((offset, idx) => (
                  <div
                    key={`row-div-${idx}`}
                    className="zone-editor__divider zone-editor__divider--row"
                    style={{ top: `${offset}%` }}
                    role="separator"
                    aria-orientation="horizontal"
                    aria-label={t('windowManager.dragRowDivider', 'Drag to resize row ratio')}
                    onPointerDown={(e) => startGridDividerDrag('row', idx, e)}
                  />
                ))}
            </div>

            <div className="zone-editor__taskbar-mock" aria-hidden="true">
              <span className="zone-editor__taskbar-pill" />
              <span className="zone-editor__taskbar-pill" />
              <span className="zone-editor__taskbar-pill" />
            </div>
          </div>

          <p className="zone-editor__hint">
            {currentLayout.type === 'grid'
              ? t(
                  'windowManager.gridEditorHint',
                  'Drag split lines to resize tracks. Shift+Click adjacent zones to merge, or select a merged zone to split.',
                )
              : t(
                  'windowManager.canvasEditorHint',
                  'Drag any zone to reposition, pull its bottom-right handle to resize, or fine-tune percentages above.',
                )}
          </p>
        </div>
      )}
    </div>
  );
};
