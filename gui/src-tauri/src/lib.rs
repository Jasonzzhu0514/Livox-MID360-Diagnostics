use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
use base64::Engine as _;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::env;
use std::io::ErrorKind;
use std::io::{BufRead, BufReader, Read};
use std::net::Ipv4Addr;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
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
}

#[derive(Debug, Serialize, Clone)]
#[serde(rename_all = "camelCase")]
struct LidarNetworkStatus {
    state: String,
    interfaces: Vec<String>,
    iface: Option<String>,
    ip: Option<String>,
    prefix: Option<u8>,
    proposed_ip: Option<String>,
    temporary: bool,
    detail: String,
}

#[derive(Debug, Deserialize)]
struct IpAddressInfo {
    family: String,
    local: String,
    prefixlen: u8,
}

#[derive(Debug, Deserialize)]
struct IpInterfaceInfo {
    ifname: String,
    #[serde(default)]
    flags: Vec<String>,
    operstate: Option<String>,
    #[serde(default)]
    addr_info: Vec<IpAddressInfo>,
}

struct NetworkHelper {
    child: Child,
    iface: String,
    ip: String,
}

impl Drop for NetworkHelper {
    fn drop(&mut self) {
        let _ = self.child.stdin.take();
        let _ = self.child.wait();
    }
}

#[derive(Clone, Default)]
struct NetworkSetupRegistry {
    helper: Arc<Mutex<Option<NetworkHelper>>>,
    operation: Arc<Mutex<()>>,
}

impl NetworkSetupRegistry {
    fn release(&self) -> Result<(), String> {
        let _operation = self
            .operation
            .lock()
            .map_err(|_| "network setup lock is unavailable".to_string())?;
        self.release_current()
    }

    fn release_current(&self) -> Result<(), String> {
        let helper = self
            .helper
            .lock()
            .map_err(|_| "network helper lock is unavailable".to_string())?
            .take();
        drop(helper);
        Ok(())
    }

    fn owns(&self, iface: &str, ip: &str) -> bool {
        self.helper
            .lock()
            .ok()
            .and_then(|helper| {
                helper
                    .as_ref()
                    .map(|helper| helper.iface == iface && helper.ip == ip)
            })
            .unwrap_or(false)
    }
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
    #[serde(skip_serializing_if = "BTreeMap::is_empty")]
    summary: BTreeMap<String, String>,
}

#[derive(Debug, Serialize, Clone)]
#[serde(rename_all = "camelCase")]
struct CompleteEvent {
    run_id: String,
    result: Option<CommandResult>,
    error: Option<String>,
}

#[derive(Debug, Serialize, Clone)]
#[serde(rename_all = "camelCase")]
struct PointCloudFrameEvent {
    run_id: String,
    header: serde_json::Value,
    points_base64: String,
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

fn find_system_command(candidates: &[&str]) -> Option<PathBuf> {
    candidates
        .iter()
        .map(PathBuf::from)
        .find(|path| path.is_file())
}

fn ip_command() -> Result<PathBuf, String> {
    find_system_command(&["/usr/sbin/ip", "/sbin/ip", "/usr/bin/ip", "/bin/ip"])
        .ok_or_else(|| "Linux ip command was not found".to_string())
}

fn pkexec_command() -> Result<PathBuf, String> {
    find_system_command(&["/usr/bin/pkexec", "/bin/pkexec"])
        .ok_or_else(|| "pkexec was not found; install policykit-1 to configure the lidar network".to_string())
}

fn valid_interface_name(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 15
        && value
            .bytes()
            .all(|value| value.is_ascii_alphanumeric() || matches!(value, b'_' | b'-' | b'.' | b':'))
}

fn lidar_host_ip(value: &str) -> Option<Ipv4Addr> {
    let ip = value.parse::<Ipv4Addr>().ok()?;
    let octets = ip.octets();
    if octets[0..3] == [192, 168, 1] && (2..=254).contains(&octets[3]) {
        Some(ip)
    } else {
        None
    }
}

fn ethernet_interface(name: &str) -> bool {
    name.starts_with("eth") || name.starts_with("en")
}

fn ignored_interface(name: &str) -> bool {
    name == "lo"
        || name.starts_with("docker")
        || name.starts_with("br-")
        || name.starts_with("veth")
        || name.starts_with("virbr")
}

fn interface_connected(info: &IpInterfaceInfo) -> bool {
    info.flags.iter().any(|flag| flag == "LOWER_UP")
        || info.operstate.as_deref() == Some("UP")
}

fn interface_ipv4(info: &IpInterfaceInfo) -> impl Iterator<Item = &IpAddressInfo> {
    info.addr_info.iter().filter(|address| address.family == "inet")
}

fn interface_lidar_address(info: &IpInterfaceInfo) -> Option<&IpAddressInfo> {
    interface_ipv4(info).find(|address| lidar_host_ip(&address.local).is_some())
}

fn read_ip_interfaces() -> Result<Vec<IpInterfaceInfo>, String> {
    let command = ip_command()?;
    let output = Command::new(&command)
        .args(["-j", "address", "show"])
        .output()
        .map_err(|error| format!("failed to run {}: {error}", command.display()))?;
    if !output.status.success() {
        return Err(format!(
            "{} failed with exit code {}",
            command.display(),
            output.status.code().map_or_else(|| "unknown".to_string(), |code| code.to_string())
        ));
    }
    serde_json::from_slice(&output.stdout).map_err(|error| format!("failed to parse network interfaces: {error}"))
}

fn choose_lidar_host_ip(interfaces: &[IpInterfaceInfo]) -> String {
    let assigned = |candidate: &str| {
        interfaces
            .iter()
            .flat_map(interface_ipv4)
            .any(|address| address.local == candidate)
    };

    const PREFERRED_HOSTS: &[u8] = &[
        5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180,
        190, 200, 210, 220, 230, 240, 250,
    ];
    for host in PREFERRED_HOSTS.iter().copied().chain(2..=254) {
        let candidate = format!("192.168.1.{host}");
        if !assigned(&candidate) {
            return candidate;
        }
    }
    "192.168.1.5".to_string()
}

fn lidar_network_status(
    registry: &NetworkSetupRegistry,
    requested_iface: Option<&str>,
) -> Result<LidarNetworkStatus, String> {
    let interfaces = read_ip_interfaces()?;
    let requested_iface = requested_iface.map(str::trim).filter(|value| !value.is_empty());

    if let Some(iface) = requested_iface {
        if !valid_interface_name(iface) {
            return Err(format!("invalid network interface: {iface}"));
        }
    }

    let supported_interfaces: Vec<&IpInterfaceInfo> = interfaces
        .iter()
        .filter(|info| !ignored_interface(&info.ifname) && ethernet_interface(&info.ifname))
        .collect();
    let mut available_interfaces: Vec<String> = supported_interfaces
        .iter()
        .map(|info| info.ifname.clone())
        .collect();
    available_interfaces.sort();

    let eligible: Vec<&IpInterfaceInfo> = match requested_iface {
        Some(iface) => supported_interfaces
            .iter()
            .copied()
            .filter(|info| info.ifname == iface)
            .collect(),
        None => supported_interfaces,
    };

    if let Some(info) = eligible
        .iter()
        .copied()
        .find(|info| interface_connected(info) && interface_lidar_address(info).is_some())
    {
        let address = interface_lidar_address(info).expect("checked above");
        return Ok(LidarNetworkStatus {
            state: "ready".to_string(),
            interfaces: available_interfaces.clone(),
            iface: Some(info.ifname.clone()),
            ip: Some(address.local.clone()),
            prefix: Some(address.prefixlen),
            proposed_ip: None,
            temporary: registry.owns(&info.ifname, &address.local),
            detail: "The Ethernet adapter is ready for MID360 discovery.".to_string(),
        });
    }

    if let Some(info) = eligible.iter().copied().find(|info| interface_connected(info)) {
        let proposed_ip = choose_lidar_host_ip(&interfaces);
        return Ok(LidarNetworkStatus {
            state: "needsSetup".to_string(),
            interfaces: available_interfaces.clone(),
            iface: Some(info.ifname.clone()),
            ip: interface_ipv4(info).next().map(|address| address.local.clone()),
            prefix: interface_ipv4(info).next().map(|address| address.prefixlen),
            proposed_ip: Some(proposed_ip.clone()),
            temporary: false,
            detail: format!(
                "{} has no 192.168.1.x address; {} can be added temporarily without replacing its current IP.",
                info.ifname, proposed_ip
            ),
        });
    }

    let iface = eligible.first().map(|info| info.ifname.clone());
    Ok(LidarNetworkStatus {
        state: "noLink".to_string(),
        interfaces: available_interfaces,
        iface: iface.clone(),
        ip: None,
        prefix: None,
        proposed_ip: None,
        temporary: false,
        detail: iface.map_or_else(
            || "No usable Ethernet adapter was found.".to_string(),
            |iface| format!("{iface} has no active Ethernet link."),
        ),
    })
}

fn configure_lidar_network_blocking(
    registry: NetworkSetupRegistry,
    iface: String,
    ip: String,
) -> Result<LidarNetworkStatus, String> {
    let iface = iface.trim().to_string();
    let ip = ip.trim().to_string();
    if !valid_interface_name(&iface) {
        return Err(format!("invalid network interface: {iface}"));
    }
    if lidar_host_ip(&ip).is_none() {
        return Err("temporary lidar IP must be in 192.168.1.2-254".to_string());
    }

    let _operation = registry
        .operation
        .lock()
        .map_err(|_| "network setup lock is unavailable".to_string())?;
    let current = lidar_network_status(&registry, Some(&iface))?;
    if current.state == "ready" {
        return Ok(current);
    }
    if current.state != "needsSetup" || current.iface.as_deref() != Some(iface.as_str()) {
        return Err(current.detail);
    }

    registry.release_current()?;
    let ip_command = ip_command()?;
    let pkexec = pkexec_command()?;
    let cidr = format!("{ip}/24");
    const HELPER_SCRIPT: &str = r#"
set -eu
ip_bin="$1"
cidr="$2"
iface="$3"
cleanup() {
  "$ip_bin" addr del "$cidr" dev "$iface" >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM
"$ip_bin" addr add "$cidr" dev "$iface"
printf 'READY\n'
while IFS= read -r _line; do :; done
"#;

    let mut child = Command::new(&pkexec)
        .arg("/bin/sh")
        .arg("-c")
        .arg(HELPER_SCRIPT)
        .arg("livox-mid360-network-helper")
        .arg(&ip_command)
        .arg(&cidr)
        .arg(&iface)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("failed to start {}: {error}", pkexec.display()))?;

    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| "network helper output is unavailable".to_string())?;
    let mut reader = BufReader::new(stdout);
    let mut line = String::new();
    let read = reader
        .read_line(&mut line)
        .map_err(|error| format!("failed to read network helper status: {error}"))?;
    drop(reader);

    if read == 0 || line.trim() != "READY" {
        let mut stderr = String::new();
        if let Some(mut pipe) = child.stderr.take() {
            let _ = pipe.read_to_string(&mut stderr);
        }
        let status = child
            .wait()
            .map_err(|error| format!("failed to wait for network authorization: {error}"))?;
        if status.code() == Some(126) || status.code() == Some(127) {
            return Err("Network authorization was canceled or unavailable.".to_string());
        }
        let detail = stderr.trim();
        return Err(if detail.is_empty() {
            "Failed to configure the temporary lidar network address.".to_string()
        } else {
            format!("Failed to configure the temporary lidar network address: {detail}")
        });
    }

    let helper = NetworkHelper {
        child,
        iface: iface.clone(),
        ip: ip.clone(),
    };
    *registry
        .helper
        .lock()
        .map_err(|_| "network helper lock is unavailable".to_string())? = Some(helper);

    let status = lidar_network_status(&registry, Some(&iface))?;
    if status.state != "ready" {
        registry.release_current()?;
        return Err("The temporary lidar address was added but is not visible on the interface.".to_string());
    }
    Ok(status)
}

fn format_spawn_error(binary: &Path, err: std::io::Error) -> String {
    let mut message = format!("failed to start {}: {}", binary.display(), err);
    if err.kind() == ErrorKind::NotFound && binary.is_file() {
        message.push_str(
            ". The binary file exists, so Linux may have failed to find the process working directory, ELF interpreter, or a required shared library. Check that the AppImage architecture matches this machine and that libc/libstdc++/ncurses runtime libraries are installed, or set LIVOX_MID360_DIAGNOSTICS_BIN to a compatible diagnostics binary.",
        );
    }
    message
}

fn cli_working_dir() -> PathBuf {
    let root = repo_root();
    if root.is_dir() {
        return root;
    }
    env::current_dir().unwrap_or_else(|_| PathBuf::from("."))
}

fn spawn_cli_process(
    app: &AppHandle,
    window: &Window,
    run_id: &str,
    args: &[String],
    control: &RunControl,
) -> Result<Child, String> {
    let info = find_cli(Some(app));
    let binary = info
        .path
        .map(PathBuf::from)
        .ok_or_else(|| format!("livox_mid360_diagnostics binary not found. Checked: {}", info.candidates.join(", ")))?;
    emit_output(window, run_id, "system", &format!("$ {} {}", binary.display(), args.join(" ")));
    let child = Command::new(&binary)
        .args(args)
        .current_dir(cli_working_dir())
        .env("NO_COLOR", "1")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|err| format_spawn_error(&binary, err))?;
    let mut pid = control
        .pid
        .lock()
        .map_err(|_| "run process lock is unavailable".to_string())?;
    *pid = Some(child.id());
    Ok(child)
}

fn spawn_line_reader<R>(
    pipe: R,
    stream: &'static str,
    tx: mpsc::Sender<(String, String)>,
) -> thread::JoinHandle<()>
where
    R: Read + Send + 'static,
{
    let stream = stream.to_string();
    thread::spawn(move || {
        let reader = BufReader::new(pipe);
        for line in reader.lines() {
            let Ok(line) = line else {
                break;
            };
            if tx.send((stream.clone(), line)).is_err() {
                break;
            }
        }
    })
}

fn push_common_args(args: &mut Vec<String>, options: &RunOptions) {
    if let Some(iface) = options.iface.as_deref().map(str::trim).filter(|value| !value.is_empty()) {
        args.push("--iface".to_string());
        args.push(iface.to_string());
    }
    // The GUI owns temporary network setup through its authorized helper.
    args.push("--no-auto-bind".to_string());
}

fn emit_output(window: &Window, run_id: &str, stream: &str, line: &str) {
    let _ = window.emit(
        "diagnostics://output",
        OutputEvent {
            run_id: run_id.to_string(),
            stream: stream.to_string(),
            line: line.to_string(),
            summary: parse_summary_line(line),
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
    stdout: Option<&mut String>,
    stderr: &mut String,
    stream: String,
    line: String,
) {
    emit_output(window, run_id, &stream, &line);
    if stream == "stdout" {
        if let Some(stdout) = stdout {
            stdout.push_str(&line);
            stdout.push('\n');
        }
    } else {
        stderr.push_str(&line);
        stderr.push('\n');
    }
}

fn record_monitor_output(
    window: &Window,
    run_id: &str,
    stdout: &mut String,
    stderr: &mut String,
    completion_seen_at: &mut Option<Instant>,
    stream: String,
    line: String,
) {
    if is_completion_marker(&line) {
        *completion_seen_at = Some(Instant::now());
    }
    record_output(window, run_id, Some(stdout), stderr, stream, line);
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
        record_monitor_output(window, run_id, stdout, stderr, completion_seen_at, stream, line);
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
    let started = Instant::now();
    let mut child = spawn_cli_process(&app, &window, &run_id, &args, &control)?;
    let (tx, rx) = mpsc::channel::<(String, String)>();
    let mut readers = Vec::new();

    if let Some(pipe) = child.stdout.take() {
        readers.push(spawn_line_reader(pipe, "stdout", tx.clone()));
    }
    if let Some(pipe) = child.stderr.take() {
        readers.push(spawn_line_reader(pipe, "stderr", tx.clone()));
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
                record_monitor_output(&window, &run_id, &mut stdout, &mut stderr, &mut completion_seen_at, stream, line);
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

fn read_u32_le(reader: &mut impl Read) -> Result<u32, std::io::Error> {
    let mut bytes = [0u8; 4];
    reader.read_exact(&mut bytes)?;
    Ok(u32::from_le_bytes(bytes))
}

type PreviewFrame = Result<(serde_json::Value, Vec<u8>), String>;
type LatestPreviewFrame = Arc<Mutex<Option<PreviewFrame>>>;

fn replace_latest_preview_frame(latest_frame: &LatestPreviewFrame, frame: PreviewFrame) {
    if let Ok(mut latest_frame) = latest_frame.lock() {
        *latest_frame = Some(frame);
    }
}

fn report_preview_read_error(latest_frame: &LatestPreviewFrame, message: String) {
    replace_latest_preview_frame(latest_frame, Err(message));
}

fn read_preview_stdout(
    mut reader: impl Read,
    latest_frame: LatestPreviewFrame,
) {
    loop {
        let header_len = match read_u32_le(&mut reader) {
            Ok(value) => value as usize,
            Err(err) if err.kind() == ErrorKind::UnexpectedEof => break,
            Err(err) => {
                report_preview_read_error(&latest_frame, format!("failed to read point cloud frame header: {err}"));
                break;
            }
        };
        let data_offset = match read_u32_le(&mut reader) {
            Ok(value) => value as usize,
            Err(err) => {
                report_preview_read_error(&latest_frame, format!("failed to read point cloud frame offset: {err}"));
                break;
            }
        };
        if data_offset < 8 + header_len {
            report_preview_read_error(&latest_frame, "invalid point cloud frame data offset".to_string());
            break;
        }
        if header_len > 256 * 1024 {
            report_preview_read_error(&latest_frame, "point cloud frame header is too large".to_string());
            break;
        }
        let mut header_bytes = vec![0u8; header_len];
        if let Err(err) = reader.read_exact(&mut header_bytes) {
            report_preview_read_error(&latest_frame, format!("failed to read point cloud frame header JSON: {err}"));
            break;
        }
        let padding_len = data_offset - 8 - header_len;
        if padding_len > 0 {
            let mut padding = vec![0u8; padding_len];
            if let Err(err) = reader.read_exact(&mut padding) {
                report_preview_read_error(&latest_frame, format!("failed to read point cloud frame padding: {err}"));
                break;
            }
        }
        let header: serde_json::Value = match serde_json::from_slice(&header_bytes) {
            Ok(value) => value,
            Err(err) => {
                report_preview_read_error(&latest_frame, format!("failed to parse point cloud frame header: {err}"));
                break;
            }
        };
        let point_count = header
            .get("point_count")
            .and_then(serde_json::Value::as_u64)
            .unwrap_or(0) as usize;
        if point_count > 200_000 {
            report_preview_read_error(&latest_frame, "point cloud frame has too many points".to_string());
            break;
        }
        let byte_len = point_count
            .checked_mul(4)
            .and_then(|value| value.checked_mul(std::mem::size_of::<f32>()))
            .unwrap_or(0);
        let mut point_bytes = vec![0u8; byte_len];
        if let Err(err) = reader.read_exact(&mut point_bytes) {
            report_preview_read_error(&latest_frame, format!("failed to read point cloud frame data: {err}"));
            break;
        }
        replace_latest_preview_frame(&latest_frame, Ok((header, point_bytes)));
    }
}

fn emit_latest_preview_frame(
    window: &Window,
    run_id: &str,
    latest_frame: &LatestPreviewFrame,
    frame_error: &mut Option<String>,
) {
    let frame = latest_frame
        .lock()
        .ok()
        .and_then(|mut latest_frame| latest_frame.take());
    if let Some(frame) = frame {
        match frame {
            Ok((header, point_bytes)) => {
                let _ = window.emit(
                    "pointcloud://frame",
                    PointCloudFrameEvent {
                        run_id: run_id.to_string(),
                        header,
                        points_base64: BASE64_STANDARD.encode(point_bytes),
                    },
                );
            }
            Err(error) => {
                *frame_error = Some(error.clone());
                emit_output(window, run_id, "stderr", &error);
            }
        }
    }
}

fn run_preview_cli(
    app: AppHandle,
    window: Window,
    run_id: String,
    args: Vec<String>,
    control: RunControl,
) -> Result<CommandResult, String> {
    let started = Instant::now();
    let mut child = spawn_cli_process(&app, &window, &run_id, &args, &control)?;
    let latest_frame = Arc::new(Mutex::new(None));
    let (line_tx, line_rx) = mpsc::channel::<(String, String)>();
    let mut readers = Vec::new();

    if let Some(pipe) = child.stdout.take() {
        let latest_frame = latest_frame.clone();
        readers.push(thread::spawn(move || {
            read_preview_stdout(pipe, latest_frame);
        }));
    }
    if let Some(pipe) = child.stderr.take() {
        readers.push(spawn_line_reader(pipe, "stderr", line_tx.clone()));
    }
    drop(line_tx);

    let mut stderr = String::new();
    let mut stopped_by_user = false;
    let mut frame_error: Option<String> = None;
    let status = loop {
        while let Ok((stream, line)) = line_rx.try_recv() {
            record_output(&window, &run_id, None, &mut stderr, stream, line);
        }
        emit_latest_preview_frame(&window, &run_id, &latest_frame, &mut frame_error);

        if let Some(status) = child
            .try_wait()
            .map_err(|err| format!("failed while waiting for preview CLI: {}", err))?
        {
            break status;
        }

        if control.stop.load(Ordering::SeqCst) {
            stopped_by_user = true;
            emit_output(&window, &run_id, "system", "preview stopped by user");
            let _ = resume_process(&control);
            let _ = child.kill();
            break child
                .wait()
                .map_err(|err| format!("failed while stopping preview process: {}", err))?;
        }

        thread::sleep(Duration::from_millis(16));
    };

    let drain_deadline = Instant::now() + Duration::from_millis(250);
    loop {
        while let Ok((stream, line)) = line_rx.try_recv() {
            record_output(&window, &run_id, None, &mut stderr, stream, line);
        }
        emit_latest_preview_frame(&window, &run_id, &latest_frame, &mut frame_error);
        if Instant::now() >= drain_deadline {
            break;
        }
        thread::sleep(Duration::from_millis(20));
    }

    for reader in readers {
        let _ = reader.join();
    }

    let mut summary = parse_summary("", &stderr);
    if stopped_by_user {
        summary.insert("status".to_string(), "Stopped by user".to_string());
    }
    if let Some(error) = frame_error {
        summary.insert("frame_error".to_string(), error);
    }
    let ok = !stopped_by_user && status.success();
    let code = status.code();
    emit_output(
        &window,
        &run_id,
        "system",
        if stopped_by_user {
            "preview process stopped by user".to_string()
        } else {
            format!("preview process exited with code {}", code.map_or_else(|| "N/A".to_string(), |code| code.to_string()))
        }
        .as_str(),
    );
    Ok(CommandResult {
        ok,
        code,
        stdout: String::new(),
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
        summary.extend(parse_summary_line(line));
    }
    summary
}

fn parse_summary_line(line: &str) -> BTreeMap<String, String> {
    let mut summary = BTreeMap::new();
    parse_colon_line(line, &mut summary);
    parse_key_value_line(line, &mut summary);
    parse_discovery_line(line, &mut summary);
    parse_error_line(line, &mut summary);
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_monitor_summary_fields_per_line() {
        let summary = parse_summary_line("elapsed=1.0 sn=ABC123 ip=192.168.1.105 core_temp=42 env_temp=37");

        assert_eq!(summary.get("sn"), Some(&"ABC123".to_string()));
        assert_eq!(summary.get("ip"), Some(&"192.168.1.105".to_string()));
        assert_eq!(summary.get("core_temp"), Some(&"42".to_string()));
        assert_eq!(summary.get("env_temp"), Some(&"37".to_string()));
    }

    #[test]
    fn parses_discovery_and_error_summary_fields() {
        let discovery = parse_summary_line("discovery: Livox-SDK2 scan candidates: enx0 (192.168.1.5/24)");
        let error = parse_summary_line("ERROR: No MID360 lidar found by SDK discovery");

        assert_eq!(discovery.get("iface"), Some(&"enx0".to_string()));
        assert_eq!(discovery.get("iface_ip"), Some(&"192.168.1.5".to_string()));
        assert_eq!(error.get("status"), Some(&"No MID360 lidar found by SDK discovery".to_string()));
    }
}

#[tauri::command]
fn get_cli_info(app: AppHandle) -> CliInfo {
    find_cli(Some(&app))
}

#[tauri::command]
fn get_lidar_network_status(
    registry: State<NetworkSetupRegistry>,
    iface: Option<String>,
) -> Result<LidarNetworkStatus, String> {
    lidar_network_status(registry.inner(), iface.as_deref())
}

#[tauri::command]
async fn configure_lidar_network(
    registry: State<'_, NetworkSetupRegistry>,
    iface: String,
    ip: String,
) -> Result<LidarNetworkStatus, String> {
    let registry = registry.inner().clone();
    tauri::async_runtime::spawn_blocking(move || configure_lidar_network_blocking(registry, iface, ip))
        .await
        .map_err(|error| format!("network setup task failed: {error}"))?
}

#[tauri::command]
async fn release_lidar_network(registry: State<'_, NetworkSetupRegistry>) -> Result<(), String> {
    let registry = registry.inner().clone();
    tauri::async_runtime::spawn_blocking(move || registry.release())
        .await
        .map_err(|error| format!("network cleanup task failed: {error}"))?
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
fn run_preview(app: AppHandle, window: Window, registry: State<RunRegistry>, run_id: String, options: RunOptions) -> Result<(), String> {
    let timeout_sec = options.timeout_sec.unwrap_or(5.0).clamp(1.0, 60.0);
    let registry = registry.inner().clone();
    let control = registry.start(&run_id)?;
    let mut args = vec![
        "preview".to_string(),
        "--timeout".to_string(),
        timeout_sec.to_string(),
        "--duration".to_string(),
        "0".to_string(),
        "--interval".to_string(),
        "0.08".to_string(),
        "--max-points-per-frame".to_string(),
        "0".to_string(),
    ];
    push_common_args(&mut args, &options);
    thread::spawn(move || {
        let result = run_preview_cli(app, window.clone(), run_id.clone(), args, control);
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
        .manage(NetworkSetupRegistry::default())
        .invoke_handler(tauri::generate_handler![
            get_cli_info,
            get_lidar_network_status,
            configure_lidar_network,
            release_lidar_network,
            run_monitor,
            run_preview,
            stop_run,
            pause_run,
            resume_run
        ])
        .run(tauri::generate_context!())
        .expect("error while running Livox MID360 Diagnostics GUI");
}
