import { invoke } from "@tauri-apps/api/tauri";
import { listen } from "@tauri-apps/api/event";
import {
  Activity,
  Cable,
  CheckCircle2,
  CircleAlert,
  Gauge,
  Loader2,
  Pause,
  Play,
  Radar,
  RefreshCw,
  Settings2,
  Square,
  TerminalSquare,
} from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";

type CliInfo = {
  path: string | null;
  exists: boolean;
  candidates: string[];
  version: string | null;
};

type RunOptions = {
  iface?: string;
  timeoutSec?: number;
  noAutoBind?: boolean;
  autoBindIp?: string;
};

type CommandResult = {
  ok: boolean;
  code: number | null;
  stdout: string;
  stderr: string;
  elapsedMs: number;
  summary: Record<string, string>;
};

type PanelMode = "diagnostics" | "logs";
type DiagnosticsView = "overview" | "details";
type RunKind = "monitor";

type OutputEvent = {
  runId: string;
  stream: "stdout" | "stderr" | "system";
  line: string;
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

function isTauriRuntime() {
  return typeof window.__TAURI_IPC__ === "function";
}

function tauriRequiredMessage() {
  return "This action requires the Tauri desktop window. Browser preview at http://127.0.0.1:1420 only renders the UI.";
}

function textValue(value: string | undefined, fallback = "unavailable") {
  const normalized = value?.trim();
  return normalized && normalized.toUpperCase() !== "N/A" ? normalized : fallback;
}

function hasDisplayValue(value: string | undefined) {
  const normalized = value?.trim();
  if (!normalized) return false;
  const lowered = normalized.toLowerCase();
  return ![
    "n/a",
    "unavailable",
    "undefined",
    "null",
    "-1",
    "unknown(-1)",
    "sdk2 link",
  ].includes(lowered);
}

function modeLabel(mode: PanelMode | RunKind) {
  if (mode === "diagnostics" || mode === "monitor") return "Diagnostics";
  return "Logs";
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

const SUMMARY_KEYS = [
  "elapsed",
  "sn",
  "product",
  "handle",
  "dev_type",
  "ip",
  "mask",
  "gateway",
  "net_state",
  "net_point",
  "net_imu",
  "net_control",
  "net_log",
  "link",
  "health",
  "diag",
  "core_temp",
  "env_temp",
  "pcl_data_type",
  "pattern",
  "dual_emit",
  "frame_rate",
  "fov_enable",
  "detect_mode",
  "work",
  "boot_work",
  "glass_heat",
  "glass_heat_state",
  "point_send",
  "imu_enable",
  "fusa",
  "force_heat",
  "esc_mode",
  "pps_sync",
  "time_sync",
  "time_sync_raw",
  "local_time",
  "last_sync",
  "time_offset",
  "fw",
  "loader",
  "hw",
  "mac",
  "fw_type",
  "flash",
  "powerups",
  "blind_spot",
  "roi",
  "status_code",
  "hms",
  "info_src",
  "info_age",
  "points",
  "point_packets",
  "point_mbps",
  "imu",
  "type",
  "frame",
  "udp",
] as const;

function parseSummaryFromLine(line: string) {
  const summary: Record<string, string> = {};

  const colonMatch = line.match(/^\s*([A-Za-z_-]+)\s*:\s*(.+)\s*$/);
  if (colonMatch) {
    const key = colonMatch[1].trim().toLowerCase().replace(/-/g, "_");
    if (["iface", "iface_ip", "lidar_ip", "broadcast_code", "arp_host_ip", "discovery_pkts", "detect_method"].includes(key)) {
      summary[key] = colonMatch[2].trim();
    }
  }

  const positions = SUMMARY_KEYS
    .map((key) => {
      const match = new RegExp(`(?:^|\\s)${key}=`).exec(line);
      if (!match || match.index < 0) {
        return { key, index: -1, valueStart: -1 };
      }
      const prefixOffset = match[0].startsWith(" ") ? 1 : 0;
      const index = match.index + prefixOffset;
      return { key, index, valueStart: index + key.length + 1 };
    })
    .filter((item) => item.index >= 0)
    .sort((left, right) => left.index - right.index);

  for (let idx = 0; idx < positions.length; idx += 1) {
    const { key, valueStart } = positions[idx];
    const valueEnd = positions[idx + 1]?.index ?? line.length;
    const value = line.slice(valueStart, valueEnd).trim();
    if (value) {
      summary[key] = value;
    }
  }

  const discoveryPrefix = "discovery: Livox-SDK2 scan candidates: ";
  if (line.startsWith(discoveryPrefix)) {
    const rest = line.slice(discoveryPrefix.length);
    const ifaceMatch = rest.match(/^(\S+)\s+\(([^/)]+)/);
    if (ifaceMatch) {
      summary.iface = summary.iface ?? ifaceMatch[1];
      summary.iface_ip = summary.iface_ip ?? ifaceMatch[2];
    }
  }

  const error = line.match(/^ERROR:\s*(.+)$/);
  if (error) {
    summary.status = error[1].trim();
  }

  return summary;
}

function statusTone(result: CommandResult | null) {
  if (!result) return "idle";
  if (isStoppedResult(result)) return "idle";
  return result.ok ? "ok" : "warn";
}

function statusText(mode: PanelMode, result: CommandResult | null, busy: RunKind | null, paused: boolean) {
  if (mode === "logs") {
    if (paused) return `${modeLabel(busy ?? "monitor")} is paused`;
    if (busy) return `${modeLabel(busy)} is running`;
    if (!result) return "No command has run yet";
    if (isStoppedResult(result)) return "Latest command was stopped";
    return result.ok ? "Latest command completed successfully" : result.summary.status ?? "Latest command needs attention";
  }
  if (paused) {
    return `${modeLabel(mode)} is paused`;
  }
  if (busy) {
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
  const running = mode !== "logs" && busy !== null;
  const tone = paused ? "paused" : running ? "running" : statusTone(result);
  const label = paused ? "Paused" : running ? "Running" : isStoppedResult(result) ? "Stopped" : result ? (result.ok ? "Passed" : "Failed") : "Waiting";
  return (
    <section className={`status-banner ${tone}`}>
      <div className="status-title">
        {tone === "ok" ? <CheckCircle2 size={20} /> : tone === "warn" ? <CircleAlert size={20} /> : <Settings2 size={20} />}
        <span>{label}</span>
      </div>
      <strong>{statusText(mode, result, busy, paused)}</strong>
    </section>
  );
}

function Field({ label, value }: { label: string; value: string | undefined }) {
  const displayValue = textValue(value);
  return (
    <div className="field" title={`${label}: ${displayValue}`}>
      <span>{label}</span>
      <strong>{displayValue}</strong>
    </div>
  );
}

type DetailField = {
  label: string;
  value: string | undefined;
};

function DetailCard({
  title,
  icon,
  fields,
}: {
  title: string;
  icon: React.ReactNode;
  fields: DetailField[];
}) {
  const visibleFields = fields.filter((field) => hasDisplayValue(field.value));
  if (visibleFields.length === 0) {
    return null;
  }
  return (
    <MetricCard title={title} icon={icon} className="span-4">
      {visibleFields.map((field) => (
        <Field key={field.label} label={field.label} value={field.value} />
      ))}
    </MetricCard>
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

function appendLine(result: CommandResult | null, stream: OutputEvent["stream"], line: string): CommandResult {
  const base = result ?? EMPTY_RESULT;
  const text = stream === "system" ? `[system] ${line}` : line;
  const parsedSummary = parseSummaryFromLine(line);
  const summary = Object.keys(parsedSummary).length > 0 ? { ...base.summary, ...parsedSummary } : base.summary;
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
    const summary = parseSummaryFromLine(trimmed);
    const parts = [
      textValue(summary.ip, ""),
      textValue(summary.sn, ""),
      textValue(summary.iface_ip, ""),
    ].filter(Boolean);
    return `Device discovered${parts.length ? `: ${parts.join(" · ")}` : "."}`;
  }
  if (trimmed === "stopped" || trimmed.startsWith("stopped.")) {
    return "Diagnostics stopped.";
  }
  if (trimmed.startsWith("elapsed=")) {
    const summary = parseSummaryFromLine(trimmed);
    const parts = [
      textValue(summary.elapsed, ""),
      textValue(summary.ip, ""),
      textValue(summary.link, ""),
      textValue(summary.health, ""),
      textValue(summary.core_temp, ""),
      textValue(summary.points, ""),
      textValue(summary.point_packets, ""),
      textValue(summary.imu, ""),
    ].filter(Boolean);
    return parts.length ? parts.join(" · ") : "Sample received.";
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

function ControlStrip({
  iface,
  timeoutSec,
  autoBindIp,
  noAutoBind,
  onIface,
  onTimeout,
  onAutoBindIp,
  onNoAutoBind,
  onCheck,
}: {
  iface: string;
  timeoutSec: number;
  autoBindIp: string;
  noAutoBind: boolean;
  onIface: (value: string) => void;
  onTimeout: (value: number) => void;
  onAutoBindIp: (value: string) => void;
  onNoAutoBind: (value: boolean) => void;
  onCheck: () => void;
}) {
  return (
    <section className="control-strip diagnostics-controls">
      <label>
        <span>Interface</span>
        <input value={iface} onChange={(event) => onIface(event.target.value)} placeholder="auto" />
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
      <label>
        <span>Auto-bind IP</span>
        <input value={autoBindIp} onChange={(event) => onAutoBindIp(event.target.value)} />
      </label>
      <label className="toggle-row">
        <input type="checkbox" checked={noAutoBind} onChange={(event) => onNoAutoBind(event.target.checked)} />
        <span>No auto-bind</span>
      </label>
      <button className="secondary" title={!isTauriRuntime() ? tauriRequiredMessage() : undefined} onClick={onCheck}>
        <RefreshCw size={17} />
        Check
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
  view,
  iface,
  timeoutSec,
  autoBindIp,
  noAutoBind,
  stopping,
  onIface,
  onTimeout,
  onAutoBindIp,
  onNoAutoBind,
  onCheck,
  onMonitor,
  onPause,
  onResume,
  onStop,
  onView,
}: {
  mode: PanelMode;
  title: string;
  result: CommandResult | null;
  busy: RunKind | null;
  paused: boolean;
  view: DiagnosticsView;
  iface: string;
  timeoutSec: number;
  autoBindIp: string;
  noAutoBind: boolean;
  stopping: boolean;
  onIface: (value: string) => void;
  onTimeout: (value: number) => void;
  onAutoBindIp: (value: string) => void;
  onNoAutoBind: (value: boolean) => void;
  onCheck: () => void;
  onMonitor: () => void;
  onPause: () => void;
  onResume: () => void;
  onStop: () => void;
  onView: (view: DiagnosticsView) => void;
}) {
  return (
    <section className="workbench-header">
      <div className="workbench-title">
        <div>
          <p className="eyebrow">Tauri Desktop Demo</p>
          <h1>{title}</h1>
        </div>
        <ActionBar busy={busy} stopping={stopping} paused={paused} onMonitor={onMonitor} onPause={onPause} onResume={onResume} onStop={onStop} />
      </div>
      <StatusBanner mode={mode} result={result} busy={busy} paused={paused} />
      <div className="view-tabs" role="tablist" aria-label="Diagnostics views">
        <button className={view === "overview" ? "active" : ""} onClick={() => onView("overview")}>
          Overview
        </button>
        <button className={view === "details" ? "active" : ""} onClick={() => onView("details")}>
          Details
        </button>
      </div>
      <ControlStrip
        iface={iface}
        timeoutSec={timeoutSec}
        autoBindIp={autoBindIp}
        noAutoBind={noAutoBind}
        onIface={onIface}
        onTimeout={onTimeout}
        onAutoBindIp={onAutoBindIp}
        onNoAutoBind={onNoAutoBind}
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

function DetailsCards({ result, cliInfo }: { result: CommandResult | null; cliInfo: CliInfo | null }) {
  const product = result?.summary.product;
  const summary = result?.summary ?? {};
  return (
    <section className="grid details-grid">
      <DetailCard
        title="Identity"
        icon={<Activity size={18} />}
        fields={[
          { label: "Model", value: deviceModel(summary.dev_type, product) },
          { label: "Device Type", value: summary.dev_type },
          { label: "Handle", value: summary.handle },
          { label: "Serial", value: summary.sn },
          { label: "MAC", value: summary.mac },
          { label: "Product Info", value: product },
        ]}
      />
      <DetailCard
        title="Network"
        icon={<Cable size={18} />}
        fields={[
          { label: "Lidar IP", value: summary.ip },
          { label: "Mask", value: summary.mask },
          { label: "Gateway", value: summary.gateway },
          { label: "Point Endpoint", value: summary.net_point },
          { label: "IMU Endpoint", value: summary.net_imu },
          { label: "State Endpoint", value: summary.net_state },
          { label: "Control Endpoint", value: summary.net_control },
          { label: "Log Endpoint", value: summary.net_log },
        ]}
      />
      <DetailCard
        title="Firmware"
        icon={<TerminalSquare size={18} />}
        fields={[
          { label: "App Version", value: summary.fw },
          { label: "Loader Version", value: summary.loader },
          { label: "Hardware", value: summary.hw },
          { label: "Firmware Type", value: summary.fw_type ?? productField(product, "FmType") },
          { label: "Firmware Ver", value: productField(product, "FmVer") },
          { label: "Build Time", value: productField(product, "BuildTime") },
          { label: "Flash Status", value: summary.flash },
          { label: "GUI Backend", value: cliInfo?.version ?? undefined },
        ]}
      />
      <DetailCard
        title="Health"
        icon={<Gauge size={18} />}
        fields={[
          { label: "Health", value: summary.health },
          { label: "Diag", value: summary.diag },
          { label: "Status Code", value: summary.status_code },
          { label: "HMS", value: summary.hms },
          { label: "Core Temp", value: summary.core_temp },
          { label: "Env Temp", value: summary.env_temp },
          { label: "Power Ups", value: summary.powerups },
          { label: "Info Source", value: summary.info_src },
          { label: "Info Age", value: summary.info_age },
        ]}
      />
      <DetailCard
        title="Configuration"
        icon={<Settings2 size={18} />}
        fields={[
          { label: "PCL Data Type", value: summary.pcl_data_type },
          { label: "Pattern", value: summary.pattern },
          { label: "Dual Emit", value: summary.dual_emit },
          { label: "Frame Rate", value: summary.frame_rate },
          { label: "FOV Enable", value: summary.fov_enable },
          { label: "Detect Mode", value: summary.detect_mode },
          { label: "Blind Spot", value: summary.blind_spot },
          { label: "ROI", value: summary.roi },
          { label: "Boot Work", value: summary.boot_work },
          { label: "Work", value: summary.work },
          { label: "Point Send", value: summary.point_send },
          { label: "IMU Enable", value: summary.imu_enable },
          { label: "Glass Heat", value: summary.glass_heat },
          { label: "Glass Heat State", value: summary.glass_heat_state },
          { label: "FUSA", value: summary.fusa },
          { label: "Force Heat", value: summary.force_heat },
          { label: "ESC Mode", value: summary.esc_mode },
          { label: "PPS Sync", value: summary.pps_sync },
        ]}
      />
      <DetailCard
        title="Time Sync"
        icon={<TerminalSquare size={18} />}
        fields={[
          { label: "Time Sync", value: summary.time_sync },
          { label: "Time Sync Raw", value: summary.time_sync_raw },
          { label: "Local Time", value: summary.local_time },
          { label: "Last Sync", value: summary.last_sync },
          { label: "Time Offset", value: summary.time_offset },
        ]}
      />
      <DetailCard
        title="Streams"
        icon={<Cable size={18} />}
        fields={[
          { label: "Points", value: summary.points },
          { label: "Point Packets", value: summary.point_packets },
          { label: "Point Mbps", value: summary.point_mbps },
          { label: "IMU", value: summary.imu },
          { label: "Last Packet Type", value: summary.type },
          { label: "Frame", value: summary.frame },
          { label: "UDP", value: summary.udp },
        ]}
      />
    </section>
  );
}

export default function App() {
  const [mode, setMode] = useState<PanelMode>("diagnostics");
  const [diagnosticsView, setDiagnosticsView] = useState<DiagnosticsView>("overview");
  const [cliInfo, setCliInfo] = useState<CliInfo | null>(null);
  const [iface, setIface] = useState("");
  const [timeoutSec, setTimeoutSec] = useState(5);
  const [autoBindIp, setAutoBindIp] = useState("192.168.1.5");
  const [noAutoBind, setNoAutoBind] = useState(false);
  const [busy, setBusy] = useState<RunKind | null>(null);
  const [stoppingRunId, setStoppingRunId] = useState<string | null>(null);
  const [pausedRunId, setPausedRunId] = useState<string | null>(null);
  const [monitorResult, setMonitorResult] = useState<CommandResult | null>(null);
  const [error, setError] = useState("");
  const activeRunRef = useRef<{ runId: string; kind: RunKind } | null>(null);
  const latestMonitorRunRef = useRef<string | null>(null);

  const activeResult = monitorResult;
  const latestResult = monitorResult;

  const options: RunOptions = useMemo(
    () => ({
      iface: iface.trim() || undefined,
      timeoutSec,
      noAutoBind,
      autoBindIp: autoBindIp.trim() || undefined,
    }),
    [autoBindIp, iface, noAutoBind, timeoutSec],
  );

  const backend = backendState(cliInfo);
  const paused = busy !== null && pausedRunId === activeRunRef.current?.runId;

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

  useEffect(() => {
    void refreshCliInfo({ quiet: true });
  }, []);

  async function stopActiveRun() {
    const active = activeRunRef.current;
    if (!active || !isTauriRuntime()) {
      return;
    }
    setStoppingRunId(active.runId);
    setPausedRunId(null);
    setMonitorResult((current) => appendLine(current, "system", "stop requested"));
    try {
      const found = await invoke<boolean>("stop_run", { runId: active.runId });
      if (!found) {
        setError("Active command was already finished.");
        setStoppingRunId(null);
      }
    } catch (err) {
      setError(String(err));
      setStoppingRunId(null);
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
      setMonitorResult((current) => appendLine(current, "system", "monitoring paused"));
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
      setMonitorResult((current) => appendLine(current, "system", "monitoring resumed"));
    } catch (err) {
      setError(String(err));
    }
  }

  async function runMonitor() {
    const runId = createRunId("monitor");
    activeRunRef.current = { runId, kind: "monitor" };
    latestMonitorRunRef.current = runId;
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
      setMonitorResult((current) => appendLine(current, event.payload.stream, event.payload.line));
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
          <button className={mode === "logs" ? "active" : ""} onClick={() => setMode("logs")}>
            <TerminalSquare size={18} />
            Logs
          </button>
        </nav>
      </aside>

      <main>
        <header className="terminal-statusbar">
          <div className="terminal-status-left">
            <span className={`status-led ${paused ? "paused" : busy ? "running" : backend}`}></span>
            <strong>livox-mid360-diagnostics</strong>
          </div>
          <div className="terminal-status-right">
            <span>{cliInfo?.version ?? "Version unavailable"}</span>
          </div>
        </header>
        {mode === "diagnostics" ? (
          <>
            <WorkbenchHeader
              mode="diagnostics"
              title="Diagnostics"
              result={monitorResult}
              busy={busy}
              paused={paused}
              view={diagnosticsView}
              iface={iface}
              timeoutSec={timeoutSec}
              autoBindIp={autoBindIp}
              noAutoBind={noAutoBind}
              stopping={stoppingRunId !== null}
              onIface={setIface}
              onTimeout={setTimeoutSec}
              onAutoBindIp={setAutoBindIp}
              onNoAutoBind={setNoAutoBind}
              onCheck={refreshCliInfo}
              onMonitor={runMonitor}
              onPause={pauseActiveRun}
              onResume={resumeActiveRun}
              onStop={stopActiveRun}
              onView={setDiagnosticsView}
            />

            {error && <div className="error-banner">{error}</div>}

            {diagnosticsView === "overview" ? (
              <>
                <MonitorCards result={monitorResult} />

                <section className="log-panel">
                  <div className="panel-heading">
                    <h2>Diagnostics Output</h2>
                    <span>{paused ? "Paused" : busy ? "Running" : isStoppedResult(activeResult) ? "Stopped" : activeResult?.ok ? "Completed" : activeResult ? "Failed" : "Waiting"}</span>
                  </div>
                  <RawLog result={activeResult} />
                </section>
              </>
            ) : (
              <DetailsCards result={latestResult} cliInfo={cliInfo} />
            )}
          </>
        ) : (
          <>
            <header className="topbar">
              <div>
                <p className="eyebrow">Tauri Desktop Demo</p>
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
    </div>
  );
}
