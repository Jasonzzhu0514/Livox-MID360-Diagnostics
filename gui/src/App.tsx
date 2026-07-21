import { invoke } from "@tauri-apps/api/tauri";
import { listen } from "@tauri-apps/api/event";
import {
  Activity,
  Cable,
  CheckCircle2,
  ChevronDown,
  CircleAlert,
  Gauge,
  Info,
  Loader2,
  Pause,
  Play,
  ScanLine,
  Radar,
  RefreshCw,
  Settings2,
  Square,
  TerminalSquare,
} from "lucide-react";
import { lazy, Suspense, useCallback, useEffect, useMemo, useRef, useState } from "react";
import packageMetadata from "../package.json";
import { AboutDialog } from "./AboutDialog";
import { ErrorBoundary } from "./ErrorBoundary";
import type { PreviewNotice, PreviewState } from "./preview-types";
import { isTauriRuntime, tauriRequiredMessage } from "./runtime";

const APP_VERSION = `v${packageMetadata.version}`;

const PointCloudPreview = lazy(() =>
  import("./PointCloudPreview").then((module) => ({ default: module.PointCloudPreview })),
);

type CliInfo = {
  path: string | null;
  exists: boolean;
  candidates: string[];
  version: string | null;
};

type RunOptions = {
  iface?: string;
  timeoutSec?: number;
};

type LidarNetworkState = "ready" | "needsSetup" | "noLink" | "unavailable";

type LidarNetworkStatus = {
  state: LidarNetworkState;
  interfaces: string[];
  iface: string | null;
  ip: string | null;
  prefix: number | null;
  proposedIp: string | null;
  temporary: boolean;
  detail: string;
};

type CommandResult = {
  ok: boolean;
  code: number | null;
  stdout: string;
  stderr: string;
  elapsedMs: number;
  summary: Record<string, string>;
};

type PanelMode = "diagnostics" | "pointcloud" | "logs";
type RunKind = "monitor" | "preview";

type OutputEvent = {
  runId: string;
  stream: "stdout" | "stderr" | "system";
  line: string;
  summary?: Record<string, string>;
};

type CompleteEvent = {
  runId: string;
  result: CommandResult | null;
  error: string | null;
};

const EMPTY_RESULT: CommandResult = {
  ok: false,
  code: null,
  stdout: "",
  stderr: "",
  elapsedMs: 0,
  summary: {},
};

function compactVersion(version: string | null | undefined) {
  const match = version?.match(/\b(\d+\.\d+\.\d+(?:[-+][A-Za-z0-9.-]+)?)\b/);
  return match ? `v${match[1]}` : "Version unavailable";
}

function textValue(value: string | undefined, fallback = "unavailable") {
  const normalized = value?.trim();
  return normalized && normalized.toUpperCase() !== "N/A" ? normalized : fallback;
}

function compactFieldValue(value: string | undefined) {
  const normalized = value?.trim();
  if (!normalized) return value;
  const lowered = normalized.toLowerCase();
  if (lowered.includes("no mid360 lidar found")) return "Not found";
  if (lowered.includes("stopped by user")) return "Stopped";
  return value;
}

function modeLabel(mode: PanelMode | RunKind) {
  if (mode === "diagnostics" || mode === "monitor") return "Diagnostics";
  if (mode === "pointcloud" || mode === "preview") return "PointCloud Preview";
  return "Logs";
}

function previewStateFromLine(line: string): PreviewState | null {
  const normalized = line.trim().toLowerCase();
  if (!normalized) return null;
  if (normalized.startsWith("error:")) return "error";
  if (normalized.includes("preview stopped by user") || normalized.includes("preview process stopped by user")) {
    return "stopping";
  }
  if (normalized.includes("preview: discovery candidates") || normalized.includes("preview: listening on")) {
    return "discovering";
  }
  if (normalized.includes("preview: found lidar") || normalized.includes("preview: config=")) {
    return "configuring";
  }
  if (normalized.includes("preview: streaming point cloud frames")) {
    return "waiting_data";
  }
  if (normalized.includes("preview: stopped")) {
    return "stopped";
  }
  return null;
}

function previewNoticeFromLine(line: string): PreviewNotice {
  const trimmed = line.trim();
  const normalized = trimmed.toLowerCase();
  if (!normalized) return null;

  const listening = trimmed.match(/^preview:\s+listening on\s+(.+?)\s+for\s+[\d.]+s/i);
  if (listening) {
    return {
      tone: "info",
      title: "Scanning for MID360",
      detail: `Listening on ${listening[1]}.`,
      hint: "Keep the sensor powered on and connected by Ethernet while discovery is running.",
    };
  }

  if (normalized.startsWith("preview: discovery candidates")) {
    return {
      tone: "info",
      title: "Scanning network interfaces",
      detail: trimmed.replace(/^preview:\s*/i, ""),
      hint: "If the expected Ethernet adapter is missing, select the interface manually in Diagnostics settings.",
    };
  }

  const bindFailure = trimmed.match(/^preview:\s+warn\s+failed to add\s+(.+?)\s+to\s+(.+?);\s+run:\s+(.+)$/i);
  if (bindFailure) {
    return {
      tone: "warning",
      title: "Auto-bind needs permission",
      detail: `Could not add ${bindFailure[1]} to ${bindFailure[2]}. Discovery will continue, but this interface may not see the lidar.`,
      hint: bindFailure[3],
    };
  }

  if (normalized.includes("preview: found lidar")) {
    return {
      tone: "info",
      title: "MID360 found",
      detail: trimmed.replace(/^preview:\s*/i, ""),
      hint: "Configuring point transmission.",
    };
  }

  if (normalized.includes("preview: streaming point cloud frames")) {
    return {
      tone: "info",
      title: "Waiting for point data",
      detail: "The lidar is configured and the preview is waiting for the first point frame.",
    };
  }

  if (normalized.includes("no mid360 lidar found by sdk discovery")) {
    return {
      tone: "error",
      title: "No MID360 lidar found",
      detail: "SDK discovery finished without receiving a MID360 broadcast.",
      hint: "Check sensor power, Ethernet link, host IP/subnet, firewall rules, and whether the selected interface is the lidar adapter.",
    };
  }

  if (normalized.includes("livoxlidarsdkinit failed")) {
    return {
      tone: "error",
      title: "Livox SDK failed to start",
      detail: trimmed.replace(/^error:\s*/i, ""),
      hint: "Close other Livox tools that may be using the SDK ports, then start preview again.",
    };
  }

  if (normalized.startsWith("error:")) {
    return {
      tone: "error",
      title: "Preview failed",
      detail: trimmed.replace(/^error:\s*/i, ""),
      hint: "Open Logs for the full backend output.",
    };
  }

  return null;
}

function previewNoticeFromError(error: string): PreviewNotice {
  const detail = error.trim();
  if (!detail) return null;
  return {
    tone: "error",
    title: "Preview failed",
    detail,
    hint: "Open Logs for the full backend output.",
  };
}

function nextPreviewNotice(current: PreviewNotice, incoming: PreviewNotice): PreviewNotice {
  if (!incoming) return current;
  if (!current) return incoming;
  if (incoming.tone === "error") return incoming;
  if (incoming.tone === "warning" && current.tone === "info") return incoming;
  if (incoming.tone === "info" && current.tone === "warning") {
    return incoming.title === "MID360 found" || incoming.title === "Waiting for point data" ? incoming : current;
  }
  return incoming;
}

function isStoppedResult(result: CommandResult | null) {
  return result?.summary.status === "Stopped by user";
}

function productField(product: string | undefined, key: string) {
  if (!product) return undefined;
  const match = product.match(new RegExp(`${key}:\\s*(.*?)(?=\\s+[A-Za-z][A-Za-z0-9_]*:|$)`));
  return match?.[1]?.trim();
}

function deviceModel(devType: string | undefined, product: string | undefined) {
  const numericType = Number(devType);
  if (numericType === 9) return "MID360";
  if (numericType === 35) return "MID360S";
  return productField(product, "DevType");
}

function statusTone(result: CommandResult | null) {
  if (!result) return "idle";
  if (isStoppedResult(result)) return "idle";
  return result.ok ? "ok" : "warn";
}

function runKindForMode(mode: PanelMode): RunKind | null {
  if (mode === "diagnostics") return "monitor";
  if (mode === "pointcloud") return "preview";
  return null;
}

function statusText(mode: PanelMode, result: CommandResult | null, busy: RunKind | null, paused: boolean) {
  const activeBusy = mode === "logs" ? busy : busy === runKindForMode(mode) ? busy : null;
  const activePaused = mode === "logs" ? paused : paused && activeBusy !== null;
  if (mode === "logs") {
    if (activePaused) return `${modeLabel(activeBusy ?? "monitor")} is paused`;
    if (activeBusy) return `${modeLabel(activeBusy)} is running`;
    if (!result) return "No command has run yet";
    if (isStoppedResult(result)) return "Latest command was stopped";
    return result.ok ? "Latest command completed successfully" : result.summary.status ?? "Latest command needs attention";
  }
  if (activePaused) {
    return `${modeLabel(mode)} is paused`;
  }
  if (activeBusy) {
    return `${modeLabel(mode)} is running`;
  }
  if (!result) {
    return `${modeLabel(mode)} has not run yet`;
  }
  if (result.ok) {
    return "Diagnostics completed";
  }
  if (isStoppedResult(result)) {
    return "Diagnostics stopped";
  }
  return result.summary.status ?? "Diagnostics failed";
}

function StatusBanner({
  mode,
  result,
  busy,
  paused,
}: {
  mode: PanelMode;
  result: CommandResult | null;
  busy: RunKind | null;
  paused: boolean;
}) {
  const activeBusy = mode === "logs" ? busy : busy === runKindForMode(mode) ? busy : null;
  const activePaused = mode === "logs" ? paused : paused && activeBusy !== null;
  const running = activeBusy !== null;
  const tone = activePaused ? "paused" : running ? "running" : statusTone(result);
  const label = activePaused ? "Paused" : running ? "Running" : isStoppedResult(result) ? "Stopped" : result ? (result.ok ? "Passed" : "Failed") : "Waiting";
  return (
    <section className={`status-banner ${tone}`}>
      <div className="status-title">
        {tone === "ok" ? <CheckCircle2 size={20} /> : tone === "warn" ? <CircleAlert size={20} /> : <Settings2 size={20} />}
        <span>{label}</span>
      </div>
      <strong>{statusText(mode, result, activeBusy, activePaused)}</strong>
    </section>
  );
}

function Field({ label, value }: { label: string; value: string | undefined }) {
  const displayValue = textValue(compactFieldValue(value));
  const fullValue = textValue(value);
  return (
    <div className="field" title={`${label}: ${fullValue}`}>
      <span>{label}</span>
      <strong>{displayValue}</strong>
    </div>
  );
}

function MetricCard({
  title,
  icon,
  children,
  className = "",
}: {
  title: string;
  icon: React.ReactNode;
  children: React.ReactNode;
  className?: string;
}) {
  return (
    <section className={`metric-card${className ? ` ${className}` : ""}`}>
      <div className="card-title">
        {icon}
        <h2>{title}</h2>
      </div>
      <div className="card-body">{children}</div>
    </section>
  );
}

function StateCard({ result }: { result: CommandResult | null }) {
  return (
    <MetricCard title="State" icon={<TerminalSquare size={18} />} className="span-5 state-card">
      <Field label="Work" value={result?.summary.work} />
      <Field label="Point Send" value={result?.summary.point_send} />
      <Field label="IMU Enable" value={result?.summary.imu_enable} />
      <Field label="Type" value={result?.summary.type} />
      <Field label="Exit" value={result?.code === null || result?.code === undefined ? undefined : String(result.code)} />
      <Field label="Elapsed" value={result ? `${result.elapsedMs} ms` : undefined} />
      <Field label="Info Age" value={result?.summary.info_age} />
    </MetricCard>
  );
}

function RawLog({ result }: { result: CommandResult | null }) {
  const logRef = useRef<HTMLPreElement | null>(null);
  const content = result ? sanitizeDiagnosticsOutput([result.stdout, result.stderr].filter(Boolean).join("\n")) : "";

  useEffect(() => {
    const node = logRef.current;
    if (!node) {
      return;
    }
    node.scrollTop = node.scrollHeight;
  }, [content]);

  return (
    <pre className="raw-log" ref={logRef}>
      {content || "No command output yet."}
    </pre>
  );
}

function createRunId(kind: RunKind) {
  return `${kind}-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

function appendLine(
  result: CommandResult | null,
  stream: OutputEvent["stream"],
  line: string,
  summaryDelta: Record<string, string> = {},
): CommandResult {
  const base = result ?? EMPTY_RESULT;
  const text = stream === "system" ? `[system] ${line}` : line;
  const summary = Object.keys(summaryDelta).length > 0 ? { ...base.summary, ...summaryDelta } : base.summary;
  if (stream === "stderr") {
    return {
      ...base,
      summary,
      stderr: `${base.stderr}${base.stderr ? "\n" : ""}${text}`,
    };
  }
  return {
    ...base,
    summary,
    stdout: `${base.stdout}${base.stdout ? "\n" : ""}${text}`,
  };
}

function sanitizeDiagnosticsOutput(output: string) {
  const lines = output.split(/\r?\n/);
  const filtered: string[] = [];
  let skippingConfigCandidates = false;

  for (const line of lines) {
    if (line.includes("可更新的 MID360 配置文件")) {
      skippingConfigCandidates = true;
      continue;
    }

    if (skippingConfigCandidates) {
      const trimmed = line.trim();
      if (
        trimmed === "" ||
        trimmed.startsWith("[") ||
        trimmed.startsWith("未发现带有雷达 IP 的配置文件") ||
        trimmed.startsWith("其它低优先级候选已折叠") ||
        /^(\d+)[.)\]]/.test(trimmed) ||
        /^[-*]/.test(trimmed) ||
        trimmed.includes("current=") ||
        trimmed.includes("external/") ||
        trimmed.includes("MID360_config") ||
        trimmed.includes("mid360_config")
      ) {
        continue;
      }
      skippingConfigCandidates = false;
    }

    const displayLine = formatDiagnosticsLine(line);
    if (displayLine) {
      filtered.push(displayLine);
    }
  }

  return filtered.join("\n").trim();
}

function formatDiagnosticsLine(line: string) {
  const trimmed = line.trim();
  if (!trimmed) return "";

  if (trimmed.startsWith("[system] $ ")) {
    return "Diagnostics command started.";
  }
  if (trimmed === "[system] starting diagnostics...") {
    return "Starting diagnostics...";
  }
  if (trimmed === "[system] stop requested") {
    return "Stop requested.";
  }
  if (trimmed === "[system] monitoring paused") {
    return "Monitoring paused.";
  }
  if (trimmed === "[system] monitoring resumed") {
    return "Monitoring resumed.";
  }
  if (trimmed.startsWith("discovery: Livox-SDK2 scan candidates:")) {
    return "Scanning for MID360 devices...";
  }
  if (trimmed.startsWith("discovery: found lidar")) {
    return "Device discovered.";
  }
  if (trimmed === "stopped" || trimmed.startsWith("stopped.")) {
    return "Diagnostics stopped.";
  }
  if (trimmed.startsWith("elapsed=")) {
    return "Sample received.";
  }
  if (trimmed.includes("=")) {
    return "";
  }
  return line;
}

function isCompletionMarker(line: string) {
  const trimmed = line.trim();
  return trimmed === "stopped" || trimmed.startsWith("stopped.");
}

function softComplete(result: CommandResult): CommandResult {
  return {
    ...result,
    ok: true,
    code: result.code ?? 0,
  };
}

function mergeResult(current: CommandResult | null, incoming: CommandResult): CommandResult {
  if (!current) return incoming;
  return {
    ...incoming,
    stdout: incoming.stdout || current.stdout,
    stderr: incoming.stderr || current.stderr,
    summary: {
      ...current.summary,
      ...incoming.summary,
    },
  };
}

function backendState(cliInfo: CliInfo | null) {
  if (!isTauriRuntime()) return "preview";
  if (!cliInfo) return "unchecked";
  return cliInfo.exists ? "ready" : "missing";
}

function lidarNetworkLabel(status: LidarNetworkStatus | null) {
  if (!status) return "Checking Ethernet...";
  if (status.state === "ready") {
    const address = status.ip ? `${status.ip}/${status.prefix ?? 24}` : "192.168.1.x";
    return `${status.temporary ? "Temporary" : "Ready"} · ${status.iface ?? "Ethernet"} · ${address}`;
  }
  if (status.state === "needsSetup") {
    return `Setup needed · ${status.iface ?? "Ethernet"} · add ${status.proposedIp ? `${status.proposedIp}/24` : "a 192.168.1.x address"}`;
  }
  if (status.state === "noLink") {
    return status.iface ? `No Ethernet link · ${status.iface}` : "No Ethernet adapter";
  }
  return "Network check unavailable";
}

function NetworkSetupField({
  status,
  loading,
  commandBusy,
  onConfigure,
  onRelease,
}: {
  status: LidarNetworkStatus | null;
  loading: boolean;
  commandBusy: boolean;
  onConfigure: () => void;
  onRelease: () => void;
}) {
  const state = status?.state ?? "unavailable";
  const icon = loading ? (
    <Loader2 className="spin" size={15} />
  ) : state === "ready" ? (
    <CheckCircle2 size={15} />
  ) : state === "noLink" ? (
    <Cable size={15} />
  ) : (
    <CircleAlert size={15} />
  );
  const canConfigure = status?.state === "needsSetup" && Boolean(status.iface && status.proposedIp);
  const canRelease = status?.state === "ready" && status.temporary;

  return (
    <div className="network-setup-field">
      <span>Lidar Network</span>
      <div className={`network-setup-status ${state} ${loading ? "loading" : ""}`} title={status?.detail} aria-live="polite">
        {icon}
        <strong>{loading ? "Checking Ethernet..." : lidarNetworkLabel(status)}</strong>
        {canConfigure && (
          <button type="button" disabled={loading || commandBusy} onClick={onConfigure}>
            Configure
          </button>
        )}
        {canRelease && (
          <button type="button" disabled={loading || commandBusy} onClick={onRelease}>
            Release
          </button>
        )}
      </div>
    </div>
  );
}

function ControlStrip({
  iface,
  timeoutSec,
  networkStatus,
  networkBusy,
  commandBusy,
  onIface,
  onTimeout,
  onConfigureNetwork,
  onReleaseNetwork,
  onCheck,
}: {
  iface: string;
  timeoutSec: number;
  networkStatus: LidarNetworkStatus | null;
  networkBusy: boolean;
  commandBusy: boolean;
  onIface: (value: string) => void;
  onTimeout: (value: number) => void;
  onConfigureNetwork: () => void;
  onReleaseNetwork: () => void;
  onCheck: () => void;
}) {
  return (
    <section className="control-strip diagnostics-controls">
      <label>
        <span>Interface</span>
        <div className="select-control">
          <select value={iface} disabled={networkBusy} onChange={(event) => onIface(event.target.value)} aria-label="Ethernet interface">
            <option value="">Auto</option>
            {(networkStatus?.interfaces ?? []).map((name) => (
              <option key={name} value={name}>
                {name}
              </option>
            ))}
          </select>
          <ChevronDown size={16} aria-hidden="true" />
        </div>
      </label>
      <label>
        <span>Discovery Timeout</span>
        <input
          type="number"
          min={1}
          step={1}
          value={timeoutSec}
          onChange={(event) => onTimeout(Number(event.target.value))}
        />
      </label>
      <NetworkSetupField
        status={networkStatus}
        loading={networkBusy}
        commandBusy={commandBusy}
        onConfigure={onConfigureNetwork}
        onRelease={onReleaseNetwork}
      />
      <button className="secondary" disabled={networkBusy} title={!isTauriRuntime() ? tauriRequiredMessage() : undefined} onClick={onCheck}>
        <RefreshCw size={17} />
        Refresh
      </button>
    </section>
  );
}

function ActionBar({
  busy,
  stopping,
  paused,
  onMonitor,
  onPause,
  onResume,
  onStop,
}: {
  busy: RunKind | null;
  stopping: boolean;
  paused: boolean;
  onMonitor: () => void;
  onPause: () => void;
  onResume: () => void;
  onStop: () => void;
}) {
  return (
    <section className="action-row">
      <button className="primary alt" disabled={busy !== null} title={!isTauriRuntime() ? tauriRequiredMessage() : undefined} onClick={onMonitor}>
        {busy === "monitor" ? <Loader2 className="spin" size={18} /> : <Play size={18} />}
        Run Diagnostics
      </button>
      {busy === "monitor" && (
        <>
          <button className="secondary" disabled={stopping} onClick={paused ? onResume : onPause}>
            {paused ? <Play size={17} /> : <Pause size={17} />}
            {paused ? "Resume" : "Pause"}
          </button>
          <button className="danger" disabled={stopping} onClick={onStop}>
            {stopping ? <Loader2 className="spin" size={18} /> : <Square size={17} />}
            Stop
          </button>
        </>
      )}
    </section>
  );
}

function WorkbenchHeader({
  mode,
  title,
  result,
  busy,
  paused,
  iface,
  timeoutSec,
  networkStatus,
  networkBusy,
  stopping,
  onIface,
  onTimeout,
  onConfigureNetwork,
  onReleaseNetwork,
  onCheck,
  onMonitor,
  onPause,
  onResume,
  onStop,
}: {
  mode: PanelMode;
  title: string;
  result: CommandResult | null;
  busy: RunKind | null;
  paused: boolean;
  iface: string;
  timeoutSec: number;
  networkStatus: LidarNetworkStatus | null;
  networkBusy: boolean;
  stopping: boolean;
  onIface: (value: string) => void;
  onTimeout: (value: number) => void;
  onConfigureNetwork: () => void;
  onReleaseNetwork: () => void;
  onCheck: () => void;
  onMonitor: () => void;
  onPause: () => void;
  onResume: () => void;
  onStop: () => void;
}) {
  return (
    <section className="workbench-header">
      <div className="workbench-title">
        <div>
          <h1>{title}</h1>
        </div>
        <ActionBar busy={busy} stopping={stopping} paused={paused} onMonitor={onMonitor} onPause={onPause} onResume={onResume} onStop={onStop} />
      </div>
      <StatusBanner mode={mode} result={result} busy={busy} paused={paused} />
      <ControlStrip
        iface={iface}
        timeoutSec={timeoutSec}
        networkStatus={networkStatus}
        networkBusy={networkBusy}
        commandBusy={busy !== null}
        onIface={onIface}
        onTimeout={onTimeout}
        onConfigureNetwork={onConfigureNetwork}
        onReleaseNetwork={onReleaseNetwork}
        onCheck={onCheck}
      />
    </section>
  );
}

function MonitorCards({ result }: { result: CommandResult | null }) {
  const product = result?.summary.product;
  return (
    <section className="grid monitor-grid">
      <MetricCard title="Device" icon={<Activity size={18} />} className="span-3">
        <Field label="Lidar IP" value={result?.summary.ip} />
        <Field label="Serial" value={result?.summary.sn} />
        <Field label="Model" value={deviceModel(result?.summary.dev_type, product)} />
        <Field label="Build" value={productField(product, "BuildTime")} />
        <Field label="Link" value={result?.summary.link ?? result?.summary.status} />
      </MetricCard>
      <MetricCard title="Thermal" icon={<Gauge size={18} />} className="span-3">
        <Field label="Core Temp" value={result?.summary.core_temp} />
        <Field label="Env Temp" value={result?.summary.env_temp} />
        <Field label="Health" value={result?.summary.health} />
        <Field label="Diag" value={result?.summary.diag} />
      </MetricCard>
      <MetricCard title="Streams" icon={<Cable size={18} />} className="span-6">
        <Field label="Points" value={result?.summary.points} />
        <Field label="Point Packets" value={result?.summary.point_packets} />
        <Field label="IMU" value={result?.summary.imu} />
        <Field label="Point Mbps" value={result?.summary.point_mbps} />
        <Field label="Frame" value={result?.summary.frame} />
        <Field label="UDP" value={result?.summary.udp} />
      </MetricCard>
      <MetricCard title="Network" icon={<Cable size={18} />} className="span-4">
        <Field label="Point Endpoint" value={result?.summary.net_point} />
        <Field label="IMU Endpoint" value={result?.summary.net_imu} />
        <Field label="State Endpoint" value={result?.summary.net_state} />
        <Field label="Mask" value={result?.summary.mask} />
        <Field label="Gateway" value={result?.summary.gateway} />
      </MetricCard>
      <MetricCard title="Firmware" icon={<TerminalSquare size={18} />} className="span-3">
        <Field label="Firmware" value={result?.summary.fw} />
        <Field label="Hardware" value={result?.summary.hw} />
        <Field label="Time Sync" value={result?.summary.time_sync} />
        <Field label="Info Source" value={result?.summary.info_src} />
      </MetricCard>
      <StateCard result={result} />
    </section>
  );
}

export default function App() {
  const [mode, setMode] = useState<PanelMode>("diagnostics");
  const [aboutOpen, setAboutOpen] = useState(false);
  const [cliInfo, setCliInfo] = useState<CliInfo | null>(null);
  const [iface, setIface] = useState("");
  const [timeoutSec, setTimeoutSec] = useState(5);
  const [networkStatus, setNetworkStatus] = useState<LidarNetworkStatus | null>(null);
  const [networkBusy, setNetworkBusy] = useState(false);
  const [busy, setBusy] = useState<RunKind | null>(null);
  const [stoppingRunId, setStoppingRunId] = useState<string | null>(null);
  const [pausedRunId, setPausedRunId] = useState<string | null>(null);
  const [monitorResult, setMonitorResult] = useState<CommandResult | null>(null);
  const [previewResult, setPreviewResult] = useState<CommandResult | null>(null);
  const [latestRunKind, setLatestRunKind] = useState<RunKind | null>(null);
  const [previewState, setPreviewState] = useState<PreviewState>("idle");
  const [previewNotice, setPreviewNotice] = useState<PreviewNotice>(null);
  const [error, setError] = useState("");
  const activeRunRef = useRef<{ runId: string; kind: RunKind } | null>(null);
  const latestMonitorRunRef = useRef<string | null>(null);
  const latestPreviewRunRef = useRef<string | null>(null);
  const previewFirstFrameRunRef = useRef<string | null>(null);

  const latestResult = latestRunKind === "monitor" ? monitorResult : latestRunKind === "preview" ? previewResult : null;

  const options: RunOptions = useMemo(
    () => ({
      iface: iface.trim() || undefined,
      timeoutSec,
    }),
    [iface, timeoutSec],
  );

  const backend = backendState(cliInfo);
  const paused = busy !== null && pausedRunId === activeRunRef.current?.runId;
  const closeAbout = useCallback(() => setAboutOpen(false), []);
  const handlePreviewFirstFrame = useCallback((runId: string) => {
    if (previewFirstFrameRunRef.current === runId) {
      return;
    }
    previewFirstFrameRunRef.current = runId;
    setPreviewState("streaming");
    setPreviewNotice(null);
  }, []);

  function appendRunLine(kind: RunKind, stream: OutputEvent["stream"], line: string, summary?: Record<string, string>) {
    if (kind === "monitor") {
      setMonitorResult((current) => appendLine(current, stream, line, summary));
      return;
    }
    setPreviewResult((current) => appendLine(current, stream, line, summary));
  }

  async function refreshCliInfo(options: { quiet?: boolean } = {}) {
    if (!options.quiet) {
      setError("");
    }
    if (!isTauriRuntime()) {
      if (!options.quiet) {
        setError(tauriRequiredMessage());
      }
      return;
    }
    try {
      const info = await invoke<CliInfo>("get_cli_info");
      setCliInfo(info);
    } catch (err) {
      setError(String(err));
    }
  }

  async function refreshLidarNetwork(options: { quiet?: boolean; selectedIface?: string } = {}) {
    if (!isTauriRuntime()) {
      const status: LidarNetworkStatus = {
        state: "unavailable",
        interfaces: [],
        iface: null,
        ip: null,
        prefix: null,
        proposedIp: null,
        temporary: false,
        detail: tauriRequiredMessage(),
      };
      setNetworkStatus(status);
      return status;
    }

    try {
      const selectedIface = options.selectedIface ?? iface;
      const status = await invoke<LidarNetworkStatus>("get_lidar_network_status", {
        iface: selectedIface.trim() || null,
      });
      setNetworkStatus(status);
      return status;
    } catch (err) {
      const detail = String(err);
      const status: LidarNetworkStatus = {
        state: "unavailable",
        interfaces: [],
        iface: null,
        ip: null,
        prefix: null,
        proposedIp: null,
        temporary: false,
        detail,
      };
      setNetworkStatus(status);
      if (!options.quiet) {
        setError(detail);
      }
      return status;
    }
  }

  async function checkEnvironment() {
    setError("");
    setNetworkBusy(true);
    try {
      await Promise.all([refreshCliInfo({ quiet: true }), refreshLidarNetwork({ quiet: true })]);
    } finally {
      setNetworkBusy(false);
    }
  }

  async function selectInterface(value: string) {
    setIface(value);
    setError("");
    setNetworkBusy(true);
    try {
      await refreshLidarNetwork({ quiet: true, selectedIface: value });
    } finally {
      setNetworkBusy(false);
    }
  }

  async function configureLidarNetwork() {
    if (!networkStatus?.iface || !networkStatus.proposedIp || !isTauriRuntime()) {
      return;
    }
    setError("");
    setNetworkBusy(true);
    try {
      const status = await invoke<LidarNetworkStatus>("configure_lidar_network", {
        iface: networkStatus.iface,
        ip: networkStatus.proposedIp,
      });
      setNetworkStatus(status);
    } catch (err) {
      setError(String(err));
      await refreshLidarNetwork({ quiet: true });
    } finally {
      setNetworkBusy(false);
    }
  }

  async function releaseLidarNetwork() {
    if (!isTauriRuntime()) {
      return;
    }
    setError("");
    setNetworkBusy(true);
    try {
      await invoke<void>("release_lidar_network");
      await refreshLidarNetwork({ quiet: true });
    } catch (err) {
      setError(String(err));
    } finally {
      setNetworkBusy(false);
    }
  }

  async function ensureLidarNetwork(kind: RunKind) {
    if (!isTauriRuntime()) {
      return true;
    }
    setNetworkBusy(true);
    const status = await refreshLidarNetwork({ quiet: true });
    setNetworkBusy(false);
    if (status.state === "ready") {
      return true;
    }

    const title = status.state === "needsSetup" ? "Network setup required" : status.state === "noLink" ? "Ethernet link required" : "Network check failed";
    setError(`${title}: ${status.detail}`);
    if (kind === "preview") {
      setPreviewState("error");
      setPreviewNotice({
        tone: status.state === "unavailable" ? "error" : "warning",
        title,
        detail: status.detail,
        hint: status.state === "needsSetup" ? "Open Diagnostics and select Configure under Lidar Network." : undefined,
      });
    }
    return false;
  }

  useEffect(() => {
    void refreshCliInfo({ quiet: true });
    void refreshLidarNetwork({ quiet: true });
  }, []);

  async function stopActiveRun() {
    const active = activeRunRef.current;
    if (!active || !isTauriRuntime()) {
      return;
    }
    setStoppingRunId(active.runId);
    setPausedRunId(null);
    if (active.kind === "preview") {
      setPreviewState("stopping");
      setPreviewNotice({
        tone: "info",
        title: "Stopping preview",
        detail: "Stopping the point cloud process.",
      });
    }
    appendRunLine(active.kind, "system", "stop requested");
    try {
      const found = await invoke<boolean>("stop_run", { runId: active.runId });
      if (!found) {
        setError("Active command was already finished.");
        setStoppingRunId(null);
        if (active.kind === "preview" && latestPreviewRunRef.current === active.runId) {
          latestPreviewRunRef.current = null;
          setPreviewState("stopped");
          setPreviewNotice({
            tone: "info",
            title: "Preview stopped",
            detail: "The point cloud process has already finished.",
          });
        }
      }
    } catch (err) {
      setError(String(err));
      setStoppingRunId(null);
      if (active.kind === "preview") {
        setPreviewState("error");
        setPreviewNotice(previewNoticeFromError(String(err)));
      }
    }
  }

  async function pauseActiveRun() {
    const active = activeRunRef.current;
    if (!active || !isTauriRuntime()) {
      return;
    }
    try {
      const found = await invoke<boolean>("pause_run", { runId: active.runId });
      if (!found) {
        setError("Active command was already finished.");
        return;
      }
      setPausedRunId(active.runId);
      appendRunLine(active.kind, "system", `${modeLabel(active.kind)} paused`);
    } catch (err) {
      setError(String(err));
    }
  }

  async function resumeActiveRun() {
    const active = activeRunRef.current;
    if (!active || !isTauriRuntime()) {
      return;
    }
    try {
      const found = await invoke<boolean>("resume_run", { runId: active.runId });
      if (!found) {
        setError("Active command was already finished.");
        return;
      }
      setPausedRunId(null);
      appendRunLine(active.kind, "system", `${modeLabel(active.kind)} resumed`);
    } catch (err) {
      setError(String(err));
    }
  }

  async function runMonitor() {
    if (busy) {
      setError(`${modeLabel(busy)} is running. Stop it before starting Diagnostics.`);
      return;
    }
    if (!(await ensureLidarNetwork("monitor"))) {
      return;
    }
    const runId = createRunId("monitor");
    activeRunRef.current = { runId, kind: "monitor" };
    latestMonitorRunRef.current = runId;
    setLatestRunKind("monitor");
    setBusy("monitor");
    setStoppingRunId(null);
    setPausedRunId(null);
    setError("");
    setMode("diagnostics");
    setMonitorResult({
      ...EMPTY_RESULT,
      stdout: "[system] starting diagnostics...",
    });
    if (!isTauriRuntime()) {
      setMonitorResult({ ...EMPTY_RESULT, stderr: tauriRequiredMessage() });
      setError(tauriRequiredMessage());
      setBusy(null);
      activeRunRef.current = null;
      return;
    }
    const unlisten = await listen<OutputEvent>("diagnostics://output", (event) => {
      if (event.payload.runId !== runId) {
        return;
      }
      if (latestMonitorRunRef.current !== runId) {
        return;
      }
      setMonitorResult((current) => appendLine(current, event.payload.stream, event.payload.line, event.payload.summary));
      if (isCompletionMarker(event.payload.line) && activeRunRef.current?.runId === runId) {
        activeRunRef.current = null;
        setPausedRunId(null);
        setMonitorResult((current) => softComplete(current ?? EMPTY_RESULT));
        setBusy(null);
      }
    });
    const unlistenComplete = await listen<CompleteEvent>("diagnostics://complete", (event) => {
      if (event.payload.runId !== runId) {
        return;
      }
      if (latestMonitorRunRef.current !== runId) {
        unlisten();
        unlistenComplete();
        return;
      }
      if (event.payload.result) {
        setMonitorResult((current) => mergeResult(current, event.payload.result as CommandResult));
      }
      if (event.payload.error) {
        setMonitorResult((current) => appendLine(current, "stderr", event.payload.error ?? "unknown error"));
        setError(event.payload.error);
      }
      if (activeRunRef.current?.runId === runId) {
        activeRunRef.current = null;
      }
      setBusy(null);
      setStoppingRunId(null);
      setPausedRunId(null);
      unlisten();
      unlistenComplete();
    });
    try {
      await invoke<void>("run_monitor", { runId, options });
    } catch (err) {
      setMonitorResult((current) => appendLine(current, "stderr", String(err)));
      setError(String(err));
      unlisten();
      unlistenComplete();
      setBusy(null);
      setStoppingRunId(null);
      setPausedRunId(null);
      activeRunRef.current = null;
    }
  }

  async function runPreview() {
    if (busy && activeRunRef.current?.kind !== "preview") {
      await switchToPointCloudPreview();
      return;
    }
    if (busy === "preview") {
      return;
    }
    if (!(await ensureLidarNetwork("preview"))) {
      return;
    }
    const runId = createRunId("preview");
    activeRunRef.current = { runId, kind: "preview" };
    latestPreviewRunRef.current = runId;
    previewFirstFrameRunRef.current = null;
    setLatestRunKind("preview");
    setBusy("preview");
    setPreviewState("starting");
    setPreviewNotice({
      tone: "info",
      title: "Starting preview",
      detail: "Starting Livox SDK discovery.",
      hint: "The preview will scan available Ethernet interfaces for MID360 broadcasts.",
    });
    setStoppingRunId(null);
    setPausedRunId(null);
    setError("");
    setMode("pointcloud");
    setPreviewResult({
      ...EMPTY_RESULT,
      stdout: "[system] starting point cloud preview...",
    });
    if (!isTauriRuntime()) {
      setError(tauriRequiredMessage());
      setPreviewNotice(previewNoticeFromError(tauriRequiredMessage()));
      setBusy(null);
      setPreviewState("error");
      activeRunRef.current = null;
      return;
    }
    const unlistenOutput = await listen<OutputEvent>("diagnostics://output", (event) => {
      if (event.payload.runId !== runId || latestPreviewRunRef.current !== runId) {
        return;
      }
      const nextPreviewState = previewStateFromLine(event.payload.line);
      if (nextPreviewState) {
        setPreviewState(nextPreviewState);
      }
      const nextNotice = previewNoticeFromLine(event.payload.line);
      if (nextNotice) {
        setPreviewNotice((current) => nextPreviewNotice(current, nextNotice));
      }
      setPreviewResult((current) => appendLine(current, event.payload.stream, event.payload.line, event.payload.summary));
    });
    const unlistenComplete = await listen<CompleteEvent>("diagnostics://complete", (event) => {
      if (event.payload.runId !== runId) {
        return;
      }
      if (latestPreviewRunRef.current !== runId) {
        unlistenOutput();
        unlistenComplete();
        return;
      }
      if (event.payload.result) {
        setPreviewResult((current) => mergeResult(current, event.payload.result as CommandResult));
      }
      if (event.payload.error) {
        setPreviewResult((current) => appendLine(current, "stderr", event.payload.error ?? "unknown error"));
        setError(event.payload.error);
        setPreviewNotice(previewNoticeFromError(event.payload.error));
        setPreviewState("error");
      } else if (event.payload.result?.ok || isStoppedResult(event.payload.result ?? null)) {
        setPreviewState("stopped");
        setPreviewNotice({
          tone: "info",
          title: "Preview stopped",
          detail: isStoppedResult(event.payload.result ?? null) ? "Point cloud preview was stopped by user." : "Point cloud preview finished.",
        });
      } else {
        setPreviewState("error");
        const status = event.payload.result?.summary.status;
        if (status) {
          setPreviewNotice((current) => nextPreviewNotice(current, previewNoticeFromLine(`ERROR: ${status}`) ?? previewNoticeFromError(status)));
        } else {
          setPreviewNotice((current) =>
            current?.tone === "error"
              ? current
              : {
                  tone: "error",
                  title: "Preview failed",
                  detail: "The point cloud process exited before streaming data.",
                  hint: "Open Logs for the full backend output.",
                },
          );
        }
      }
      if (activeRunRef.current?.runId === runId) {
        activeRunRef.current = null;
      }
      setBusy(null);
      setStoppingRunId(null);
      setPausedRunId(null);
      unlistenOutput();
      unlistenComplete();
    });
    try {
      await invoke<void>("run_preview", { runId, options });
    } catch (err) {
      setPreviewResult((current) => appendLine(current, "stderr", String(err)));
      setError(String(err));
      setPreviewNotice(previewNoticeFromError(String(err)));
      unlistenOutput();
      unlistenComplete();
      setBusy(null);
      setStoppingRunId(null);
      setPausedRunId(null);
      setPreviewState("error");
      activeRunRef.current = null;
    }
  }

  async function switchToPointCloudPreview() {
    setMode("pointcloud");
    const active = activeRunRef.current;
    if (!active || active.kind === "preview") {
      return;
    }

    setError("");
    setPreviewState("stopping");
    setPreviewNotice({
      tone: "info",
      title: "Switching to PointCloud Preview",
      detail: `Stopping ${modeLabel(active.kind)} before starting the point cloud stream.`,
    });
    await stopActiveRun();
  }

  useEffect(() => {
    if (mode === "pointcloud" && !busy && !latestPreviewRunRef.current) {
      void runPreview();
    }
  }, [mode, busy]);

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div className="brand">
          <div className="brand-mark">
            <Radar size={25} strokeWidth={2.3} aria-hidden="true" />
          </div>
          <div>
            <h1>Livox MID360</h1>
            <span>Diagnostics GUI</span>
          </div>
        </div>
        <nav>
          <button className={mode === "diagnostics" ? "active" : ""} onClick={() => setMode("diagnostics")}>
            <Gauge size={18} />
            Diagnostics
          </button>
          <button className={mode === "pointcloud" ? "active" : ""} onClick={() => void switchToPointCloudPreview()}>
            <ScanLine size={18} />
            Preview
          </button>
          <button className={mode === "logs" ? "active" : ""} onClick={() => setMode("logs")}>
            <TerminalSquare size={18} />
            Logs
          </button>
        </nav>
        <button
          type="button"
          className="sidebar-status sidebar-about-trigger"
          onClick={() => setAboutOpen(true)}
          aria-haspopup="dialog"
          aria-label="About Livox MID360 Diagnostics"
          title="About Livox MID360 Diagnostics"
        >
          <div className="terminal-status-left">
            <span className={`status-led ${paused ? "paused" : busy ? "running" : backend}`}></span>
            <strong>livox-mid360-diagnostics</strong>
          </div>
          <span className="sidebar-about-meta">
            <span>{compactVersion(cliInfo?.version)}</span>
            <Info size={14} aria-hidden="true" />
          </span>
        </button>
      </aside>

      <main>
        {mode === "diagnostics" ? (
          <>
            <WorkbenchHeader
              mode="diagnostics"
              title="Diagnostics"
              result={monitorResult}
              busy={busy}
              paused={paused}
              iface={iface}
              timeoutSec={timeoutSec}
              networkStatus={networkStatus}
              networkBusy={networkBusy}
              stopping={stoppingRunId !== null}
              onIface={selectInterface}
              onTimeout={setTimeoutSec}
              onConfigureNetwork={configureLidarNetwork}
              onReleaseNetwork={releaseLidarNetwork}
              onCheck={checkEnvironment}
              onMonitor={runMonitor}
              onPause={pauseActiveRun}
              onResume={resumeActiveRun}
              onStop={stopActiveRun}
            />

            {error && <div className="error-banner">{error}</div>}

            <MonitorCards result={monitorResult} />

            <section className="log-panel">
              <div className="panel-heading">
                <h2>Diagnostics Output</h2>
                <span>{paused && busy === "monitor" ? "Paused" : busy === "monitor" ? "Running" : isStoppedResult(monitorResult) ? "Stopped" : monitorResult?.ok ? "Completed" : monitorResult ? "Failed" : "Waiting"}</span>
              </div>
              <RawLog result={monitorResult} />
            </section>
          </>
        ) : mode === "pointcloud" ? (
          <ErrorBoundary fallbackTitle="PointCloud Preview failed">
            <Suspense fallback={<div className="module-loading">Loading PointCloud Preview...</div>}>
              <PointCloudPreview
                activeRunId={activeRunRef.current?.kind === "preview" ? activeRunRef.current.runId : null}
                state={previewState}
                running={busy === "preview"}
                switching={busy === "monitor" && stoppingRunId !== null}
                stopping={stoppingRunId !== null && activeRunRef.current?.kind === "preview"}
                error={error}
                notice={previewNotice}
                onFirstFrame={handlePreviewFirstFrame}
                onStart={runPreview}
                onStop={stopActiveRun}
              />
            </Suspense>
          </ErrorBoundary>
        ) : (
          <>
            <header className="topbar">
              <div>
                <h1>Command Logs</h1>
              </div>
            </header>
            <StatusBanner mode={mode} result={latestResult} busy={busy} paused={paused} />
            <section className="log-panel full">
              <div className="panel-heading">
                <h2>Latest Output</h2>
                <span>{paused ? "Paused" : busy ? "Running" : isStoppedResult(latestResult) ? "Stopped" : latestResult?.ok ? "Completed" : latestResult ? "Failed" : "Waiting"}</span>
              </div>
              <RawLog result={latestResult} />
            </section>
          </>
        )}
      </main>
      {aboutOpen && <AboutDialog version={APP_VERSION} onClose={closeAbout} />}
    </div>
  );
}
