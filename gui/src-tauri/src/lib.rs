use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::env;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    mpsc, Arc, Mutex,
};
use std::thread;
use std::time::{Duration, Instant};
use tauri::{AppHandle, State, Window};

#[derive(Debug, Serialize)]
struct CliInfo {
    path: Option<String>,
    exists: bool,
    candidates: Vec<String>,
    version: Option<String>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct RunOptions {
    iface: Option<String>,
    timeout_sec: Option<f64>,
    no_auto_bind: Option<bool>,
    auto_bind_ip: Option<String>,
}

#[derive(Debug, Serialize, Clone)]
#[serde(rename_all = "camelCase")]
struct CommandResult {
    ok: bool,
    code: Option<i32>,
    stdout: String,
    stderr: String,
    elapsed_ms: u128,
    summary: BTreeMap<String, String>,
}

#[derive(Debug, Serialize, Clone)]
#[serde(rename_all = "camelCase")]
struct OutputEvent {
    run_id: String,
    stream: String,
    line: String,
}

#[derive(Debug, Serialize, Clone)]
#[serde(rename_all = "camelCase")]
struct CompleteEvent {
    run_id: String,
    result: Option<CommandResult>,
    error: Option<String>,
}

#[derive(Clone)]
struct RunControl {
    stop: Arc<AtomicBool>,
    pid: Arc<Mutex<Option<u32>>>,
}

#[derive(Clone, Default)]
struct RunRegistry {
    runs: Arc<Mutex<BTreeMap<String, RunControl>>>,
}

impl RunRegistry {
    fn start(&self, run_id: &str) -> Result<RunControl, String> {
        let mut runs = self
            .runs
            .lock()
            .map_err(|_| "run registry lock is unavailable".to_string())?;
        if runs.contains_key(run_id) {
            return Err(format!("run already exists: {run_id}"));
        }
        let control = RunControl {
            stop: Arc::new(AtomicBool::new(false)),
            pid: Arc::new(Mutex::new(None)),
        };
        runs.insert(run_id.to_string(), control.clone());
        Ok(control)
    }

    fn control(&self, run_id: &str) -> Result<Option<RunControl>, String> {
        let runs = self
            .runs
            .lock()
            .map_err(|_| "run registry lock is unavailable".to_string())?;
        Ok(runs.get(run_id).cloned())
    }

    fn stop(&self, run_id: &str) -> Result<bool, String> {
        let Some(control) = self.control(run_id)? else {
            return Ok(false);
        };
        control.stop.store(true, Ordering::SeqCst);
        resume_process(&control)?;
        Ok(true)
    }

    fn pause(&self, run_id: &str) -> Result<bool, String> {
        let Some(control) = self.control(run_id)? else {
            return Ok(false);
        };
        signal_process(&control, libc::SIGSTOP)?;
        Ok(true)
    }

    fn resume(&self, run_id: &str) -> Result<bool, String> {
        let Some(control) = self.control(run_id)? else {
            return Ok(false);
        };
        resume_process(&control)?;
        Ok(true)
    }

    fn finish(&self, run_id: &str) {
        if let Ok(mut runs) = self.runs.lock() {
            runs.remove(run_id);
        }
    }
}

fn signal_process(control: &RunControl, signal: libc::c_int) -> Result<(), String> {
    let pid = *control
        .pid
        .lock()
        .map_err(|_| "run process lock is unavailable".to_string())?;
    let Some(pid) = pid else {
        return Err("run process is not ready yet".to_string());
    };
    let result = unsafe { libc::kill(pid as libc::pid_t, signal) };
    if result == 0 {
        Ok(())
    } else {
        Err(std::io::Error::last_os_error().to_string())
    }
}

fn resume_process(control: &RunControl) -> Result<(), String> {
    signal_process(control, libc::SIGCONT)
}

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .map(Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("."))
}

fn cli_names() -> Vec<String> {
    let mut names = if cfg!(windows) {
        vec![
            "livox_mid360_diagnostics.exe".to_string(),
            "livox_mid360_diagnostics".to_string(),
        ]
    } else {
        vec!["livox_mid360_diagnostics".to_string()]
    };
    if cfg!(target_os = "linux") {
        names.push(format!("livox_mid360_diagnostics-linux-{}", env::consts::ARCH));
    }
    names
}

fn bundled_cli_candidates(app: Option<&AppHandle>) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    let names = cli_names();
    if let Some(app) = app {
        for name in &names {
            if let Some(path) = app.path_resolver().resolve_resource(format!("resources/{name}")) {
                paths.push(path);
            }
            if let Some(path) = app.path_resolver().resolve_resource(name) {
                paths.push(path);
            }
        }
    }
    if let Ok(exe) = env::current_exe() {
        if let Some(dir) = exe.parent() {
            for name in &names {
                paths.push(dir.join("resources").join(name));
                paths.push(dir.join(name));
            }
        }
    }
    paths
}

fn candidate_paths(app: Option<&AppHandle>) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    if let Ok(path) = env::var("LIVOX_MID360_DIAGNOSTICS_BIN") {
        paths.push(PathBuf::from(path));
    }
    paths.extend(bundled_cli_candidates(app));

    let root = repo_root();
    let names = cli_names();
    let dirs = [
        root.join("build").join("sdk2"),
        root.join("build"),
        root.clone(),
        root.join("dist").join("prebuilt"),
        Path::new(env!("CARGO_MANIFEST_DIR")).join("resources"),
    ];

    for dir in dirs {
        for name in &names {
            paths.push(dir.join(name));
        }
    }

    paths
}

fn find_cli(app: Option<&AppHandle>) -> CliInfo {
    let candidates = candidate_paths(app);
    let found = candidates.iter().find(|path| path.is_file()).cloned();
    let version = found.as_ref().and_then(|path| cli_version(path));
    CliInfo {
        path: found.as_ref().map(|path| path.display().to_string()),
        exists: found.is_some(),
        candidates: candidates
            .iter()
            .map(|path| path.display().to_string())
            .collect(),
        version,
    }
}

fn cli_version(path: &Path) -> Option<String> {
    let output = Command::new(path).arg("--version").output().ok()?;
    if !output.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if text.is_empty() {
        None
    } else {
        Some(text)
    }
}

fn push_common_args(args: &mut Vec<String>, options: &RunOptions) {
    if let Some(iface) = options.iface.as_deref().map(str::trim).filter(|value| !value.is_empty()) {
        args.push("--iface".to_string());
        args.push(iface.to_string());
    }
    if options.no_auto_bind.unwrap_or(false) {
        args.push("--no-auto-bind".to_string());
    } else if let Some(ip) = options.auto_bind_ip.as_deref().map(str::trim).filter(|value| !value.is_empty()) {
        args.push("--auto-bind-ip".to_string());
        args.push(ip.to_string());
    }
}

fn emit_output(window: &Window, run_id: &str, stream: &str, line: &str) {
    let _ = window.emit(
        "diagnostics://output",
        OutputEvent {
            run_id: run_id.to_string(),
            stream: stream.to_string(),
            line: line.to_string(),
        },
    );
}

fn is_completion_marker(line: &str) -> bool {
    let trimmed = line.trim();
    trimmed == "stopped" || trimmed.starts_with("stopped.")
}

fn record_output(
    window: &Window,
    run_id: &str,
    stdout: &mut String,
    stderr: &mut String,
    completion_seen_at: &mut Option<Instant>,
    stream: String,
    line: String,
) {
    emit_output(window, run_id, &stream, &line);
    if is_completion_marker(&line) {
        *completion_seen_at = Some(Instant::now());
    }
    if stream == "stdout" {
        stdout.push_str(&line);
        stdout.push('\n');
    } else {
        stderr.push_str(&line);
        stderr.push('\n');
    }
}

fn drain_available_output(
    rx: &mpsc::Receiver<(String, String)>,
    window: &Window,
    run_id: &str,
    stdout: &mut String,
    stderr: &mut String,
    completion_seen_at: &mut Option<Instant>,
) {
    while let Ok((stream, line)) = rx.try_recv() {
        record_output(window, run_id, stdout, stderr, completion_seen_at, stream, line);
    }
}

fn run_cli(
    app: AppHandle,
    window: Window,
    run_id: String,
    args: Vec<String>,
    timeout: Duration,
    control: RunControl,
) -> Result<CommandResult, String> {
    let info = find_cli(Some(&app));
    let binary = info
        .path
        .map(PathBuf::from)
        .ok_or_else(|| format!("livox_mid360_diagnostics binary not found. Checked: {}", info.candidates.join(", ")))?;
    let started = Instant::now();
    emit_output(&window, &run_id, "system", &format!("$ {} {}", binary.display(), args.join(" ")));
    let mut child = Command::new(&binary)
        .args(&args)
        .current_dir(repo_root())
        .env("NO_COLOR", "1")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|err| format!("failed to start {}: {}", binary.display(), err))?;
    {
        let mut pid = control
            .pid
            .lock()
            .map_err(|_| "run process lock is unavailable".to_string())?;
        *pid = Some(child.id());
    }

    let stdout_pipe = child.stdout.take();
    let stderr_pipe = child.stderr.take();
    let (tx, rx) = mpsc::channel::<(String, String)>();
    let mut readers = Vec::new();

    if let Some(pipe) = stdout_pipe {
        let tx = tx.clone();
        readers.push(thread::spawn(move || {
            let reader = BufReader::new(pipe);
            for line in reader.lines() {
                match line {
                    Ok(line) => {
                        if tx.send(("stdout".to_string(), line)).is_err() {
                            break;
                        }
                    }
                    Err(_) => break,
                }
            }
        }));
    }
    if let Some(pipe) = stderr_pipe {
        let tx = tx.clone();
        readers.push(thread::spawn(move || {
            let reader = BufReader::new(pipe);
            for line in reader.lines() {
                match line {
                    Ok(line) => {
                        if tx.send(("stderr".to_string(), line)).is_err() {
                            break;
                        }
                    }
                    Err(_) => break,
                }
            }
        }));
    }
    drop(tx);

    let mut stdout = String::new();
    let mut stderr = String::new();
    let mut completion_seen_at: Option<Instant> = None;
    let mut forced_completion = false;
    let mut stopped_by_user = false;
    let status = loop {
        drain_available_output(&rx, &window, &run_id, &mut stdout, &mut stderr, &mut completion_seen_at);

        if let Some(status) = child
            .try_wait()
            .map_err(|err| format!("failed while waiting for CLI: {}", err))?
        {
            break status;
        }

        if control.stop.load(Ordering::SeqCst) {
            stopped_by_user = true;
            emit_output(&window, &run_id, "system", "command stopped by user");
            let _ = resume_process(&control);
            let _ = child.kill();
            break child
                .wait()
                .map_err(|err| format!("failed while stopping CLI process: {}", err))?;
        }

        if let Some(seen_at) = completion_seen_at {
            if seen_at.elapsed() >= Duration::from_millis(1200) {
                forced_completion = true;
                emit_output(
                    &window,
                    &run_id,
                    "system",
                    "command reached its completion marker; finalizing lingering process",
                );
                let _ = child.kill();
                break child
                    .wait()
                    .map_err(|err| format!("failed while finalizing CLI process: {}", err))?;
            }
        }

        if started.elapsed() >= timeout {
            let _ = child.kill();
            let _ = child.wait();
            emit_output(&window, &run_id, "system", &format!("command timed out after {} seconds", timeout.as_secs()));
            return Err(format!("command timed out after {} seconds", timeout.as_secs()));
        }

        thread::sleep(Duration::from_millis(25));
    };

    let drain_deadline = Instant::now() + Duration::from_millis(250);
    loop {
        match rx.recv_timeout(Duration::from_millis(20)) {
            Ok((stream, line)) => {
                record_output(&window, &run_id, &mut stdout, &mut stderr, &mut completion_seen_at, stream, line);
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {
                if Instant::now() >= drain_deadline {
                    break;
                }
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }

    let mut summary = parse_summary(&stdout, &stderr);
    if stopped_by_user {
        summary.insert("status".to_string(), "Stopped by user".to_string());
    }
    let ok = !stopped_by_user && (status.success() || forced_completion);
    let code = if forced_completion { Some(0) } else { status.code() };
    emit_output(
        &window,
        &run_id,
        "system",
        if stopped_by_user {
            "process stopped by user".to_string()
        } else {
            format!("process exited with code {}", code.map_or_else(|| "N/A".to_string(), |code| code.to_string()))
        }
        .as_str(),
    );
    Ok(CommandResult {
        ok,
        code,
        stdout,
        stderr,
        elapsed_ms: started.elapsed().as_millis(),
        summary,
    })
}

fn emit_complete(window: &Window, run_id: String, result: Result<CommandResult, String>) {
    let payload = match result {
        Ok(result) => CompleteEvent {
            run_id,
            result: Some(result),
            error: None,
        },
        Err(error) => CompleteEvent {
            run_id,
            result: None,
            error: Some(error),
        },
    };
    let _ = window.emit("diagnostics://complete", payload);
}

fn parse_summary(stdout: &str, stderr: &str) -> BTreeMap<String, String> {
    let mut summary = BTreeMap::new();
    for line in stdout.lines().chain(stderr.lines()) {
        parse_colon_line(line, &mut summary);
        parse_key_value_line(line, &mut summary);
        parse_discovery_line(line, &mut summary);
        parse_error_line(line, &mut summary);
    }
    summary
}

fn parse_colon_line(line: &str, summary: &mut BTreeMap<String, String>) {
    let Some((key, value)) = line.split_once(':') else {
        return;
    };
    let normalized = key.trim().to_ascii_lowercase().replace('-', "_");
    let interesting = [
        "iface",
        "iface_ip",
        "lidar_ip",
        "broadcast_code",
        "arp_host_ip",
        "discovery_pkts",
        "detect_method",
    ];
    if interesting.contains(&normalized.as_str()) {
        summary.insert(normalized, value.trim().to_string());
    }
}

fn parse_key_value_line(line: &str, summary: &mut BTreeMap<String, String>) {
    const KEYS: &[&str] = &[
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
    ];

    let mut positions: Vec<(usize, &str)> = KEYS
        .iter()
        .filter_map(|key| find_key_value_start(line, key).map(|index| (index, *key)))
        .collect();
    if positions.is_empty() {
        return;
    }
    positions.sort_by_key(|(index, _)| *index);
    for (idx, (start, key)) in positions.iter().enumerate() {
        let value_start = start + key.len() + 1;
        let value_end = positions
            .get(idx + 1)
            .map(|(next, _)| *next)
            .unwrap_or(line.len());
        let value = line[value_start..value_end].trim();
        if !value.is_empty() {
            summary.insert((*key).to_string(), value.to_string());
        }
    }
}

fn find_key_value_start(line: &str, key: &str) -> Option<usize> {
    let needle = format!("{key}=");
    let mut search_start = 0;
    while let Some(relative) = line[search_start..].find(&needle) {
        let index = search_start + relative;
        if index == 0 || line.as_bytes().get(index.wrapping_sub(1)) == Some(&b' ') {
            return Some(index);
        }
        search_start = index + needle.len();
    }
    None
}

fn parse_discovery_line(line: &str, summary: &mut BTreeMap<String, String>) {
    let Some(rest) = line.strip_prefix("discovery: Livox-SDK2 scan candidates: ") else {
        return;
    };
    let Some((iface, tail)) = rest.split_once(" (") else {
        return;
    };
    summary.entry("iface".to_string()).or_insert_with(|| iface.trim().to_string());
    if let Some((ip, _)) = tail.split_once('/') {
        summary
            .entry("iface_ip".to_string())
            .or_insert_with(|| ip.trim().to_string());
    }
}

fn parse_error_line(line: &str, summary: &mut BTreeMap<String, String>) {
    let Some(message) = line.strip_prefix("ERROR: ") else {
        return;
    };
    summary.insert("status".to_string(), message.trim().to_string());
}

#[tauri::command]
fn get_cli_info(app: AppHandle) -> CliInfo {
    find_cli(Some(&app))
}

#[tauri::command]
fn run_monitor(app: AppHandle, window: Window, registry: State<RunRegistry>, run_id: String, options: RunOptions) -> Result<(), String> {
    let timeout_sec = options.timeout_sec.unwrap_or(5.0).clamp(1.0, 60.0);
    let registry = registry.inner().clone();
    let control = registry.start(&run_id)?;
    let mut args = vec![
        "monitor".to_string(),
        "--timeout".to_string(),
        timeout_sec.to_string(),
        "--duration".to_string(),
        "0".to_string(),
        "--interval".to_string(),
        "1".to_string(),
    ];
    push_common_args(&mut args, &options);
    thread::spawn(move || {
        let result = run_cli(
            app,
            window.clone(),
            run_id.clone(),
            args,
            Duration::from_secs(365 * 24 * 60 * 60),
            control,
        );
        registry.finish(&run_id);
        emit_complete(&window, run_id, result);
    });
    Ok(())
}

#[tauri::command]
fn stop_run(registry: State<RunRegistry>, run_id: String) -> Result<bool, String> {
    registry.stop(&run_id)
}

#[tauri::command]
fn pause_run(registry: State<RunRegistry>, run_id: String) -> Result<bool, String> {
    registry.pause(&run_id)
}

#[tauri::command]
fn resume_run(registry: State<RunRegistry>, run_id: String) -> Result<bool, String> {
    registry.resume(&run_id)
}

pub fn run() {
    tauri::Builder::default()
        .manage(RunRegistry::default())
        .invoke_handler(tauri::generate_handler![get_cli_info, run_monitor, stop_run, pause_run, resume_run])
        .run(tauri::generate_context!())
        .expect("error while running Livox MID360 Diagnostics GUI");
}
