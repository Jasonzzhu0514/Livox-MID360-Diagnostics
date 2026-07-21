import { listen } from "@tauri-apps/api/event";
import { Maximize2, Play, Square } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import type { PreviewNotice, PreviewState } from "./preview-types";
import { isTauriRuntime } from "./runtime";

type PointCloudFrameEvent = {
  runId: string;
  header: {
    seq?: number;
    data_type?: string;
    intensity_range?: [number, number];
  };
  pointsBase64: string;
};

type Props = {
  activeRunId: string | null;
  state: PreviewState;
  running: boolean;
  switching: boolean;
  stopping: boolean;
  error: string;
  notice: PreviewNotice;
  onFirstFrame: (runId: string) => void;
  onStart: () => void;
  onStop: () => void;
};

type Stats = {
  frames: number;
  points: number;
  dataType: string;
  seq: number;
};

const INITIAL_POINT_BUFFER_CAPACITY = 16_384;
const STATS_UPDATE_INTERVAL_MS = 250;

const INITIAL_STATS: Stats = {
  frames: 0,
  points: 0,
  dataType: "waiting",
  seq: 0,
};

const ODOMETRY_LOW = new THREE.Color(0x242424);
const ODOMETRY_HIGH = new THREE.Color(0xffffff);
const DEFAULT_INTENSITY_RANGE: [number, number] = [0, 255];

function createSquarePointSprite() {
  const size = 64;
  const canvas = document.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  const context = canvas.getContext("2d");
  if (!context) {
    throw new Error("Could not create the point sprite canvas.");
  }
  context.clearRect(0, 0, size, size);
  context.fillStyle = "#ffffff";
  context.fillRect(0, 0, size, size);

  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.NoColorSpace;
  texture.generateMipmaps = true;
  texture.minFilter = THREE.LinearMipmapLinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.needsUpdate = true;
  return texture;
}

function writeOdometryPointColor(colors: Float32Array, offset: number, intensity: number, intensityRange: [number, number]) {
  const lower = Number.isFinite(intensityRange[0]) ? intensityRange[0] : DEFAULT_INTENSITY_RANGE[0];
  const upper = Number.isFinite(intensityRange[1]) ? intensityRange[1] : DEFAULT_INTENSITY_RANGE[1];
  const value = Number.isFinite(intensity) ? intensity : lower;
  const normalized = Math.min(1, Math.max(0, (value - lower) / Math.max(0.0001, upper - lower)));
  const tone = Math.pow(normalized, 0.55);
  colors[offset] = ODOMETRY_LOW.r + (ODOMETRY_HIGH.r - ODOMETRY_LOW.r) * tone;
  colors[offset + 1] = ODOMETRY_LOW.g + (ODOMETRY_HIGH.g - ODOMETRY_LOW.g) * tone;
  colors[offset + 2] = ODOMETRY_LOW.b + (ODOMETRY_HIGH.b - ODOMETRY_LOW.b) * tone;
}

function previewStateLabel(state: PreviewState) {
  switch (state) {
    case "starting":
      return "Starting";
    case "discovering":
      return "Discovering";
    case "configuring":
      return "Configuring";
    case "waiting_data":
      return "Waiting data";
    case "streaming":
      return "Streaming";
    case "stopping":
      return "Stopping";
    case "stopped":
      return "Stopped";
    case "error":
      return "Error";
    default:
      return "Ready";
  }
}

function decodePointData(encoded: string) {
  try {
    const text = window.atob(encoded);
    const bytes = new Uint8Array(text.length);
    for (let index = 0; index < text.length; index += 1) {
      bytes[index] = text.charCodeAt(index);
    }
    return bytes.byteLength % (Float32Array.BYTES_PER_ELEMENT * 4) === 0 ? new Float32Array(bytes.buffer) : null;
  } catch {
    return null;
  }
}

export function PointCloudPreview({ activeRunId, state, running, switching, stopping, error, notice, onFirstFrame, onStart, onStop }: Props) {
  const mountRef = useRef<HTMLDivElement | null>(null);
  const cameraRef = useRef<THREE.PerspectiveCamera | null>(null);
  const controlsRef = useRef<OrbitControls | null>(null);
  const pointsRef = useRef<THREE.Points | null>(null);
  const positionsRef = useRef<Float32Array | null>(null);
  const colorsRef = useRef<Float32Array | null>(null);
  const positionAttributeRef = useRef<THREE.BufferAttribute | null>(null);
  const colorAttributeRef = useRef<THREE.BufferAttribute | null>(null);
  const renderRef = useRef<(() => void) | null>(null);
  const fpsFrameCountRef = useRef(0);
  const fpsLastSampleRef = useRef(0);
  const statsRef = useRef<Stats>(INITIAL_STATS);
  const statsLastUpdateRef = useRef(0);
  const statsUpdateTimerRef = useRef<number | null>(null);
  const firstFrameRunRef = useRef<string | null>(null);
  const [renderFps, setRenderFps] = useState(0);
  const [stats, setStats] = useState<Stats>(INITIAL_STATS);

  function ensurePointBufferCapacity(requiredPoints: number) {
    const cloud = pointsRef.current;
    const currentPositions = positionsRef.current;
    if (!cloud || !currentPositions) {
      return false;
    }

    const currentCapacity = currentPositions.length / 3;
    if (requiredPoints <= currentCapacity) {
      return true;
    }

    let nextCapacity = Math.max(INITIAL_POINT_BUFFER_CAPACITY, currentCapacity);
    while (nextCapacity < requiredPoints) {
      nextCapacity *= 2;
    }

    const positions = new Float32Array(nextCapacity * 3);
    const colors = new Float32Array(nextCapacity * 3);
    const positionAttribute = new THREE.BufferAttribute(positions, 3).setUsage(THREE.DynamicDrawUsage);
    const colorAttribute = new THREE.BufferAttribute(colors, 3).setUsage(THREE.DynamicDrawUsage);

    cloud.geometry.dispose();
    cloud.geometry.setAttribute("position", positionAttribute);
    cloud.geometry.setAttribute("color", colorAttribute);
    positionsRef.current = positions;
    colorsRef.current = colors;
    positionAttributeRef.current = positionAttribute;
    colorAttributeRef.current = colorAttribute;
    return true;
  }

  function publishStats() {
    const now = performance.now();
    const elapsed = now - statsLastUpdateRef.current;
    if (elapsed >= STATS_UPDATE_INTERVAL_MS) {
      if (statsUpdateTimerRef.current !== null) {
        window.clearTimeout(statsUpdateTimerRef.current);
        statsUpdateTimerRef.current = null;
      }
      statsLastUpdateRef.current = now;
      setStats(statsRef.current);
      return;
    }
    if (statsUpdateTimerRef.current === null) {
      statsUpdateTimerRef.current = window.setTimeout(() => {
        statsUpdateTimerRef.current = null;
        statsLastUpdateRef.current = performance.now();
        setStats(statsRef.current);
      }, STATS_UPDATE_INTERVAL_MS - elapsed);
    }
  }

  useEffect(() => {
    const mount = mountRef.current;
    if (!mount) return;

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x070b0c);

    const camera = new THREE.PerspectiveCamera(58, 1, 0.05, 500);
    camera.position.set(8, -10, 6);
    camera.up.set(0, 0, 1);
    camera.lookAt(0, 0, 0);
    cameraRef.current = camera;

    const renderer = new THREE.WebGLRenderer({ antialias: false, alpha: false, powerPreference: "high-performance" });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 1.5));
    mount.appendChild(renderer.domElement);

    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = false;
    controls.target.set(0, 0, 0);
    controlsRef.current = controls;

    const grid = new THREE.GridHelper(24, 24, 0x2f4d46, 0x1b2927);
    grid.rotation.x = Math.PI / 2;
    scene.add(grid);

    const axes = new THREE.AxesHelper(3);
    scene.add(axes);

    const geometry = new THREE.BufferGeometry();
    const positions = new Float32Array(INITIAL_POINT_BUFFER_CAPACITY * 3);
    const colors = new Float32Array(INITIAL_POINT_BUFFER_CAPACITY * 3);
    const positionAttribute = new THREE.BufferAttribute(positions, 3).setUsage(THREE.DynamicDrawUsage);
    const colorAttribute = new THREE.BufferAttribute(colors, 3).setUsage(THREE.DynamicDrawUsage);
    geometry.setAttribute("position", positionAttribute);
    geometry.setAttribute("color", colorAttribute);
    geometry.setDrawRange(0, 0);
    positionsRef.current = positions;
    colorsRef.current = colors;
    positionAttributeRef.current = positionAttribute;
    colorAttributeRef.current = colorAttribute;
    const pointSprite = createSquarePointSprite();
    const material = new THREE.PointsMaterial({
      size: 1,
      vertexColors: true,
      sizeAttenuation: false,
      map: pointSprite,
      alphaMap: pointSprite,
      transparent: true,
      opacity: 0.95,
      alphaTest: 0.36,
      depthWrite: false,
      depthTest: true,
      toneMapped: false,
    });
    const cloud = new THREE.Points(geometry, material);
    cloud.frustumCulled = false;
    pointsRef.current = cloud;
    scene.add(cloud);

    let disposed = false;
    fpsLastSampleRef.current = performance.now();
    fpsFrameCountRef.current = 0;
    const render = () => {
      if (disposed) return;
      const now = performance.now();
      fpsFrameCountRef.current += 1;
      const elapsed = now - fpsLastSampleRef.current;
      if (elapsed >= 1000) {
        setRenderFps(Math.round((fpsFrameCountRef.current * 1000) / elapsed));
        fpsFrameCountRef.current = 0;
        fpsLastSampleRef.current = now;
      }
      renderer.render(scene, camera);
    };
    renderRef.current = render;
    controls.addEventListener("change", render);

    const resize = () => {
      const rect = mount.getBoundingClientRect();
      const width = Math.max(1, rect.width);
      const height = Math.max(1, rect.height);
      renderer.setSize(width, height, false);
      camera.aspect = width / height;
      camera.updateProjectionMatrix();
      render();
    };
    const observer = new ResizeObserver(resize);
    observer.observe(mount);
    resize();

    return () => {
      disposed = true;
      observer.disconnect();
      controls.removeEventListener("change", render);
      renderRef.current = null;
      if (statsUpdateTimerRef.current !== null) {
        window.clearTimeout(statsUpdateTimerRef.current);
        statsUpdateTimerRef.current = null;
      }
      controls.dispose();
      geometry.dispose();
      material.dispose();
      pointSprite.dispose();
      renderer.dispose();
      renderer.domElement.remove();
      positionsRef.current = null;
      colorsRef.current = null;
      positionAttributeRef.current = null;
      colorAttributeRef.current = null;
      pointsRef.current = null;
    };
  }, []);

  useEffect(() => {
    if (!isTauriRuntime()) {
      return;
    }
    let disposed = false;
    let unlisten: (() => void) | null = null;
    void listen<PointCloudFrameEvent>("pointcloud://frame", (event) => {
      if (disposed) return;
      if (!activeRunId || event.payload.runId !== activeRunId) return;
      const raw = decodePointData(event.payload.pointsBase64);
      if (!raw) return;
      const incomingPoints = Math.floor(raw.length / 4);
      const cloud = pointsRef.current;
      if (!cloud || !ensurePointBufferCapacity(incomingPoints)) {
        return;
      }
      const positions = positionsRef.current;
      const colors = colorsRef.current;
      const positionAttribute = positionAttributeRef.current;
      const colorAttribute = colorAttributeRef.current;
      if (!positions || !colors || !positionAttribute || !colorAttribute) {
        return;
      }

      const intensityRange = event.payload.header.intensity_range ?? DEFAULT_INTENSITY_RANGE;
      for (let pointIndex = 0; pointIndex < incomingPoints; pointIndex += 1) {
        const rawOffset = pointIndex * 4;
        const pointOffset = pointIndex * 3;
        positions[pointOffset] = raw[rawOffset];
        positions[pointOffset + 1] = raw[rawOffset + 1];
        positions[pointOffset + 2] = raw[rawOffset + 2];
        writeOdometryPointColor(colors, pointOffset, raw[rawOffset + 3], intensityRange);
      }
      if (incomingPoints > 0) {
        positionAttribute.clearUpdateRanges();
        colorAttribute.clearUpdateRanges();
        positionAttribute.addUpdateRange(0, incomingPoints * 3);
        colorAttribute.addUpdateRange(0, incomingPoints * 3);
        positionAttribute.needsUpdate = true;
        colorAttribute.needsUpdate = true;
      }
      cloud.geometry.setDrawRange(0, incomingPoints);
      renderRef.current?.();
      if (incomingPoints > 0 && firstFrameRunRef.current !== activeRunId) {
        firstFrameRunRef.current = activeRunId;
        onFirstFrame(activeRunId);
      }

      const nextStats: Stats = {
        frames: statsRef.current.frames + 1,
        points: incomingPoints,
        dataType: event.payload.header.data_type ?? "raw",
        seq: event.payload.header.seq ?? statsRef.current.seq,
      };
      statsRef.current = nextStats;
      publishStats();
    })
      .then((dispose) => {
        if (disposed) {
          dispose();
        } else {
          unlisten = dispose;
        }
      })
      .catch(() => undefined);
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [activeRunId, onFirstFrame]);

  function clearCloud() {
    const cloud = pointsRef.current;
    if (cloud) {
      cloud.geometry.setDrawRange(0, 0);
    }
    renderRef.current?.();
    statsRef.current = INITIAL_STATS;
    if (statsUpdateTimerRef.current !== null) {
      window.clearTimeout(statsUpdateTimerRef.current);
      statsUpdateTimerRef.current = null;
    }
    statsLastUpdateRef.current = performance.now();
    setStats(INITIAL_STATS);
  }

  useEffect(() => {
    clearCloud();
    firstFrameRunRef.current = null;
  }, [activeRunId]);

  function resetCamera() {
    const camera = cameraRef.current;
    const controls = controlsRef.current;
    if (!camera || !controls) return;
    camera.position.set(8, -10, 6);
    controls.target.set(0, 0, 0);
    controls.update();
    renderRef.current?.();
  }

  return (
    <section className="pointcloud-view">
      <header className="pointcloud-toolbar">
        <div className="pointcloud-title">
          <h1>PointCloud Preview</h1>
          <span>{previewStateLabel(state)}</span>
        </div>
        {notice ? (
          <div className={`pointcloud-notice ${notice.tone}`} title={notice.hint ? `${notice.detail} ${notice.hint}` : notice.detail}>
            <strong>{notice.title}</strong>
            <span>{notice.detail}</span>
          </div>
        ) : (
          error && (
            <div className="pointcloud-notice error" title={error}>
              <strong>Preview failed</strong>
              <span>{error}</span>
            </div>
          )
        )}
        <div className="pointcloud-actions">
          <button className="secondary icon-button" title="Reset camera" onClick={resetCamera}>
            <Maximize2 size={17} />
          </button>
          {running ? (
            <button className="danger" disabled={stopping} onClick={onStop}>
              <Square size={17} />
              {stopping ? "Stopping" : "Stop"}
            </button>
          ) : (
            <button className="primary" disabled={switching} onClick={onStart}>
              <Play size={17} />
              {switching ? "Switching" : "Start Preview"}
            </button>
          )}
        </div>
      </header>

      <div className="pointcloud-stats">
        <div>
          <span>Frames</span>
          <strong>{stats.frames.toLocaleString()}</strong>
        </div>
        <div>
          <span>Points</span>
          <strong>{stats.points.toLocaleString()}</strong>
        </div>
      </div>

      <div className="pointcloud-layout">
        <div className="pointcloud-canvas-wrap">
          <div ref={mountRef} className="pointcloud-canvas" />
          <div className="pointcloud-hud">
            <span>FPS {renderFps}</span>
            <span>{previewStateLabel(state)}</span>
            <span>{stats.dataType}</span>
            <span>seq {stats.seq}</span>
          </div>
        </div>
      </div>
    </section>
  );
}
