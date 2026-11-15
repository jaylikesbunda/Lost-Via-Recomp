#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::VecDeque,
    fs,
    io::{BufRead, BufReader, Read, Write},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::{atomic::{AtomicBool, Ordering}, mpsc, Mutex},
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
const DEFAULT_FPS: u32 = 60;
const LOG_CAP: usize = 4000;
const NO_REPO: &str = "The repository folder was not found. Set it in Settings.";
#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

#[derive(Clone, Serialize, Deserialize, Debug)]
#[serde(default)]
struct Settings {
    /// Target frame rate passed as --lost_target_fps. 0 means uncapped.
    fps: u32,
    /// Passes --lost_native_primary (experimental Direct3D 12 renderer).
    native_primary: bool,
    /// Manual repository folder when the launcher is not inside the repo.
    repo_root: Option<String>,
}

impl Default for Settings {
    fn default() -> Self {
        Settings { fps: DEFAULT_FPS, native_primary: false, repo_root: None }
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
}

#[derive(Clone, Serialize)]
struct StepEvent {
    task: &'static str,
    step: &'static str,
    state: &'static str,
    message: String,
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
    let marker = dest.join(".launcher-version");
    let version = app.package_info().version.to_string();
    if fs::read_to_string(&marker).ok().as_deref() == Some(version.as_str()) && is_repo(&dest) {
        return Some(dest);
    }
    let unpack = || -> Result<(), String> {
        fs::create_dir_all(&dest).map_err(|e| e.to_string())?;
        let file = fs::File::open(&zip_path).map_err(|e| e.to_string())?;
        let mut archive = zip::ZipArchive::new(file).map_err(|e| e.to_string())?;
        archive.extract(&dest).map_err(|e| e.to_string())?;
        fs::write(&marker, &version).map_err(|e| e.to_string())
    };
    match unpack() {
        Ok(()) => Some(dest),
        Err(e) => {
            eprintln!("could not unpack the bundled source: {e}");
            None
        }
    }
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

fn read_settings(path: &Path) -> Settings {
    let mut s: Settings = fs::read_to_string(path)
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default();
    if !FPS_CHOICES.contains(&s.fps) {
        s.fps = DEFAULT_FPS;
    }
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
fn log(app: &AppHandle, line: impl Into<String>) {
    let line: String = line.into();
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

fn emit_step(app: &AppHandle, task: &'static str, step: &'static str, state: &'static str, message: impl Into<String>) {
    let _ = app.emit("step", StepEvent { task, step, state, message: message.into() });
}

fn begin_task_log(app: &AppHandle, task: &str) {
    let st = app.state::<AppState>();
    let path = st.log_dir.join(format!("{task}-{}.log", unix_stamp()));
    *st.log_file.lock().unwrap() = fs::File::create(&path).ok();
    *st.log_path.lock().unwrap() = path.display().to_string();
    st.log_buf.lock().unwrap().clear();
}

fn pump(app: AppHandle, reader: impl Read + Send + 'static) -> JoinHandle<()> {
    thread::spawn(move || {
        for line in BufReader::new(reader).split(b'\n').flatten() {
            let text = String::from_utf8_lossy(&line).trim_end_matches('\r').to_string();
            if !text.trim().is_empty() {
                log(&app, text);
            }
        }
    })
}

/// Runs a program in `cwd`, streaming stdout and stderr into the log. Returns whether it exited successfully.
fn run_streamed(app: &AppHandle, program: &str, args: &[String], cwd: &Path) -> Result<bool, String> {
    log(app, format!("> {program} {}", args.join(" ")));
    let mut cmd = Command::new(program);
    cmd.args(args).current_dir(cwd).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
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
fn copy_tree(app: &AppHandle, src: &Path, dst: &Path) -> Result<usize, String> {
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
                if count % 200 == 0 {
                    log(app, format!("Copied {count} files..."));
                }
            }
        }
    }
    Ok(count)
}

fn extract_job(app: &AppHandle, repo: &Path, source: &Path, folder: bool) -> bool {
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
        match find_python() {
            None => Err("Python 3 was not found. Install it, then try again.".into()),
            Some((program, mut args, _)) => {
                args.extend([
                    "-I".to_string(),
                    "tools/xdvdfs.py".to_string(),
                    "extract".to_string(),
                    source.display().to_string(),
                    assets.display().to_string(),
                ]);
                match run_streamed(app, &program, &args, repo) {
                    Ok(true) => Ok(()),
                    Ok(false) => Err("Extracting the disc failed. Open the Log page for details.".into()),
                    Err(e) => Err(e),
                }
            }
        }
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

fn build_job(app: &AppHandle, repo: &Path) -> bool {
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
        prereqs: prereqs(),
        settings,
        log_dir,
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
    start_task(&app, "extract", move |app| extract_job(&app, &repo, &source, folder))
}

#[tauri::command]
fn start_build(app: AppHandle) -> Result<(), String> {
    let repo = repo_root(&app).ok_or_else(|| NO_REPO.to_string())?;
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
    cmd.current_dir(&port).arg(format!("--lost_target_fps={}", settings.fps));
    if settings.native_primary {
        cmd.arg("--lost_native_primary");
    }
    cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
    cmd.spawn().map_err(|e| format!("Could not start the game: {e}"))?;
    Ok(())
}

#[tauri::command]
fn save_settings(app: AppHandle, settings: Settings) -> Result<Settings, String> {
    let mut s = settings;
    if !FPS_CHOICES.contains(&s.fps) {
        s.fps = DEFAULT_FPS;
    }
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

fn main() {
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
