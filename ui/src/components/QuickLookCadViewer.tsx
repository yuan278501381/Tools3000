import {
  useState,
  useEffect,
  useMemo,
  useRef,
  useCallback,
  type PointerEvent as ReactPointerEvent,
  type WheelEvent as ReactWheelEvent,
} from 'react';
import { useTranslation } from 'react-i18next';
import {
  Layers,
  Maximize2,
  Minus,
  Plus,
  Sun,
  Moon,
  Eye,
  EyeOff,
} from 'lucide-react';

export interface DxfLayerPayload {
  name: string;
  colorHex: string;
  adaptive?: boolean;
  count: number;
}

export interface DxfEntityPayload {
  /** 0=Line, 1=Polyline, 2=Circle, 3=Arc, 4=Text */
  k: number;
  l: string;
  c: string;
  a?: boolean;
  pts: number[];
  cl?: boolean;
  r?: number;
  sa?: number;
  ea?: number;
  th?: number;
  rot?: number;
  t?: string;
}

export interface DxfPreviewPayload {
  totalEntities: number;
  bounds: [number, number, number, number];
  layers: DxfLayerPayload[];
  entities: DxfEntityPayload[];
}

interface QuickLookCadViewerProps {
  dxfData: DxfPreviewPayload;
}

const MIN_CAD_ZOOM = 0.1;  // 10%
const MAX_CAD_ZOOM = 50.0; // 5000%

function resolveCadStrokeColor(colorHex: string, adaptive: boolean | undefined, darkCanvas: boolean): string {
  if (adaptive) {
    return darkCanvas ? '#f8fafc' : '#0f172a';
  }
  const upper = (colorHex || '').toUpperCase();
  if (!darkCanvas) {
    if (upper === '#FACC15' || upper === '#FFFF00') return '#b45309';
    if (upper === '#06B6D4' || upper === '#00FFFF') return '#0369a1';
    if (upper === '#22C55E' || upper === '#00FF00') return '#15803d';
    if (upper === '#CBD5E1') return '#475569';
    if (upper === '#F8FAFC' || upper === '#FFFFFF') return '#0f172a';
  }
  return colorHex || (darkCanvas ? '#f8fafc' : '#0f172a');
}

export function QuickLookCadViewer({ dxfData }: QuickLookCadViewerProps) {
  const { t } = useTranslation();
  const [darkCanvas, setDarkCanvas] = useState(true);
  const [zoom, setZoom] = useState(1);
  const [pan, setPan] = useState<{ x: number; y: number }>({ x: 0, y: 0 });
  const [isDragging, setIsDragging] = useState(false);
  const [layersOpen, setLayersOpen] = useState(false);
  const [hiddenLayers, setHiddenLayers] = useState<Set<string>>(() => new Set());
  const [viewportSize, setViewportSize] = useState<{ w: number; h: number }>({ w: 920, h: 560 });

  const containerRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const dragRef = useRef<{
    pointerId: number;
    startX: number;
    startY: number;
    originX: number;
    originY: number;
  } | null>(null);

  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    const updateSize = () => {
      const rect = el.getBoundingClientRect();
      const w = Math.max(240, Math.round(rect.width || el.clientWidth || 920));
      const h = Math.max(180, Math.round(rect.height || el.clientHeight || 560));
      setViewportSize(prev => (prev.w === w && prev.h === h ? prev : { w, h }));
    };
    updateSize();
    if (typeof ResizeObserver !== 'undefined') {
      const ro = new ResizeObserver(updateSize);
      ro.observe(el);
      return () => ro.disconnect();
    }
    window.addEventListener('resize', updateSize);
    return () => window.removeEventListener('resize', updateSize);
  }, []);

  const visibleLayerCount = useMemo(() => {
    let count = 0;
    for (const ly of dxfData.layers) {
      if (!hiddenLayers.has(ly.name)) count += 1;
    }
    return count;
  }, [dxfData.layers, hiddenLayers]);

  const handleFitView = useCallback(() => {
    setZoom(1);
    setPan({ x: 0, y: 0 });
  }, []);

  const applyZoomAtPoint = useCallback((nextZoomRaw: number, anchorX: number, anchorY: number) => {
    const clampedZoom = Math.min(MAX_CAD_ZOOM, Math.max(MIN_CAD_ZOOM, Number(nextZoomRaw.toFixed(4))));
    setZoom(prevZoom => {
      if (Math.abs(clampedZoom - prevZoom) < 1e-5) return prevZoom;
      const ratio = clampedZoom / prevZoom;
      const cx = viewportSize.w * 0.5;
      const cy = viewportSize.h * 0.5;
      setPan(prevPan => ({
        x: anchorX - cx - (anchorX - cx - prevPan.x) * ratio,
        y: anchorY - cy - (anchorY - cy - prevPan.y) * ratio,
      }));
      return clampedZoom;
    });
  }, [viewportSize.h, viewportSize.w]);

  const handleWheel = useCallback((e: ReactWheelEvent<HTMLCanvasElement>) => {
    e.preventDefault();
    const rect = e.currentTarget.getBoundingClientRect();
    const mx = e.clientX - rect.left;
    const my = e.clientY - rect.top;
    const factor = e.deltaY < 0 ? 1.18 : 1 / 1.18;
    applyZoomAtPoint(zoom * factor, mx, my);
  }, [applyZoomAtPoint, zoom]);

  const handlePointerDown = useCallback((e: ReactPointerEvent<HTMLCanvasElement>) => {
    if (e.button !== 0 && e.button !== 1) return;
    e.preventDefault();
    try {
      e.currentTarget.setPointerCapture?.(e.pointerId);
    } catch {
      // Ignore synthetic events in unit tests
    }
    dragRef.current = {
      pointerId: e.pointerId,
      startX: e.clientX,
      startY: e.clientY,
      originX: pan.x,
      originY: pan.y,
    };
    setIsDragging(true);
  }, [pan.x, pan.y]);

  const handlePointerMove = useCallback((e: ReactPointerEvent<HTMLCanvasElement>) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== e.pointerId) return;
    e.preventDefault();
    setPan({
      x: drag.originX + (e.clientX - drag.startX),
      y: drag.originY + (e.clientY - drag.startY),
    });
  }, []);

  const handlePointerEnd = useCallback((e: ReactPointerEvent<HTMLCanvasElement>) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== e.pointerId) return;
    try {
      e.currentTarget.releasePointerCapture?.(e.pointerId);
    } catch {
      // Ignore
    }
    dragRef.current = null;
    setIsDragging(false);
  }, []);

  const toggleLayer = useCallback((layerName: string) => {
    setHiddenLayers(prev => {
      const next = new Set(prev);
      if (next.has(layerName)) next.delete(layerName);
      else next.add(layerName);
      return next;
    });
  }, []);

  const handleShowAllLayers = useCallback(() => {
    setHiddenLayers(new Set());
  }, []);

  const handleHideAllLayers = useCallback(() => {
    setHiddenLayers(new Set(dxfData.layers.map(ly => ly.name)));
  }, [dxfData.layers]);

  // High-DPI hardware-accelerated Canvas 2D vector drawing
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    let ctx: CanvasRenderingContext2D | null;
    try {
      ctx = canvas.getContext('2d');
    } catch {
      ctx = null;
    }
    if (!ctx) return;

    const dpr = Math.min(window.devicePixelRatio || 1, 3);
    const w = viewportSize.w;
    const h = viewportSize.h;
    const targetPixelW = Math.max(1, Math.round(w * dpr));
    const targetPixelH = Math.max(1, Math.round(h * dpr));
    if (canvas.width !== targetPixelW || canvas.height !== targetPixelH) {
      canvas.width = targetPixelW;
      canvas.height = targetPixelH;
    }

    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.fillStyle = darkCanvas ? '#0b0f17' : '#ffffff';
    ctx.fillRect(0, 0, w, h);

    // Subtle engineering coordinate grid
    const gridStep = 48;
    ctx.beginPath();
    const offsetX = ((w * 0.5 + pan.x) % gridStep + gridStep) % gridStep;
    const offsetY = ((h * 0.5 + pan.y) % gridStep + gridStep) % gridStep;
    for (let gx = offsetX; gx < w; gx += gridStep) {
      ctx.moveTo(gx, 0);
      ctx.lineTo(gx, h);
    }
    for (let gy = offsetY; gy < h; gy += gridStep) {
      ctx.moveTo(0, gy);
      ctx.lineTo(w, gy);
    }
    ctx.strokeStyle = darkCanvas ? 'rgba(148, 163, 184, 0.08)' : 'rgba(15, 23, 42, 0.06)';
    ctx.lineWidth = 1;
    ctx.stroke();

    const [minX, minY, maxX, maxY] = dxfData.bounds;
    const worldW = Math.max(1e-3, maxX - minX);
    const worldH = Math.max(1e-3, maxY - minY);
    const padPx = 56;
    const baseFitScale = Math.min(
      Math.max(16, w - padPx) / worldW,
      Math.max(16, h - padPx) / worldH,
    );
    const scale = baseFitScale * zoom;
    const worldCx = (minX + maxX) * 0.5;
    const worldCy = (minY + maxY) * 0.5;
    const screenCx = w * 0.5 + pan.x;
    const screenCy = h * 0.5 + pan.y;

    const toScreenX = (wx: number) => screenCx + (wx - worldCx) * scale;
    const toScreenY = (wy: number) => screenCy - (wy - worldCy) * scale;

    // Group geometric strokes by resolved color to minimize state switches
    const colorBuckets = new Map<string, DxfEntityPayload[]>();
    const textEntities: DxfEntityPayload[] = [];

    for (const ent of dxfData.entities) {
      if (hiddenLayers.has(ent.l)) continue;
      if (ent.k === 4) {
        textEntities.push(ent);
        continue;
      }
      const stroke = resolveCadStrokeColor(ent.c, ent.a, darkCanvas);
      let bucket = colorBuckets.get(stroke);
      if (!bucket) {
        bucket = [];
        colorBuckets.set(stroke, bucket);
      }
      bucket.push(ent);
    }

    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.lineWidth = 1.15;

    for (const [strokeColor, list] of colorBuckets.entries()) {
      ctx.beginPath();
      for (const ent of list) {
        const pts = ent.pts;
        if (ent.k === 0 && pts.length >= 4) {
          ctx.moveTo(toScreenX(pts[0]), toScreenY(pts[1]));
          ctx.lineTo(toScreenX(pts[2]), toScreenY(pts[3]));
        } else if (ent.k === 1 && pts.length >= 4) {
          ctx.moveTo(toScreenX(pts[0]), toScreenY(pts[1]));
          for (let i = 2; i + 1 < pts.length; i += 2) {
            ctx.lineTo(toScreenX(pts[i]), toScreenY(pts[i + 1]));
          }
          if (ent.cl) {
            ctx.closePath();
          }
        } else if (ent.k === 2 && pts.length >= 2 && ent.r && ent.r > 0) {
          const sx = toScreenX(pts[0]);
          const sy = toScreenY(pts[1]);
          const sr = ent.r * scale;
          if (sr >= 0.4) {
            ctx.moveTo(sx + sr, sy);
            ctx.arc(sx, sy, sr, 0, Math.PI * 2);
          }
        } else if (ent.k === 3 && pts.length >= 2 && ent.r && ent.r > 0) {
          const sx = toScreenX(pts[0]);
          const sy = toScreenY(pts[1]);
          const sr = ent.r * scale;
          if (sr >= 0.4) {
            const saRad = -((ent.sa ?? 0) * Math.PI) / 180;
            const eaRad = -((ent.ea ?? 360) * Math.PI) / 180;
            ctx.moveTo(sx + sr * Math.cos(saRad), sy + sr * Math.sin(saRad));
            ctx.arc(sx, sy, sr, saRad, eaRad, true);
          }
        }
      }
      ctx.strokeStyle = strokeColor;
      ctx.stroke();
    }

    if (textEntities.length > 0) {
      const rootSans =
        (typeof window !== 'undefined' &&
          window.getComputedStyle(document.documentElement).getPropertyValue('--font-sans').trim()) ||
        'sans-serif';
      ctx.textBaseline = 'alphabetic';

      for (const ent of textEntities) {
        if (!ent.t || ent.pts.length < 2) continue;
        const pxHeight = (ent.th ?? 2.5) * scale;
        if (pxHeight < 3.5) continue; // Cull sub-pixel unreadable labels when zoomed far out
        const clampedPx = Math.min(96, Math.max(9, pxHeight));
        const sx = toScreenX(ent.pts[0]);
        const sy = toScreenY(ent.pts[1]);
        if (sx < -300 || sx > w + 300 || sy < -200 || sy > h + 200) continue;

        ctx.save();
        ctx.translate(sx, sy);
        if (ent.rot) {
          ctx.rotate((-ent.rot * Math.PI) / 180);
        }
        ctx.font = `500 ${clampedPx.toFixed(1)}px ${rootSans}`;
        ctx.fillStyle = resolveCadStrokeColor(ent.c, ent.a, darkCanvas);
        const lines = ent.t.split('\n');
        for (let idx = 0; idx < lines.length; idx += 1) {
          ctx.fillText(lines[idx], 0, idx * clampedPx * 1.25);
        }
        ctx.restore();
      }
    }
  }, [dxfData, darkCanvas, zoom, pan, hiddenLayers, viewportSize]);

  return (
    <div
      className={`ql-cad-viewer ${darkCanvas ? 'ql-cad-viewer--dark' : 'ql-cad-viewer--light'}`}
      ref={containerRef}
    >
      {/* 悬浮控制舱 */}
      <div className="ql-cad-toolbar" role="toolbar">
        <div className="ql-cad-tool-group">
          <button
            type="button"
            className="ql-cad-btn"
            onClick={() => applyZoomAtPoint(zoom / 1.25, viewportSize.w * 0.5, viewportSize.h * 0.5)}
            title={t('quicklook.zoomOut', 'Zoom Out')}
            aria-label={t('quicklook.zoomOut', 'Zoom Out')}
          >
            <Minus size={13} aria-hidden="true" />
          </button>
          <span className="ql-cad-zoom-readout">{Math.round(zoom * 100)}%</span>
          <button
            type="button"
            className="ql-cad-btn"
            onClick={() => applyZoomAtPoint(zoom * 1.25, viewportSize.w * 0.5, viewportSize.h * 0.5)}
            title={t('quicklook.zoomIn', 'Zoom In')}
            aria-label={t('quicklook.zoomIn', 'Zoom In')}
          >
            <Plus size={13} aria-hidden="true" />
          </button>
          <button
            type="button"
            className="ql-cad-btn"
            onClick={handleFitView}
            title={t('quicklook.cadFitView', 'Fit to View')}
          >
            <Maximize2 size={13} aria-hidden="true" />
            <span>{t('quicklook.cadFitView', 'Fit to View')}</span>
          </button>
        </div>

        <div className="ql-cad-tool-group">
          <button
            type="button"
            className="ql-cad-btn"
            onClick={() => setDarkCanvas(prev => !prev)}
            data-testid="ql-cad-bg-toggle"
          >
            {darkCanvas ? <Sun size={13} aria-hidden="true" /> : <Moon size={13} aria-hidden="true" />}
            <span>
              {darkCanvas
                ? t('quicklook.cadBgLight', 'Print White')
                : t('quicklook.cadBgDark', 'Classic Dark')}
            </span>
          </button>

          <button
            type="button"
            className={`ql-cad-btn ${layersOpen ? 'active' : ''}`}
            onClick={() => setLayersOpen(prev => !prev)}
            data-testid="ql-cad-layers-toggle"
          >
            <Layers size={13} aria-hidden="true" />
            <span>
              {t('quicklook.cadLayersBtn', 'Layers ({{visible}}/{{total}})', {
                visible: visibleLayerCount,
                total: dxfData.layers.length,
              })}
            </span>
          </button>
        </div>
      </div>

      {/* 可展开 CAD 图层面板 */}
      {layersOpen && (
        <aside className="ql-cad-layers-panel" data-testid="ql-cad-layers-panel">
          <div className="ql-cad-layers-header">
            <button type="button" className="ql-cad-layer-batch-btn" onClick={handleShowAllLayers}>
              {t('quicklook.cadShowAllLayers', 'Show All')}
            </button>
            <button type="button" className="ql-cad-layer-batch-btn" onClick={handleHideAllLayers}>
              {t('quicklook.cadHideAllLayers', 'Hide All')}
            </button>
          </div>
          <div className="ql-cad-layers-list">
            {dxfData.layers.map(layer => {
              const visible = !hiddenLayers.has(layer.name);
              const swatch = resolveCadStrokeColor(layer.colorHex, layer.adaptive, darkCanvas);
              return (
                <button
                  key={layer.name}
                  type="button"
                  className={`ql-cad-layer-item ${visible ? 'visible' : 'hidden'}`}
                  onClick={() => toggleLayer(layer.name)}
                >
                  <span
                    className="ql-cad-layer-swatch"
                    style={{ backgroundColor: swatch }}
                    aria-hidden="true"
                  />
                  <span className="ql-cad-layer-name" title={layer.name}>{layer.name}</span>
                  <span className="ql-cad-layer-count">{layer.count}</span>
                  {visible ? <Eye size={13} aria-hidden="true" /> : <EyeOff size={13} aria-hidden="true" />}
                </button>
              );
            })}
          </div>
        </aside>
      )}

      <canvas
        ref={canvasRef}
        className={`ql-cad-canvas ${isDragging ? 'dragging' : ''}`}
        onWheel={handleWheel}
        onDoubleClick={handleFitView}
        onPointerDown={handlePointerDown}
        onPointerMove={handlePointerMove}
        onPointerUp={handlePointerEnd}
        onPointerCancel={handlePointerEnd}
      />

      <div className="ql-cad-hud-footer">
        <span>{t('quicklook.cadPanZoomHint', 'Wheel to zoom · Drag to pan · Double-click to fit')}</span>
        <span>
          {t('quicklook.cadEntitiesSummary', '{{count}} entities', {
            count: dxfData.totalEntities,
          })}
        </span>
      </div>
    </div>
  );
}
