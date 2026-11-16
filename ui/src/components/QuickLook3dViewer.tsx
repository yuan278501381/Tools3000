import {
  useState,
  useEffect,
  useMemo,
  useRef,
  useCallback,
  type PointerEvent as ReactPointerEvent,
  type WheelEvent as ReactWheelEvent,
  type MouseEvent as ReactMouseEvent,
} from 'react';
import { useTranslation } from 'react-i18next';
import {
  Box,
  Grid3x3,
  RotateCcw,
  Minus,
  Plus,
} from 'lucide-react';

export interface Mesh3dPreviewPayload {
  formatLabel?: string;
  totalTriangles: number;
  sampledTriangles: number;
  dimensions: [number, number, number];
  positionsBase64?: string;
  positions?: number[];
}

interface QuickLook3dViewerProps {
  meshData: Mesh3dPreviewPayload;
}

const ISO_YAW = -Math.PI / 4;
const ISO_PITCH = 0.6154797; // atan(1 / sqrt(2)) ~ 35.264 deg
const MIN_3D_ZOOM = 0.25;
const MAX_3D_ZOOM = 10.0;

interface DecodedMeshBuffers {
  positions: Float32Array;
  normals: Float32Array;
  wirePositions: Float32Array;
  triCount: number;
}

function decodeMeshBuffers(meshData: Mesh3dPreviewPayload): DecodedMeshBuffers {
  let pos: Float32Array | null = null;

  if (meshData.positionsBase64) {
    try {
      const binStr = window.atob(meshData.positionsBase64);
      const byteLen = binStr.length - (binStr.length % 4);
      const bytes = new Uint8Array(byteLen);
      for (let i = 0; i < byteLen; i += 1) {
        bytes[i] = binStr.charCodeAt(i);
      }
      pos = new Float32Array(bytes.buffer);
    } catch {
      pos = null;
    }
  }
  if (!pos && Array.isArray(meshData.positions)) {
    pos = new Float32Array(meshData.positions);
  }
  if (!pos) {
    pos = new Float32Array(0);
  }

  const triCount = Math.floor(pos.length / 9);
  const trimmedPos = pos.length === triCount * 9 ? pos : pos.subarray(0, triCount * 9);
  const normals = new Float32Array(triCount * 9);
  const wirePositions = new Float32Array(triCount * 18);

  for (let t = 0; t < triCount; t += 1) {
    const b = t * 9;
    const x0 = trimmedPos[b], y0 = trimmedPos[b + 1], z0 = trimmedPos[b + 2];
    const x1 = trimmedPos[b + 3], y1 = trimmedPos[b + 4], z1 = trimmedPos[b + 5];
    const x2 = trimmedPos[b + 6], y2 = trimmedPos[b + 7], z2 = trimmedPos[b + 8];

    const ux = x1 - x0, uy = y1 - y0, uz = z1 - z0;
    const vx = x2 - x0, vy = y2 - y0, vz = z2 - z0;
    let nx = uy * vz - uz * vy;
    let ny = uz * vx - ux * vz;
    let nz = ux * vy - uy * vx;
    const len = Math.hypot(nx, ny, nz);
    if (len > 1e-8) {
      nx /= len;
      ny /= len;
      nz /= len;
    } else {
      nx = 0;
      ny = 0;
      nz = 1;
    }

    normals[b] = nx; normals[b + 1] = ny; normals[b + 2] = nz;
    normals[b + 3] = nx; normals[b + 4] = ny; normals[b + 5] = nz;
    normals[b + 6] = nx; normals[b + 7] = ny; normals[b + 8] = nz;

    const wb = t * 18;
    wirePositions[wb] = x0; wirePositions[wb + 1] = y0; wirePositions[wb + 2] = z0;
    wirePositions[wb + 3] = x1; wirePositions[wb + 4] = y1; wirePositions[wb + 5] = z1;
    wirePositions[wb + 6] = x1; wirePositions[wb + 7] = y1; wirePositions[wb + 8] = z1;
    wirePositions[wb + 9] = x2; wirePositions[wb + 10] = y2; wirePositions[wb + 11] = z2;
    wirePositions[wb + 12] = x2; wirePositions[wb + 13] = y2; wirePositions[wb + 14] = z2;
    wirePositions[wb + 15] = x0; wirePositions[wb + 16] = y0; wirePositions[wb + 17] = z0;
  }

  return { positions: trimmedPos, normals, wirePositions, triCount };
}

function formatDimValue(v: number): string {
  if (!Number.isFinite(v)) return '0';
  if (Math.abs(v) >= 100) return v.toFixed(1).replace(/\.0$/, '');
  return v.toFixed(2).replace(/\.?0+$/, '');
}

interface GlPipelineState {
  gl: WebGLRenderingContext;
  program: WebGLProgram;
  posBuffer: WebGLBuffer;
  normBuffer: WebGLBuffer;
  wireBuffer: WebGLBuffer;
  aPos: number;
  aNormal: number;
  uMvp: WebGLUniformLocation | null;
  uNormalMat: WebGLUniformLocation | null;
  uWirePass: WebGLUniformLocation | null;
  triCount: number;
}

function buildRotation3x3(yaw: number, pitch: number): Float32Array {
  // Orbit around Z-up / Y-up smoothly: first yaw around Y, then pitch around X
  const cy = Math.cos(yaw), sy = Math.sin(yaw);
  const cp = Math.cos(pitch), sp = Math.sin(pitch);
  // R = Rx(pitch) * Ry(yaw)
  return new Float32Array([
    cy, 0, sy,
    sp * sy, cp, -sp * cy,
    -cp * sy, sp, cp * cy,
  ]);
}

function buildMvp4x4(
  rot3: Float32Array,
  aspect: number,
  zoom: number,
  panX: number,
  panY: number,
  viewW: number,
  viewH: number,
): Float32Array {
  const fovRad = (38 * Math.PI) / 180;
  const f = 1.0 / Math.tan(fovRad * 0.5);
  const near = 0.1;
  const far = 50.0;
  const camDist = 3.4 / Math.max(0.05, zoom);

  const ndcPanX = (panX / Math.max(1, viewW)) * 2.0;
  const ndcPanY = -(panY / Math.max(1, viewH)) * 2.0;

  const sx = f / Math.max(0.1, aspect);
  const sy = f;
  const sz = (far + near) / (near - far);
  const tz = (2 * far * near) / (near - far);

  // Column-major 4x4 matrix: P * Translate(pan, -camDist) * R
  const r00 = rot3[0], r01 = rot3[1], r02 = rot3[2];
  const r10 = rot3[3], r11 = rot3[4], r12 = rot3[5];
  const r20 = rot3[6], r21 = rot3[7], r22 = rot3[8];

  // Camera space position: xc = r0*v, yc = r1*v, zc = r2*v - camDist
  // Clip space:
  // x_clip = sx * xc + ndcPanX * (-zc)
  // y_clip = sy * yc + ndcPanY * (-zc)
  // z_clip = sz * zc + tz
  // w_clip = -zc
  return new Float32Array([
    sx * r00 - ndcPanX * r20, sy * r10 - ndcPanY * r20, sz * r20, -r20,
    sx * r01 - ndcPanX * r21, sy * r11 - ndcPanY * r21, sz * r21, -r21,
    sx * r02 - ndcPanX * r22, sy * r12 - ndcPanY * r22, sz * r22, -r22,
    ndcPanX * camDist, ndcPanY * camDist, -sz * camDist + tz, camDist,
  ]);
}

export function QuickLook3dViewer({ meshData }: QuickLook3dViewerProps) {
  const { t } = useTranslation();
  const [wireframeOverlay, setWireframeOverlay] = useState(false);
  const [yaw, setYaw] = useState(ISO_YAW);
  const [pitch, setPitch] = useState(ISO_PITCH);
  const [zoom, setZoom] = useState(1);
  const [pan, setPan] = useState<{ x: number; y: number }>({ x: 0, y: 0 });
  const [isDragging, setIsDragging] = useState(false);
  const [viewportSize, setViewportSize] = useState<{ w: number; h: number }>({ w: 920, h: 560 });

  const containerRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const glStateRef = useRef<GlPipelineState | null>(null);
  const dragRef = useRef<{
    pointerId: number;
    mode: 'orbit' | 'pan';
    startX: number;
    startY: number;
    originYaw: number;
    originPitch: number;
    originPanX: number;
    originPanY: number;
  } | null>(null);

  const decoded = useMemo(() => decodeMeshBuffers(meshData), [meshData]);

  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    const measure = () => {
      const rect = el.getBoundingClientRect();
      const w = Math.max(240, Math.round(rect.width || el.clientWidth || 920));
      const h = Math.max(180, Math.round(rect.height || el.clientHeight || 560));
      setViewportSize(prev => (prev.w === w && prev.h === h ? prev : { w, h }));
    };
    measure();
    if (typeof ResizeObserver !== 'undefined') {
      const ro = new ResizeObserver(measure);
      ro.observe(el);
      return () => ro.disconnect();
    }
    window.addEventListener('resize', measure);
    return () => window.removeEventListener('resize', measure);
  }, []);

  // Initialize & upload WebGL buffers when decoded mesh changes
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    let gl: WebGLRenderingContext | null;
    try {
      gl = (canvas.getContext('webgl', { antialias: true, alpha: true }) ||
        canvas.getContext('experimental-webgl')) as WebGLRenderingContext | null;
    } catch {
      gl = null;
    }
    if (!gl) {
      glStateRef.current = null;
      return;
    }

    const vsSrc = `
      attribute vec3 aPos;
      attribute vec3 aNormal;
      uniform mat4 uMvp;
      uniform mat3 uNormalMat;
      varying vec3 vNormal;
      void main() {
        vNormal = normalize(uNormalMat * aNormal);
        gl_Position = uMvp * vec4(aPos, 1.0);
      }
    `;
    const fsSrc = `
      precision mediump float;
      varying vec3 vNormal;
      uniform float uWirePass;
      void main() {
        if (uWirePass > 0.5) {
          gl_FragColor = vec4(0.22, 0.74, 0.97, 0.58);
          return;
        }
        vec3 n = normalize(vNormal);
        vec3 keyDir = normalize(vec3(0.52, 0.68, 0.82));
        vec3 fillDir = normalize(vec3(-0.68, -0.35, 0.48));
        vec3 halfVec = normalize(keyDir + vec3(0.0, 0.0, 1.0));

        float keyDiff = max(dot(n, keyDir), 0.0) * 0.78 + abs(dot(n, keyDir)) * 0.22;
        float fillDiff = max(dot(n, fillDir), 0.0) * 0.65 + abs(dot(n, fillDir)) * 0.15;
        float spec = pow(abs(dot(n, halfVec)), 28.0) * 0.26;

        vec3 baseMetal = vec3(0.56, 0.65, 0.78);
        vec3 warmRim = vec3(0.32, 0.46, 0.66);
        vec3 color = baseMetal * (0.24 + keyDiff * 0.74) + warmRim * (fillDiff * 0.34) + vec3(spec);
        gl_FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
      }
    `;

    const compileShader = (type: number, source: string) => {
      const sh = gl!.createShader(type);
      if (!sh) return null;
      gl!.shaderSource(sh, source);
      gl!.compileShader(sh);
      if (!gl!.getShaderParameter(sh, gl!.COMPILE_STATUS)) {
        gl!.deleteShader(sh);
        return null;
      }
      return sh;
    };

    const vs = compileShader(gl.VERTEX_SHADER, vsSrc);
    const fs = compileShader(gl.FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) {
      if (vs) gl.deleteShader(vs);
      if (fs) gl.deleteShader(fs);
      glStateRef.current = null;
      return;
    }

    const program = gl.createProgram();
    if (!program) {
      gl.deleteShader(vs);
      gl.deleteShader(fs);
      glStateRef.current = null;
      return;
    }
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);
    gl.deleteShader(vs);
    gl.deleteShader(fs);

    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      gl.deleteProgram(program);
      glStateRef.current = null;
      return;
    }

    const posBuffer = gl.createBuffer();
    const normBuffer = gl.createBuffer();
    const wireBuffer = gl.createBuffer();
    if (!posBuffer || !normBuffer || !wireBuffer) {
      gl.deleteProgram(program);
      glStateRef.current = null;
      return;
    }

    gl.bindBuffer(gl.ARRAY_BUFFER, posBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, decoded.positions, gl.STATIC_DRAW);

    gl.bindBuffer(gl.ARRAY_BUFFER, normBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, decoded.normals, gl.STATIC_DRAW);

    gl.bindBuffer(gl.ARRAY_BUFFER, wireBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, decoded.wirePositions, gl.STATIC_DRAW);

    glStateRef.current = {
      gl,
      program,
      posBuffer,
      normBuffer,
      wireBuffer,
      aPos: gl.getAttribLocation(program, 'aPos'),
      aNormal: gl.getAttribLocation(program, 'aNormal'),
      uMvp: gl.getUniformLocation(program, 'uMvp'),
      uNormalMat: gl.getUniformLocation(program, 'uNormalMat'),
      uWirePass: gl.getUniformLocation(program, 'uWirePass'),
      triCount: decoded.triCount,
    };

    return () => {
      if (glStateRef.current) {
        const s = glStateRef.current;
        s.gl.deleteBuffer(s.posBuffer);
        s.gl.deleteBuffer(s.normBuffer);
        s.gl.deleteBuffer(s.wireBuffer);
        s.gl.deleteProgram(s.program);
        glStateRef.current = null;
      }
    };
  }, [decoded]);

  // Frame render (WebGL primary + software Canvas 2D fallback)
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const dpr = Math.min(window.devicePixelRatio || 1, 2.5);
    const w = viewportSize.w;
    const h = viewportSize.h;
    const pw = Math.max(1, Math.round(w * dpr));
    const ph = Math.max(1, Math.round(h * dpr));
    if (canvas.width !== pw || canvas.height !== ph) {
      canvas.width = pw;
      canvas.height = ph;
    }

    const rot3 = buildRotation3x3(yaw, pitch);
    const state = glStateRef.current;

    if (state && state.triCount > 0) {
      const { gl } = state;
      gl.viewport(0, 0, pw, ph);
      gl.clearColor(0.055, 0.07, 0.1, 1.0);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      gl.enable(gl.DEPTH_TEST);
      gl.depthFunc(gl.LEQUAL);

      gl.useProgram(state.program);
      const mvp = buildMvp4x4(rot3, w / Math.max(1, h), zoom, pan.x, pan.y, w, h);
      gl.uniformMatrix4fv(state.uMvp, false, mvp);
      // Transpose row-major rot3 into column-major WebGL mat3
      gl.uniformMatrix3fv(
        state.uNormalMat,
        false,
        new Float32Array([
          rot3[0], rot3[3], rot3[6],
          rot3[1], rot3[4], rot3[7],
          rot3[2], rot3[5], rot3[8],
        ]),
      );

      // Pass 1: Dual-light Shaded Solid Mesh
      gl.enable(gl.POLYGON_OFFSET_FILL);
      gl.polygonOffset(1.0, 1.0);
      gl.uniform1f(state.uWirePass, 0.0);

      gl.bindBuffer(gl.ARRAY_BUFFER, state.posBuffer);
      if (state.aPos >= 0) {
        gl.enableVertexAttribArray(state.aPos);
        gl.vertexAttribPointer(state.aPos, 3, gl.FLOAT, false, 0, 0);
      }
      gl.bindBuffer(gl.ARRAY_BUFFER, state.normBuffer);
      if (state.aNormal >= 0) {
        gl.enableVertexAttribArray(state.aNormal);
        gl.vertexAttribPointer(state.aNormal, 3, gl.FLOAT, false, 0, 0);
      }
      gl.drawArrays(gl.TRIANGLES, 0, state.triCount * 3);
      gl.disable(gl.POLYGON_OFFSET_FILL);

      // Pass 2: Crisp Sci-Fi Wireframe Overlay
      if (wireframeOverlay) {
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
        gl.uniform1f(state.uWirePass, 1.0);
        if (state.aNormal >= 0) {
          gl.disableVertexAttribArray(state.aNormal);
        }
        gl.bindBuffer(gl.ARRAY_BUFFER, state.wireBuffer);
        if (state.aPos >= 0) {
          gl.enableVertexAttribArray(state.aPos);
          gl.vertexAttribPointer(state.aPos, 3, gl.FLOAT, false, 0, 0);
        }
        gl.drawArrays(gl.LINES, 0, state.triCount * 6);
        gl.disable(gl.BLEND);
      }
      return;
    }

    // Software Canvas 2D fallback (headless / RDP without WebGL)
    let ctx2d: CanvasRenderingContext2D | null;
    try {
      ctx2d = canvas.getContext('2d');
    } catch {
      ctx2d = null;
    }
    if (!ctx2d) return;

    ctx2d.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx2d.fillStyle = '#0e121a';
    ctx2d.fillRect(0, 0, w, h);

    const triCount = decoded.triCount;
    if (triCount === 0) return;

    const scalePx = Math.min(w, h) * 0.36 * zoom;
    const cx = w * 0.5 + pan.x;
    const cy = h * 0.5 + pan.y;

    const indices = new Int32Array(triCount);
    const depths = new Float32Array(triCount);
    const pos = decoded.positions;
    const nrm = decoded.normals;

    for (let i = 0; i < triCount; i += 1) {
      indices[i] = i;
      const b = i * 9;
      const mx = (pos[b] + pos[b + 3] + pos[b + 6]) / 3;
      const my = (pos[b + 1] + pos[b + 4] + pos[b + 7]) / 3;
      const mz = (pos[b + 2] + pos[b + 5] + pos[b + 8]) / 3;
      depths[i] = rot3[6] * mx + rot3[7] * my + rot3[8] * mz;
    }
    const sorted = Array.from(indices).sort((a, b) => depths[a] - depths[b]);

    for (const i of sorted) {
      const b = i * 9;
      const p0x = cx + (rot3[0] * pos[b] + rot3[1] * pos[b + 1] + rot3[2] * pos[b + 2]) * scalePx;
      const p0y = cy - (rot3[3] * pos[b] + rot3[4] * pos[b + 1] + rot3[5] * pos[b + 2]) * scalePx;
      const p1x = cx + (rot3[0] * pos[b + 3] + rot3[1] * pos[b + 4] + rot3[2] * pos[b + 5]) * scalePx;
      const p1y = cy - (rot3[3] * pos[b + 3] + rot3[4] * pos[b + 4] + rot3[5] * pos[b + 5]) * scalePx;
      const p2x = cx + (rot3[0] * pos[b + 6] + rot3[1] * pos[b + 7] + rot3[2] * pos[b + 8]) * scalePx;
      const p2y = cy - (rot3[3] * pos[b + 6] + rot3[4] * pos[b + 7] + rot3[5] * pos[b + 8]) * scalePx;

      const rnx = rot3[0] * nrm[b] + rot3[1] * nrm[b + 1] + rot3[2] * nrm[b + 2];
      const rny = rot3[3] * nrm[b] + rot3[4] * nrm[b + 1] + rot3[5] * nrm[b + 2];
      const rnz = rot3[6] * nrm[b] + rot3[7] * nrm[b + 1] + rot3[8] * nrm[b + 2];
      const shade = Math.min(1, 0.28 + Math.abs(rnx * 0.45 + rny * 0.58 + rnz * 0.68) * 0.72);
      const r = Math.round(138 * shade);
      const g = Math.round(162 * shade);
      const bl = Math.round(196 * shade);

      ctx2d.beginPath();
      ctx2d.moveTo(p0x, p0y);
      ctx2d.lineTo(p1x, p1y);
      ctx2d.lineTo(p2x, p2y);
      ctx2d.closePath();
      ctx2d.fillStyle = `rgb(${r}, ${g}, ${bl})`;
      ctx2d.fill();
      if (wireframeOverlay) {
        ctx2d.strokeStyle = 'rgba(56, 189, 248, 0.55)';
        ctx2d.lineWidth = 0.8;
        ctx2d.stroke();
      }
    }
  }, [decoded, yaw, pitch, zoom, pan, wireframeOverlay, viewportSize]);

  const handleResetIso = useCallback(() => {
    setYaw(ISO_YAW);
    setPitch(ISO_PITCH);
    setZoom(1);
    setPan({ x: 0, y: 0 });
  }, []);

  const handlePointerDown = useCallback((e: ReactPointerEvent<HTMLCanvasElement>) => {
    if (e.button !== 0 && e.button !== 1 && e.button !== 2) return;
    e.preventDefault();
    try {
      e.currentTarget.setPointerCapture?.(e.pointerId);
    } catch {
      // Ignore synthetic events in tests
    }
    const mode: 'orbit' | 'pan' = e.button === 2 || e.button === 1 || e.shiftKey ? 'pan' : 'orbit';
    dragRef.current = {
      pointerId: e.pointerId,
      mode,
      startX: e.clientX,
      startY: e.clientY,
      originYaw: yaw,
      originPitch: pitch,
      originPanX: pan.x,
      originPanY: pan.y,
    };
    setIsDragging(true);
  }, [pan.x, pan.y, pitch, yaw]);

  const handlePointerMove = useCallback((e: ReactPointerEvent<HTMLCanvasElement>) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== e.pointerId) return;
    e.preventDefault();
    const dx = e.clientX - drag.startX;
    const dy = e.clientY - drag.startY;
    if (drag.mode === 'orbit') {
      setYaw(drag.originYaw + dx * 0.0085);
      setPitch(Math.max(-Math.PI * 0.49, Math.min(Math.PI * 0.49, drag.originPitch + dy * 0.0085)));
    } else {
      setPan({
        x: drag.originPanX + dx,
        y: drag.originPanY + dy,
      });
    }
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

  const handleWheel = useCallback((e: ReactWheelEvent<HTMLCanvasElement>) => {
    e.preventDefault();
    const factor = e.deltaY < 0 ? 1.15 : 1 / 1.15;
    setZoom(prev => Math.min(MAX_3D_ZOOM, Math.max(MIN_3D_ZOOM, Number((prev * factor).toFixed(3)))));
  }, []);

  const handleContextMenu = useCallback((e: ReactMouseEvent<HTMLCanvasElement>) => {
    e.preventDefault();
  }, []);

  const [dimX, dimY, dimZ] = meshData.dimensions;

  return (
    <div className="ql-3d-viewer" ref={containerRef}>
      {/* 左上控制栏 */}
      <div className="ql-cad-toolbar" role="toolbar">
        <div className="ql-cad-tool-group">
          <button
            type="button"
            className={`ql-cad-btn ${!wireframeOverlay ? 'active' : ''}`}
            onClick={() => setWireframeOverlay(false)}
            data-testid="ql-3d-shaded-btn"
          >
            <Box size={13} aria-hidden="true" />
            <span>{t('quicklook.meshShadedMode', 'Shaded')}</span>
          </button>
          <button
            type="button"
            className={`ql-cad-btn ${wireframeOverlay ? 'active' : ''}`}
            onClick={() => setWireframeOverlay(true)}
            data-testid="ql-3d-wireframe-btn"
          >
            <Grid3x3 size={13} aria-hidden="true" />
            <span>{t('quicklook.meshWireframeMode', 'Wireframe')}</span>
          </button>
        </div>

        <div className="ql-cad-tool-group">
          <button
            type="button"
            className="ql-cad-btn"
            onClick={() => setZoom(prev => Math.max(MIN_3D_ZOOM, Number((prev / 1.2).toFixed(3))))}
            title={t('quicklook.zoomOut', 'Zoom Out')}
            aria-label={t('quicklook.zoomOut', 'Zoom Out')}
          >
            <Minus size={13} aria-hidden="true" />
          </button>
          <span className="ql-cad-zoom-readout">{Math.round(zoom * 100)}%</span>
          <button
            type="button"
            className="ql-cad-btn"
            onClick={() => setZoom(prev => Math.min(MAX_3D_ZOOM, Number((prev * 1.2).toFixed(3))))}
            title={t('quicklook.zoomIn', 'Zoom In')}
            aria-label={t('quicklook.zoomIn', 'Zoom In')}
          >
            <Plus size={13} aria-hidden="true" />
          </button>
          <button
            type="button"
            className="ql-cad-btn"
            onClick={handleResetIso}
            data-testid="ql-3d-reset-btn"
          >
            <RotateCcw size={13} aria-hidden="true" />
            <span>{t('quicklook.meshResetView', 'Reset Iso')}</span>
          </button>
        </div>
      </div>

      {/* 右上角模型三角面数与物理尺寸包围盒 */}
      <div className="ql-3d-stats-badge" data-testid="ql-3d-stats-badge">
        <span className="ql-3d-stat-pill">
          {t('quicklook.meshTrianglesCount', '{{count}} tris', { count: meshData.totalTriangles || 0 })}
        </span>
        <span className="ql-3d-stat-divider" aria-hidden="true">·</span>
        <span className="ql-3d-stat-pill">
          {t('quicklook.meshBoundingBox', 'Size {{x}} × {{y}} × {{z}}', {
            x: formatDimValue(dimX),
            y: formatDimValue(dimY),
            z: formatDimValue(dimZ),
          })}
        </span>
      </div>

      <canvas
        ref={canvasRef}
        className={`ql-3d-canvas ${isDragging ? 'dragging' : ''}`}
        onPointerDown={handlePointerDown}
        onPointerMove={handlePointerMove}
        onPointerUp={handlePointerEnd}
        onPointerCancel={handlePointerEnd}
        onWheel={handleWheel}
        onDoubleClick={handleResetIso}
        onContextMenu={handleContextMenu}
      />

      <div className="ql-cad-hud-footer">
        <span>{t('quicklook.meshOrbitHint', 'Left-drag orbit · Right-drag pan · Wheel zoom')}</span>
        {meshData.formatLabel && <span>{meshData.formatLabel}</span>}
      </div>
    </div>
  );
}
