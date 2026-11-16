//! Builds the game from the build pack (tools/make_build_pack.ps1): the same steps as
//! tools/build.ps1, with the pack's compiler, CMake and Ninja, and Microsoft's C++ library and
//! Windows SDK fetched by xwin. Nothing else is needed on the player's PC.

use super::{codecheck, emit_progress, emit_step, log, run_streamed_env, Sink};
use std::{
    fs,
    path::{Path, PathBuf},
};
use tauri::{AppHandle, Manager};

const PRESET: &str = "win-amd64-relwithdebinfo";
/// Written into the build folder once it was configured from a pack. A folder configured another
/// way (Visual Studio, an older pack) has its CMake cache cleared first.
const PACK_MARK: &str = "launcher-pack.txt";
const PACK_VERSION: &str = "1";

pub struct Pack {
    pub root: PathBuf,
}

impl Pack {
    fn p(&self, rel: &str) -> PathBuf {
        self.root.join(rel)
    }
    fn valid(root: &Path) -> bool {
        root.join("llvm/bin/clang++.exe").is_file()
            && root.join("cmake/bin/cmake.exe").is_file()
            && root.join("rexglue-sdk/win-amd64").is_dir()
            && root.join("codegen/rexgluerd.exe").is_file()
    }
}

/// The installer's pack, a pack built into this checkout, or $LOST_BUILD_PACK.
pub fn find_pack(app: &AppHandle, repo: &Path) -> Option<Pack> {
    let mut candidates = Vec::new();
    if let Ok(p) = std::env::var("LOST_BUILD_PACK") {
        candidates.push(PathBuf::from(p));
    }
    if let Ok(r) = app.path().resource_dir() {
        candidates.push(r.join("resources").join("pack"));
    }
    candidates.push(repo.join("launcher/src-tauri/resources/pack"));
    find_pack_in(candidates)
}

/// The first valid pack among `candidates`.
pub fn find_pack_in(candidates: impl IntoIterator<Item = PathBuf>) -> Option<Pack> {
    candidates.into_iter().find(|c| Pack::valid(c)).map(|root| Pack { root })
}

/// Where xwin puts Microsoft's libraries (once per PC).
pub fn msvc_dir(app: &AppHandle) -> Option<PathBuf> {
    if let Ok(p) = std::env::var("LOST_MSVC_DIR") {
        return Some(PathBuf::from(p));
    }
    app.path().app_local_data_dir().ok().map(|d| d.join("msvc"))
}

pub fn msvc_ready(msvc: &Path) -> bool {
    msvc.join("crt/include/vector").is_file() && msvc.join("sdk/include/um/windows.h").is_file()
}

fn build_dir(repo: &Path) -> PathBuf {
    std::env::var("LOST_BUILD_DIR").map(PathBuf::from).unwrap_or_else(|_| repo.join("port/out/build").join(PRESET))
}

/// PATH, INCLUDE and LIB for the pack's compiler and xwin's libraries. Nothing from Visual Studio.
fn env(pack: &Pack, msvc: &Path) -> Vec<(String, String)> {
    let sys = std::env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".into());
    let join = |v: Vec<PathBuf>| v.iter().map(|p| p.display().to_string()).collect::<Vec<_>>().join(";");
    let path = join(vec![
        pack.p("llvm/bin"),
        pack.p("ninja"),
        pack.p("cmake/bin"),
        PathBuf::from(&sys).join("System32"),
        PathBuf::from(&sys),
    ]);
    let include = join(
        ["crt/include", "sdk/include/ucrt", "sdk/include/um", "sdk/include/shared", "sdk/include/winrt", "sdk/include/cppwinrt"]
            .iter()
            .map(|r| msvc.join(r))
            .collect(),
    );
    let lib = join(["crt/lib/x86_64", "sdk/lib/um/x86_64", "sdk/lib/ucrt/x86_64"].iter().map(|r| msvc.join(r)).collect());
    vec![
        ("PATH".into(), path),
        ("INCLUDE".into(), include),
        ("LIB".into(), lib),
        // Keep a Visual Studio on this PC, if any, out of the build.
        ("VCToolsInstallDir".into(), String::new()),
        ("VSINSTALLDIR".into(), String::new()),
        ("VCINSTALLDIR".into(), String::new()),
    ]
}

/// A path as CMake wants it in -D values: forward slashes (a backslash starts an escape).
fn cm(p: &Path) -> String {
    p.display().to_string().replace('\\', "/")
}

fn jobs() -> String {
    let n = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    n.saturating_sub(2).max(1).to_string()
}

fn run(app: &Sink, program: &Path, args: &[String], cwd: &Path, env: &[(String, String)]) -> Result<(), String> {
    match run_streamed_env(app, &program.display().to_string(), args, cwd, env) {
        Ok(true) => Ok(()),
        Ok(false) => Err(format!("{} failed. Open the log for details.", program.file_name().unwrap_or_default().to_string_lossy())),
        Err(e) => Err(e),
    }
}

fn step<T>(app: &Sink, id: &'static str, label: &str, f: impl FnOnce() -> Result<T, String>) -> Result<T, String> {
    emit_step(app, "build", id, "running", format!("{label}..."));
    log(app, format!("== {label} =="));
    match f() {
        Ok(v) => {
            emit_step(app, "build", id, "done", format!("{label} done."));
            Ok(v)
        }
        Err(e) => {
            log(app, format!("ERROR: {e}"));
            emit_step(app, "build", id, "failed", e.clone());
            Err(e)
        }
    }
}

fn fetch_msvc(app: &Sink, pack: &Pack, msvc: &Path) -> Result<(), String> {
    if msvc_ready(msvc) {
        log(app, format!("Microsoft libraries already in {}", msvc.display()));
        return Ok(());
    }
    if msvc.exists() {
        fs::remove_dir_all(msvc).map_err(|e| format!("Could not clear {}: {e}", msvc.display()))?;
    }
    // xwin moves unpacked files into place, so its cache must be on the output's drive.
    let cwd = msvc.parent().unwrap_or(msvc);
    let cache = cwd.join("xwin-cache");
    fs::create_dir_all(cwd).map_err(|e| e.to_string())?;
    let mut args: Vec<String> = ["--accept-license", "--http-retry", "3", "--arch", "x86_64", "--variant", "desktop", "--cache-dir"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    args.push(cache.display().to_string());
    args.extend(["splat".to_string(), "--disable-symlinks".to_string(), "--output".to_string(), msvc.display().to_string()]);
    let result = run(app, &pack.p("xwin/xwin.exe"), &args, cwd, &[]);
    let _ = fs::remove_dir_all(&cache);
    // Its last step links SDK version folders to their parent, which Windows refuses without
    // admin rights or developer mode; the files themselves are all in place by then.
    if let Err(e) = result {
        if !msvc_ready(msvc) {
            return Err(e);
        }
        log(app, "xwin could not create its folder links; the libraries are complete without them.");
    }
    if !msvc_ready(msvc) {
        return Err("The Microsoft libraries download did not complete.".into());
    }
    Ok(())
}

/// The code generator's output and the files it rewrites that must keep their contents.
fn regenerate_patched(app: &Sink, pack: &Pack, port: &Path, env: &[(String, String)]) -> Result<(), String> {
    let glue = port.join("generated/rexglue.cmake");
    let manifest = port.join("lost_manifest.toml");
    let keep: Vec<(PathBuf, Option<Vec<u8>>)> = [glue, manifest.clone()].into_iter().map(|p| { let b = fs::read(&p).ok(); (p, b) }).collect();
    let args = vec!["codegen".to_string(), "--ignore-stamp".to_string(), manifest.display().to_string()];
    let result = run(app, &pack.p("codegen/rexgluerd.exe"), &args, port, env);
    // The source-built generator reports SDK 0.10.1 and rewrites these to require it; the game
    // links the prebuilt 0.10.0 runtime, so they keep their original contents.
    for (p, bytes) in keep {
        if let Some(b) = bytes {
            let _ = fs::write(&p, b);
        }
    }
    result
}

fn hazards(app: &Sink, generated: &Path) -> Result<bool, String> {
    let found = codecheck::check(generated)?;
    for (m, n) in &found {
        log(app, format!("{n:6} {m}"));
    }
    Ok(!found.is_empty())
}

pub fn build(app: &Sink, repo: &Path, pack: &Pack, msvc: &Path) -> bool {
    let port = repo.join("port");
    let bdir = build_dir(repo);
    let env = env(pack, msvc);
    let cmake = pack.p("cmake/bin/cmake.exe");

    if step(app, "msvc", "Downloading Microsoft libraries", || fetch_msvc(app, pack, msvc)).is_err() {
        return false;
    }

    let generated = port.join("generated/default");
    let prepared = step(app, "configure", "Preparing the build", || {
        let mark = bdir.join(PACK_MARK);
        if fs::read_to_string(&mark).ok().as_deref() != Some(PACK_VERSION) {
            let _ = fs::remove_file(bdir.join("CMakeCache.txt"));
            let _ = fs::remove_dir_all(bdir.join("CMakeFiles"));
        }
        let llvm = pack.p("llvm/bin");
        let configure: Vec<String> = vec![
            "--preset".into(),
            PRESET.into(),
            format!("-DCMAKE_PREFIX_PATH={}", cm(&pack.p("rexglue-sdk/win-amd64"))),
            format!("-DLOST_REXGLUE_SDK_SRC={}", cm(&pack.p("sdk-src"))),
            "-DCMAKE_C_FLAGS=-march=x86-64-v3".into(),
            "-DCMAKE_CXX_FLAGS=-march=x86-64-v3".into(),
            "-DCMAKE_LINKER_TYPE=LLD".into(),
            // CMake's compiler checks build in Debug, which needs the debug runtime xwin leaves out.
            "-DCMAKE_TRY_COMPILE_CONFIGURATION=Release".into(),
            format!("-DCMAKE_RC_COMPILER={}", cm(&llvm.join("llvm-rc.exe"))),
            format!("-DCMAKE_MT={}", cm(&llvm.join("llvm-mt.exe"))),
            format!("-DCMAKE_AR={}", cm(&llvm.join("llvm-ar.exe"))),
            "-B".into(),
            bdir.display().to_string(),
        ];
        run(app, &cmake, &configure, &port, &env)?;
        fs::write(&mark, PACK_VERSION).map_err(|e| e.to_string())?;

        let first_time = !generated.join("sources.cmake").is_file();
        let codegen: Vec<String> =
            vec!["--build".into(), bdir.display().to_string(), "--parallel".into(), jobs(), "--target".into(), "lost_codegen".into()];
        run(app, &cmake, &codegen, &port, &env)?;
        if hazards(app, &generated)? {
            log(app, "Regenerating with the patched code generator (vpkd3d128 aliasing, vexptefp underflow)");
            regenerate_patched(app, pack, &port, &env)?;
            if hazards(app, &generated)? {
                return Err("The patched code generator still left vector hazards.".into());
            }
        }
        if first_time {
            run(app, &cmake, &configure, &port, &env)?;
        }
        Ok(())
    });
    if prepared.is_err() {
        emit_step(app, "build", "compile", "skipped", "");
        return false;
    }

    let compiled = step(app, "compile", "Compiling the game", || {
        emit_progress(app, 0.0, "Compiling");
        let args: Vec<String> = vec!["--build".into(), bdir.display().to_string(), "--parallel".into(), jobs()];
        run(app, &cmake, &args, &port, &env)?;
        // The patched Xenos plugin, and the VC++ runtime so players need not install it.
        for src in [pack.p("plugin")].into_iter().chain(std::iter::once(pack.p("vcrt"))) {
            for e in fs::read_dir(&src).map_err(|e| e.to_string())?.flatten() {
                fs::copy(e.path(), bdir.join(e.file_name())).map_err(|err| format!("Could not copy {}: {err}", e.path().display()))?;
            }
        }
        if !bdir.join("lost.exe").is_file() {
            return Err("The build finished but lost.exe was not produced.".into());
        }
        Ok(())
    });
    compiled.is_ok()
}
