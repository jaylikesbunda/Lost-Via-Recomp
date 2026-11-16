#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod builder;
mod codecheck;
mod xdvdfs;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::VecDeque,
    fs,
    io::{BufRead, BufReader, Read, Write},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::{atomic::{AtomicBool, Ordering}, mpsc, Arc, Mutex},
    thread::{self, JoinHandle},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
use tauri::{AppHandle, Emitter, Manager};

/// default.xex of the supported release (title 5553080F).
const EXPECTED_XEX_SHA256: &str = "443200592190486539885ecebdc65d2f8dd7200f3b587ee35d70cd8a4a5b256c";
const XEX_REL: &str = "port/assets/default.xex";
const ASSETS_REL: &str = "port/assets";
const EXE_REL: &str = "port/out/build/win-amd64-relwithdebinfo/lost.exe";
const FPS_CHOICES: [u32; 5] = [0, 30, 60, 120, 144];
const RESOLUTIONS: [&str; 4] = ["", "1080p", "1440p", "4k"];
const LOG_CAP: usize = 4000;
const NO_REPO: &str = "The repository folder was not found. Set it in Settings.";
#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

#[derive(Clone, Serialize, Deserialize, Debug)]
#[serde(default)]
struct Settings {
    /// Target frame rate passed as --lost_target_fps (0 = uncapped). None: follow the monitor.
    fps: Option<u32>,
    /// The native Direct3D 12 renderer (--lost_native_primary).
    native_primary: bool,
    /// Borderless fullscreen (--fullscreen) or a window (--no-fullscreen).
    fullscreen: bool,
    /// --resolution preset; empty is the game's default (720p).
    resolution: String,
    /// The player accepted Microsoft's license terms for the C++ library and Windows SDK.
    msvc_license: bool,
    /// Manual repository folder when the launcher is not inside the repo.
    repo_root: Option<String>,
}

impl Default for Settings {
    fn default() -> Self {
        Settings { fps: None, native_primary: true, fullscreen: true, resolution: String::new(), msvc_license: false, repo_root: None }
    }
}

#[derive(Clone, Serialize)]
struct Prereq {
    name: String,
    ok: bool,
    detail: String,
}

#[derive(Serialize)]
struct Status {
    repo_root: Option<String>,
    game_state: String,
    game_detail: String,
    built: bool,
    prereqs: Vec<Prereq>,
    settings: Settings,
    log_dir: String,
    refresh_hz: u32,
    auto_fps: u32,
    /// The next build downloads Microsoft's libraries, whose license the player has not accepted.
    needs_license: bool,
}

#[derive(Clone, Serialize)]
struct StepEvent {
    task: &'static str,
    step: &'static str,
    state: &'static str,
    message: String,
}

#[derive(Clone, Serialize)]
struct Progress {
    percent: f64,
    text: String,
}

/// Where a job's log lines, steps and progress go: the launcher's window, or the console when
/// RecompLauncher runs the setup step (--prepare).
#[derive(Clone)]
pub enum Sink {
    App(AppHandle),
    Console(Arc<Console>),
}

/// --prepare output: "progress <0..1> <text>" lines on stdout, everything else in a log file.
pub struct Console {
    file: Mutex<Option<fs::File>>,
    /// The running step's share of the whole setup (start, length) and the last value printed.
    stage: Mutex<(f64, f64, i64)>,
}

impl Console {
    fn print(&self, fraction: f64, text: &str) {
        let mut st = self.stage.lock().unwrap();
        let value = (st.0 + st.1 * fraction.clamp(0.0, 1.0)).clamp(0.0, 1.0);
        let milli = (value * 1000.0) as i64;
        if milli != st.2 {
            st.2 = milli;
            println!("progress {value:.3} {text}");
        }
    }
}

fn emit_progress(sink: &Sink, percent: f64, text: impl Into<String>) {
    match sink {
        Sink::App(app) => {
            let _ = app.emit("progress", Progress { percent, text: text.into() });
        }
        Sink::Console(c) => c.print(percent / 100.0, &text.into()),
    }
}

#[derive(Clone, Serialize)]
struct TaskFinished {
    task: &'static str,
    ok: bool,
}

#[derive(Serialize)]
struct LogView {
    path: String,
    lines: Vec<String>,
}

struct AppState {
    busy: AtomicBool,
    settings: Mutex<Settings>,
    settings_path: PathBuf,
    log_dir: PathBuf,
    log_buf: Mutex<VecDeque<String>>,
    log_file: Mutex<Option<fs::File>>,
    log_path: Mutex<String>,
    log_tx: Mutex<mpsc::Sender<String>>,
    bundled_source: Option<PathBuf>,
}

// ---------- small helpers ----------

fn no_window(cmd: &mut Command) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    let _ = cmd;
}

fn is_repo(dir: &Path) -> bool {
    dir.join("tools").join("build.ps1").is_file()
        && dir.join("tools").join("setup.ps1").is_file()
        && dir.join("port").is_dir()
}

/// The launcher sits in <repo>\launcher, so the repo root is that folder's parent.
fn find_repo_from_exe() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    for dir in exe.ancestors() {
        if dir.file_name().is_some_and(|n| n.eq_ignore_ascii_case("launcher")) {
            if let Some(parent) = dir.parent() {
                if is_repo(parent) {
                    return Some(parent.to_path_buf());
                }
            }
        }
    }
    None
}

fn repo_root(app: &AppHandle) -> Option<PathBuf> {
    let st = app.state::<AppState>();
    let manual = st.settings.lock().unwrap().repo_root.clone();
    if let Some(p) = manual.map(PathBuf::from) {
        if is_repo(&p) {
            return Some(p);
        }
    }
    find_repo_from_exe().or_else(|| st.bundled_source.clone().filter(|p| is_repo(p)))
}

/// The installer ships the project source as resources/source.zip (tracked files only, no game
/// material). It is unpacked into the app's local data folder on first run and after an update.
/// Files already there (game files, build output) are kept; source files are overwritten.
fn unpack_bundled_source(app: &tauri::App) -> Option<PathBuf> {
    let zip_path = app.path().resource_dir().ok()?.join("resources").join("source.zip");
    if !zip_path.is_file() {
        return None;
    }
    let dest = app.path().app_local_data_dir().ok()?.join("source");
    match unpack_source(&zip_path, &dest, &app.package_info().version.to_string()) {
        Ok(()) => Some(dest),
        Err(e) => {
            eprintln!("could not unpack the bundled source: {e}");
            None
        }
    }
}

/// Unpacks source.zip into `dest` unless this launcher version already did.
fn unpack_source(zip_path: &Path, dest: &Path, version: &str) -> Result<(), String> {
    let marker = dest.join(".launcher-version");
    if fs::read_to_string(&marker).ok().as_deref() == Some(version) && is_repo(dest) {
        return Ok(());
    }
    fs::create_dir_all(dest).map_err(|e| e.to_string())?;
    let file = fs::File::open(zip_path).map_err(|e| e.to_string())?;
    let mut archive = zip::ZipArchive::new(file).map_err(|e| e.to_string())?;
    archive.extract(dest).map_err(|e| e.to_string())?;
    fs::write(&marker, version).map_err(|e| e.to_string())
}

fn unix_stamp() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let mut file = fs::File::open(path).map_err(|e| e.to_string())?;
    let mut hasher = Sha256::new();
    std::io::copy(&mut file, &mut hasher).map_err(|e| e.to_string())?;
    Ok(hasher.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

/// Returns (state, detail) where state is "missing", "wrong" or "ok".
fn game_check(repo: &Path) -> (String, String) {
    let xex = repo.join(XEX_REL);
    if !xex.is_file() {
        return ("missing".into(), "No game files yet. Choose your disc image.".into());
    }
    match sha256_file(&xex) {
        Ok(h) if h == EXPECTED_XEX_SHA256 => ("ok".into(), "Verified (title 5553080F).".into()),
        Ok(_) => (
            "wrong".into(),
            "default.xex is not the supported version. Lost: Via Domus title 5553080F is required.".into(),
        ),
        Err(e) => ("wrong".into(), format!("Could not read default.xex: {e}")),
    }
}

fn tool_version(program: &str, args: &[&str]) -> Option<String> {
    let mut cmd = Command::new(program);
    cmd.args(args).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    no_window(&mut cmd);
    let out = cmd.output().ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if text.is_empty() { None } else { Some(text) }
}

/// Program, leading arguments and version text of a working Python 3 interpreter.
fn find_python() -> Option<(String, Vec<String>, String)> {
    const CANDIDATES: [&[&str]; 2] = [&["python"], &["py", "-3"]];
    for cand in CANDIDATES {
        let mut args: Vec<&str> = cand[1..].to_vec();
        args.push("--version");
        if let Some(v) = tool_version(cand[0], &args) {
            if v.starts_with("Python 3") {
                let lead = cand[1..].iter().map(|s| s.to_string()).collect();
                return Some((cand[0].to_string(), lead, v));
            }
        }
    }
    None
}

fn vs_install() -> Option<String> {
    let pf = std::env::var("ProgramFiles(x86)").unwrap_or_else(|_| r"C:\Program Files (x86)".into());
    let vswhere = PathBuf::from(pf).join(r"Microsoft Visual Studio\Installer\vswhere.exe");
    if !vswhere.is_file() {
        return None;
    }
    let mut cmd = Command::new(vswhere);
    cmd.args([
        "-latest", "-version", "[17.0,18.0)", "-products", "*",
        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property", "installationPath",
    ]);
    cmd.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    no_window(&mut cmd);
    let out = cmd.output().ok()?;
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .map(str::trim)
        .find(|l| !l.is_empty())
        .map(str::to_string)
}

fn prereqs() -> Vec<Prereq> {
    let py = find_python().map(|(_, _, v)| v);
    let git = tool_version("git", &["--version"]);
    let vs = vs_install();
    vec![
        Prereq {
            name: "Python 3".into(),
            ok: py.is_some(),
            detail: py.unwrap_or_else(|| "Not found. Install Python 3 from python.org and tick Add to PATH.".into()),
        },
        Prereq {
            name: "Git".into(),
            ok: git.is_some(),
            detail: git.unwrap_or_else(|| "Not found. Install Git for Windows.".into()),
        },
        Prereq {
            name: "Visual Studio 2022 with C++ tools".into(),
            ok: vs.is_some(),
            detail: vs.clone().unwrap_or_else(|| {
                "Not found. Install Visual Studio 2022 with the Desktop development with C++ workload.".into()
            }),
        },
    ]
}

fn sanitize(s: &mut Settings) {
    if s.fps.is_some_and(|f| !FPS_CHOICES.contains(&f)) {
        s.fps = None;
    }
    if !RESOLUTIONS.contains(&s.resolution.as_str()) {
        s.resolution.clear();
    }
}

/// The primary monitor's refresh rate in Hz (0 if unknown).
fn refresh_hz() -> u32 {
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::Graphics::Gdi::{EnumDisplaySettingsW, DEVMODEW, ENUM_CURRENT_SETTINGS};
        let mut dm: DEVMODEW = std::mem::zeroed();
        dm.dmSize = std::mem::size_of::<DEVMODEW>() as u16;
        if EnumDisplaySettingsW(std::ptr::null(), ENUM_CURRENT_SETTINGS, &mut dm) != 0 && dm.dmDisplayFrequency > 1 {
            return dm.dmDisplayFrequency;
        }
    }
    0
}

/// The highest frame rate choice the monitor can show (60 if the refresh rate is unknown).
fn auto_fps(hz: u32) -> u32 {
    if hz == 0 {
        return 60;
    }
    [144, 120, 60, 30].into_iter().find(|&f| f <= hz + 1).unwrap_or(30)
}

fn read_settings(path: &Path) -> Settings {
    let mut s: Settings = fs::read_to_string(path)
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default();
    sanitize(&mut s);
    s
}

fn write_settings(path: &Path, s: &Settings) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let bytes = serde_json::to_vec_pretty(s).map_err(|e| e.to_string())?;
    fs::write(path, bytes).map_err(|e| e.to_string())
}

// ---------- logging and events ----------

/// Keeps the in-memory log, the task's log file and the page stream in step.
fn log(sink: &Sink, line: impl Into<String>) {
    let line: String = line.into();
    let app = match sink {
        Sink::App(app) => app,
        Sink::Console(c) => {
            if let Some(f) = c.file.lock().unwrap().as_mut() {
                let _ = writeln!(f, "{line}");
            }
            return;
        }
    };
    let st = app.state::<AppState>();
    {
        let mut buf = st.log_buf.lock().unwrap();
        if buf.len() >= LOG_CAP {
            buf.pop_front();
        }
        buf.push_back(line.clone());
    }
    if let Some(f) = st.log_file.lock().unwrap().as_mut() {
        let _ = writeln!(f, "{line}");
    }
    let _ = st.log_tx.lock().unwrap().send(line);
}

/// Each step's share of the --prepare progress.
fn prepare_stage(step: &str) -> (f64, f64) {
    match step {
        "extract" => (0.0, 0.1),
        "msvc" => (0.1, 0.15),
        "configure" => (0.25, 0.1),
        _ => (0.35, 0.65),
    }
}

fn emit_step(sink: &Sink, task: &'static str, step: &'static str, state: &'static str, message: impl Into<String>) {
    let message = message.into();
    match sink {
        Sink::App(app) => {
            let _ = app.emit("step", StepEvent { task, step, state, message });
        }
        Sink::Console(c) => match state {
            "running" => {
                let (start, len) = prepare_stage(step);
                *c.stage.lock().unwrap() = (start, len, -1);
                c.print(0.0, message.trim_end_matches('.'));
            }
            "failed" => eprintln!("{message}"),
            _ => {}
        },
    }
}

fn begin_task_log(app: &AppHandle, task: &str) {
    let st = app.state::<AppState>();
    let path = st.log_dir.join(format!("{task}-{}.log", unix_stamp()));
    *st.log_file.lock().unwrap() = fs::File::create(&path).ok();
    *st.log_path.lock().unwrap() = path.display().to_string();
    st.log_buf.lock().unwrap().clear();
}

fn pump(app: Sink, reader: impl Read + Send + 'static) -> JoinHandle<()> {
    thread::spawn(move || {
        for line in BufReader::new(reader).split(b'\n').flatten() {
            let text = String::from_utf8_lossy(&line).trim_end_matches('\r').to_string();
            // "# progress <done> <total> <file>" from tools/xdvdfs.py extract.
            if let Some(rest) = text.strip_prefix("# progress ") {
                let mut it = rest.splitn(3, ' ');
                let done = it.next().and_then(|v| v.parse::<f64>().ok());
                let total = it.next().and_then(|v| v.parse::<f64>().ok());
                if let (Some(done), Some(total)) = (done, total) {
                    let file = it.next().unwrap_or("").rsplit('/').next().unwrap_or("");
                    emit_progress(&app, 100.0 * done / total.max(1.0), format!("Extracting {file}"));
                }
                continue;
            }
            // Ninja's "[n/m] ..." lines while compiling.
            if let Some((n, m)) = text
                .strip_prefix('[')
                .and_then(|r| r.split_once(']'))
                .and_then(|(nm, _)| nm.split_once('/'))
                .and_then(|(n, m)| Some((n.parse::<f64>().ok()?, m.parse::<f64>().ok()?)))
            {
                emit_progress(&app, 100.0 * n / m.max(1.0), format!("Compiling {n} of {m}"));
            }
            if !text.trim().is_empty() {
                log(&app, text);
            }
        }
    })
}

/// Runs a program in `cwd`, streaming stdout and stderr into the log. Returns whether it exited successfully.
fn run_streamed(app: &Sink, program: &str, args: &[String], cwd: &Path) -> Result<bool, String> {
    run_streamed_env(app, program, args, cwd, &[])
}

/// run_streamed with environment changes: an empty value removes the variable.
fn run_streamed_env(app: &Sink, program: &str, args: &[String], cwd: &Path, env: &[(String, String)]) -> Result<bool, String> {
    log(app, format!("> {program} {}", args.join(" ")));
    let mut cmd = Command::new(program);
    cmd.args(args).current_dir(cwd).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    for (k, v) in env {
        if v.is_empty() {
            cmd.env_remove(k);
        } else {
            cmd.env(k, v);
        }
    }
    no_window(&mut cmd);
    let mut child = cmd.spawn().map_err(|e| format!("Could not start {program}: {e}"))?;
    let mut readers = Vec::new();
    if let Some(out) = child.stdout.take() {
        readers.push(pump(app.clone(), out));
    }
    if let Some(err) = child.stderr.take() {
        readers.push(pump(app.clone(), err));
    }
    let status = child.wait().map_err(|e| e.to_string())?;
    for r in readers {
        let _ = r.join();
    }
    Ok(status.success())
}

// ---------- jobs ----------

/// Copies an already-extracted game folder into port/assets, one file at a time.
fn count_files(dir: &Path) -> usize {
    fs::read_dir(dir)
        .map(|rd| {
            rd.flatten()
                .map(|e| match e.file_type() {
                    Ok(t) if t.is_dir() => count_files(&e.path()),
                    Ok(t) if t.is_file() => 1,
                    _ => 0,
                })
                .sum()
        })
        .unwrap_or(0)
}

fn copy_tree(app: &Sink, src: &Path, dst: &Path) -> Result<usize, String> {
    let total = count_files(src).max(1);
    let mut count = 0usize;
    let mut stack = vec![src.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let rel = dir.strip_prefix(src).unwrap_or(Path::new("")).to_path_buf();
        let target_dir = dst.join(&rel);
        fs::create_dir_all(&target_dir).map_err(|e| format!("Could not create {}: {e}", target_dir.display()))?;
        for entry in fs::read_dir(&dir).map_err(|e| e.to_string())? {
            let entry = entry.map_err(|e| e.to_string())?;
            let ft = entry.file_type().map_err(|e| e.to_string())?;
            let from = entry.path();
            if ft.is_dir() {
                stack.push(from);
            } else if ft.is_file() {
                let to = target_dir.join(entry.file_name());
                fs::copy(&from, &to).map_err(|e| format!("Could not copy {}: {e}", from.display()))?;
                count += 1;
                emit_progress(app, 100.0 * count as f64 / total as f64,
                              format!("Copying {}", entry.file_name().to_string_lossy()));
                if count % 200 == 0 {
                    log(app, format!("Copied {count} files..."));
                }
            }
        }
    }
    Ok(count)
}

fn extract_job(app: &Sink, repo: &Path, source: &Path, folder: bool) -> bool {
    let assets = repo.join(ASSETS_REL);
    emit_step(app, "extract", "extract", "running", if folder { "Copying game files..." } else { "Extracting disc..." });
    let result: Result<(), String> = if folder {
        let same = match (fs::canonicalize(source), fs::canonicalize(&assets)) {
            (Ok(a), Ok(b)) => a == b,
            _ => false,
        };
        if same {
            log(app, "The folder is already port/assets.");
            Ok(())
        } else {
            copy_tree(app, source, &assets).map(|n| log(app, format!("Copied {n} files.")))
        }
    } else {
        log(app, format!("Extracting {}", source.display()));
        let mut last = String::new();
        xdvdfs::extract(source, &assets, |done, total, file| {
            emit_progress(app, 100.0 * done as f64 / total as f64, format!("Extracting {}", file.rsplit('/').next().unwrap_or(file)));
            if file != last {
                log(app, file.to_string());
                last = file.to_string();
            }
        })
        .map(|n| log(app, format!("Extracted {n} files.")))
    };
    let result = result.and_then(|_| {
        let (state, detail) = game_check(repo);
        if state == "ok" { Ok(()) } else { Err(detail) }
    });
    match result {
        Ok(()) => {
            emit_step(app, "extract", "extract", "done", "Game files verified (title 5553080F).");
            true
        }
        Err(e) => {
            log(app, format!("ERROR: {e}"));
            emit_step(app, "extract", "extract", "failed", e);
            false
        }
    }
}

const BUILD_STAGES: [(&str, &str, &str); 3] = [
    ("setup", "Set up toolchain", "tools/setup.ps1"),
    ("patch", "Patch SDK", "tools/patch-sdk.ps1"),
    ("compile", "Compile game", "tools/build.ps1"),
];

fn build_job(handle: &AppHandle, repo: &Path) -> bool {
    let app = &Sink::App(handle.clone());
    if let Some(pack) = builder::find_pack(handle, repo) {
        if game_check(repo).0 != "ok" {
            emit_step(app, "build", "msvc", "failed", "Game files are missing or not the supported version.");
            return false;
        }
        log(app, format!("Build pack: {}", pack.root.display()));
        let Some(msvc) = builder::msvc_dir(handle) else {
            emit_step(app, "build", "msvc", "failed", "No folder for the Microsoft libraries.");
            return false;
        };
        return builder::build(app, repo, &pack, &msvc);
    }
    // No build pack (a development checkout): the scripts, with the tools installed on this PC.
    let missing: Vec<String> = prereqs().into_iter().filter(|p| !p.ok).map(|p| p.name).collect();
    if !missing.is_empty() {
        let msg = format!("Missing: {}. Install them, then try again.", missing.join(", "));
        log(app, format!("ERROR: {msg}"));
        emit_step(app, "build", "setup", "failed", msg);
        return false;
    }
    if game_check(repo).0 != "ok" {
        let msg = "Game files are missing or not the supported version.";
        log(app, format!("ERROR: {msg}"));
        emit_step(app, "build", "setup", "failed", msg);
        return false;
    }
    let mut ok = true;
    for (id, label, script) in BUILD_STAGES {
        if !ok {
            emit_step(app, "build", id, "skipped", "");
            continue;
        }
        emit_step(app, "build", id, "running", format!("{label}..."));
        log(app, format!("== {label} ({script}) =="));
        let args = vec![
            "-NoProfile".to_string(),
            "-ExecutionPolicy".to_string(),
            "Bypass".to_string(),
            "-File".to_string(),
            script.to_string(),
        ];
        match run_streamed(app, "powershell", &args, repo) {
            Ok(true) => emit_step(app, "build", id, "done", format!("{label} done.")),
            Ok(false) => {
                ok = false;
                emit_step(app, "build", id, "failed", format!("{label} failed. Open the Log page for details."));
            }
            Err(e) => {
                ok = false;
                emit_step(app, "build", id, "failed", e);
            }
        }
    }
    if ok && !repo.join(EXE_REL).is_file() {
        ok = false;
        emit_step(app, "build", "compile", "failed", "The build finished but lost.exe was not produced.");
    }
    ok
}

/// Runs one job on a worker thread. Only one job runs at a time.
fn start_task(app: &AppHandle, task: &'static str, job: impl FnOnce(AppHandle) -> bool + Send + 'static) -> Result<(), String> {
    let st = app.state::<AppState>();
    if st.busy.swap(true, Ordering::SeqCst) {
        return Err("Another task is already running.".into());
    }
    begin_task_log(app, task);
    let handle = app.clone();
    thread::spawn(move || {
        let ok = job(handle.clone());
        handle.state::<AppState>().busy.store(false, Ordering::SeqCst);
        let _ = handle.emit("task-finished", TaskFinished { task, ok });
    });
    Ok(())
}

// ---------- status ----------

fn collect_status(app: &AppHandle) -> Status {
    let (settings, log_dir) = {
        let st = app.state::<AppState>();
        let s = st.settings.lock().unwrap().clone();
        (s, st.log_dir.display().to_string())
    };
    let repo = repo_root(app);
    let pack = repo.as_ref().is_some_and(|r| builder::find_pack(app, r).is_some());
    let needs_license = pack
        && !settings.msvc_license
        && !builder::msvc_dir(app).is_some_and(|d| builder::msvc_ready(&d));
    let (game_state, game_detail, built) = match &repo {
        Some(r) => {
            let (state, detail) = game_check(r);
            (state, detail, r.join(EXE_REL).is_file())
        }
        None => ("missing".to_string(), NO_REPO.to_string(), false),
    };
    Status {
        repo_root: repo.map(|p| p.display().to_string()),
        game_state,
        game_detail,
        built,
        prereqs: if pack { vec![] } else { prereqs() },
        needs_license,
        settings,
        log_dir,
        refresh_hz: refresh_hz(),
        auto_fps: auto_fps(refresh_hz()),
    }
}

// ---------- commands ----------

#[tauri::command]
async fn get_status(app: AppHandle) -> Result<Status, String> {
    tauri::async_runtime::spawn_blocking(move || collect_status(&app))
        .await
        .map_err(|e| e.to_string())
}

#[tauri::command]
fn start_extract(app: AppHandle, path: String, folder: bool) -> Result<(), String> {
    let repo = repo_root(&app).ok_or_else(|| NO_REPO.to_string())?;
    let source = PathBuf::from(&path);
    if folder {
        if !source.is_dir() {
            return Err("That folder does not exist.".into());
        }
    } else if !source.is_file() {
        return Err("That disc image does not exist.".into());
    }
    start_task(&app, "extract", move |app| extract_job(&Sink::App(app), &repo, &source, folder))
}

#[tauri::command]
fn start_build(app: AppHandle) -> Result<(), String> {
    let repo = repo_root(&app).ok_or_else(|| NO_REPO.to_string())?;
    if collect_status(&app).needs_license {
        return Err("Accept Microsoft's license terms first.".into());
    }
    start_task(&app, "build", move |app| build_job(&app, &repo))
}

#[tauri::command]
fn play(app: AppHandle) -> Result<(), String> {
    let settings = app.state::<AppState>().settings.lock().unwrap().clone();
    let repo = repo_root(&app).ok_or_else(|| NO_REPO.to_string())?;
    let port = repo.join("port");
    let exe = repo.join(EXE_REL);
    if !exe.is_file() {
        return Err("The game is not built yet. Run Build first.".into());
    }
    let mut cmd = Command::new(&exe);
    let fps = settings.fps.unwrap_or_else(|| auto_fps(refresh_hz()));
    cmd.current_dir(&port).arg(format!("--lost_target_fps={fps}"));
    // Explicit, so the game finds its files wherever the build put lost.exe.
    cmd.arg("--game_data_root").arg(repo.join(ASSETS_REL));
    cmd.arg(if settings.native_primary { "--lost_native_primary" } else { "--no-lost_native_primary" });
    cmd.arg(if settings.fullscreen { "--fullscreen" } else { "--no-fullscreen" });
    if !settings.resolution.is_empty() {
        cmd.arg(format!("--resolution={}", settings.resolution));
    }
    cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
    cmd.spawn().map_err(|e| format!("Could not start the game: {e}"))?;
    Ok(())
}

#[tauri::command]
fn save_settings(app: AppHandle, settings: Settings) -> Result<Settings, String> {
    let mut s = settings;
    sanitize(&mut s);
    s.repo_root = s.repo_root.take().map(|p| p.trim().to_string()).filter(|p| !p.is_empty());
    if let Some(p) = &s.repo_root {
        if !is_repo(Path::new(p)) {
            return Err("That folder is not the Lost: Via Domus repository. It must contain tools and port folders.".into());
        }
    }
    let st = app.state::<AppState>();
    write_settings(&st.settings_path, &s)?;
    *st.settings.lock().unwrap() = s.clone();
    Ok(s)
}

#[tauri::command]
fn get_log(app: AppHandle) -> LogView {
    let st = app.state::<AppState>();
    let lines = st.log_buf.lock().unwrap().iter().cloned().collect();
    let path = st.log_path.lock().unwrap().clone();
    LogView { path, lines }
}

#[tauri::command]
fn open_log_dir(app: AppHandle) -> Result<(), String> {
    let dir = app.state::<AppState>().log_dir.clone();
    Command::new("explorer").arg(&dir).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// RecompLauncher's setup step (recomplauncher.json "prepare"), with no window:
///   lost-launcher.exe --prepare --game <disc image or folder> [--dir <folder>] [--accept-msvc-license]
/// Unpacks the bundled source into <folder>\source (default: the launcher's folder), takes the
/// game files from the player's copy and builds lost.exe there. Prints "progress <0..1> <text>"
/// lines; the full log goes to <folder>\logs. Exit codes: 0 done, 2 bad arguments or files,
/// 3 not the supported game, 4 Microsoft's license not accepted, 5 build failed.
fn prepare(args: &[String]) -> i32 {
    let mut game: Option<PathBuf> = None;
    let mut dir: Option<PathBuf> = None;
    let mut license = false;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--game" => game = it.next().map(PathBuf::from),
            "--dir" => dir = it.next().map(PathBuf::from),
            "--accept-msvc-license" => license = true,
            other => {
                eprintln!("Unknown option {other}");
                return 2;
            }
        }
    }
    let Some(exe_dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)) else {
        return 2;
    };
    let dir = dir.unwrap_or_else(|| exe_dir.clone());
    let Some(game) = game.filter(|g| g.exists()) else {
        eprintln!("Pass the disc image or game folder with --game.");
        return 2;
    };
    let logs = dir.join("logs");
    let _ = fs::create_dir_all(&logs);
    let sink = Sink::Console(Arc::new(Console {
        file: Mutex::new(fs::File::create(logs.join(format!("prepare-{}.log", unix_stamp()))).ok()),
        stage: Mutex::new((0.0, 0.1, -1)),
    }));

    let msvc = std::env::var("LOST_MSVC_DIR").map(PathBuf::from).unwrap_or_else(|_| dir.join("msvc"));
    if !license && !builder::msvc_ready(&msvc) {
        eprintln!("Building needs Microsoft's C++ library and Windows SDK. Tick the option that accepts their license.");
        return 4;
    }
    let repo = dir.join("source");
    if let Err(e) = unpack_source(&exe_dir.join("resources/source.zip"), &repo, env!("CARGO_PKG_VERSION")) {
        log(&sink, format!("ERROR: {e}"));
        eprintln!("Could not unpack the game's source: {e}");
        return 2;
    }
    if !extract_job(&sink, &repo, &game, game.is_dir()) {
        return 3;
    }
    let candidates = std::env::var("LOST_BUILD_PACK").ok().map(PathBuf::from).into_iter().chain([exe_dir.join("resources/pack")]);
    let Some(pack) = builder::find_pack_in(candidates) else {
        eprintln!("The build tools (resources\\pack) are missing from this release.");
        return 2;
    };
    log(&sink, format!("Build pack: {}", pack.root.display()));
    if !builder::build(&sink, &repo, &pack, &msvc) {
        return 5;
    }
    println!("progress 1 Ready");
    0
}

/// `--prepare` (above), and the test hooks `--extract <iso> <out>` and `--check-generated <dir>`,
/// all without the window.
fn cli() -> Option<i32> {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("--prepare") => Some(prepare(&args[2..])),
        Some("--extract") if args.len() == 4 => {
            let mut last = -1i64;
            let r = xdvdfs::extract(Path::new(&args[2]), Path::new(&args[3]), |done, total, file| {
                let pct = (100 * done / total) as i64;
                if pct != last {
                    println!("{pct}% {file}");
                    last = pct;
                }
            });
            Some(match r {
                Ok(n) => { println!("extracted {n} files"); 0 }
                Err(e) => { println!("error: {e}"); 1 }
            })
        }
        Some("--check-generated") if args.len() == 3 => Some(match codecheck::check(Path::new(&args[2])) {
            Ok(found) => {
                for (m, n) in &found {
                    println!("{n:6} {m}");
                }
                println!("{} hazard kinds", found.len());
                i32::from(!found.is_empty())
            }
            Err(e) => { println!("error: {e}"); 2 }
        }),
        _ => None,
    }
}

fn main() {
    if let Some(code) = cli() {
        std::process::exit(code);
    }
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            let config_dir = app.path().app_config_dir()?;
            let log_dir = app.path().app_log_dir()?;
            fs::create_dir_all(&log_dir)?;
            let settings_path = config_dir.join("settings.json");
            let settings = read_settings(&settings_path);

            // Batches log lines so a chatty build cannot flood the page with one event per line.
            let (tx, rx) = mpsc::channel::<String>();
            let emitter = app.handle().clone();
            thread::spawn(move || {
                while let Ok(first) = rx.recv() {
                    let mut batch = vec![first];
                    let deadline = Instant::now() + Duration::from_millis(80);
                    while batch.len() < 500 {
                        let wait = deadline.saturating_duration_since(Instant::now());
                        match rx.recv_timeout(wait) {
                            Ok(line) => batch.push(line),
                            Err(_) => break,
                        }
                    }
                    let _ = emitter.emit("log-lines", batch);
                }
            });

            app.manage(AppState {
                busy: AtomicBool::new(false),
                settings: Mutex::new(settings),
                settings_path,
                log_dir,
                log_buf: Mutex::new(VecDeque::new()),
                log_file: Mutex::new(None),
                log_path: Mutex::new(String::new()),
                log_tx: Mutex::new(tx),
                bundled_source: if find_repo_from_exe().is_some() { None } else { unpack_bundled_source(app) },
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            get_status,
            start_extract,
            start_build,
            play,
            save_settings,
            get_log,
            open_log_dir
        ])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}
