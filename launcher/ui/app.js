(function () {
  "use strict";

  const { invoke } = window.__TAURI__.core;
  const { listen } = window.__TAURI__.event;
  const openDialog = window.__TAURI__.dialog.open;
  const $ = (id) => document.getElementById(id);
  const LOG_CAP = 4000;
  const STEP_TEXT = {
    msvc: "Downloading Microsoft libraries (once)",
    configure: "Preparing the build",
    compile: "Compiling the game",
    setup: "Setting up tools",
    patch: "Patching SDK",
  };
  const ask = window.__TAURI__.dialog.ask;

  let status = null;
  let busy = null;      // "extract" | "build" | null
  let stepText = "";    // what the running task is doing
  let error = "";
  let logLines = [];

  // ---------- home ----------

  function render() {
    const s = status;
    const main = $("mainBtn");
    const alt = $("altBtn");
    alt.hidden = true;
    $("progress").hidden = !busy;
    $("logToggle").hidden = !busy && !(error && logLines.length);
    if ($("logToggle").hidden) $("log").hidden = true;
    $("logToggle").textContent = $("log").hidden ? "Show log" : "Hide log";
    $("err").hidden = !error;
    $("err").textContent = error;

    const missing = s ? s.prereqs.filter((p) => !p.ok) : [];
    $("missing").hidden = !s || busy || s.built || !missing.length;
    $("missing").innerHTML = "";
    for (const p of missing) {
      const li = document.createElement("li");
      li.textContent = p.name;
      $("missing").appendChild(li);
    }

    if (!s) return;
    if (!s.repo_root) {
      $("status").textContent = "Game folder not found. Set it in Settings.";
      main.textContent = "Play";
      main.disabled = true;
      return;
    }
    if (busy) {
      $("status").textContent = stepText || "Working...";
      main.textContent = busy === "build" ? "Building" : "Preparing";
      main.disabled = true;
      return;
    }

    const gameOk = s.game_state === "ok";
    if (!gameOk) {
      $("status").textContent = s.game_state === "wrong"
        ? "These game files are not the supported version."
        : "Choose your Xbox 360 disc image to begin.";
      main.textContent = "Choose disc";
      main.disabled = false;
      main.onclick = pickIso;
      alt.hidden = false;
      alt.textContent = "Use an extracted folder instead";
      alt.onclick = pickFolder;
    } else if (!s.built) {
      $("status").textContent = missing.length
        ? "Install the missing tools below, then build."
        : "Game files ready. The first build takes a while.";
      main.textContent = "Build";
      main.disabled = missing.length > 0;
      main.onclick = startBuild;
    } else {
      $("status").textContent = "Ready.";
      main.textContent = "Play";
      main.disabled = false;
      main.onclick = play;
      alt.hidden = false;
      alt.textContent = "Rebuild";
      alt.onclick = startBuild;
    }
  }

  async function refresh() {
    try {
      status = await invoke("get_status");
    } catch (e) {
      error = String(e);
    }
    render();
    renderSettings();
  }

  // ---------- actions ----------

  async function run(task, cmd, args) {
    setProgress(null);
    error = "";
    logLines = [];
    renderLog();
    busy = task;
    stepText = "";
    render();
    try {
      await invoke(cmd, args);
    } catch (e) {
      busy = null;
      error = String(e);
      render();
    }
  }

  async function pickIso() {
    const path = await openDialog({
      multiple: false,
      title: "Choose your Lost: Via Domus disc image",
      filters: [{ name: "Xbox 360 disc image", extensions: ["iso", "xiso"] }],
    });
    if (path) run("extract", "start_extract", { path: String(path), folder: false });
  }

  async function pickFolder() {
    const path = await openDialog({ directory: true, multiple: false, title: "Choose the extracted game folder" });
    if (path) run("extract", "start_extract", { path: String(path), folder: true });
  }

  async function startBuild() {
    // The first build downloads Microsoft's C++ library and Windows SDK, which come with
    // Microsoft's license terms (the Visual Studio Build Tools license).
    if (status && status.needs_license) {
      const ok = await ask(
        "Building the game needs Microsoft's C++ library and Windows SDK. They are downloaded once " +
        "from Microsoft (about 700 MB) and used only to build the game on this PC.\n\n" +
        "By continuing you accept Microsoft's license terms for the Visual Studio Build Tools " +
        "(visualstudio.microsoft.com/license-terms).",
        { title: "Microsoft license", kind: "info", okLabel: "Accept and build", cancelLabel: "Cancel" });
      if (!ok) return;
      try {
        status.settings = await invoke("save_settings", { settings: { ...status.settings, msvc_license: true } });
        status.needs_license = false;
      } catch (e) {
        error = String(e);
        render();
        return;
      }
    }
    run("build", "start_build", {});
  }

  async function play() {
    error = "";
    try {
      await invoke("play");
    } catch (e) {
      error = String(e);
    }
    render();
  }

  // ---------- log ----------

  function renderLog() {
    const box = $("log");
    const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 30;
    box.textContent = logLines.join("\n");
    if (atBottom) box.scrollTop = box.scrollHeight;
  }

  $("logToggle").addEventListener("click", () => {
    const show = $("log").hidden;
    $("log").hidden = !show;
    $("logToggle").textContent = show ? "Hide log" : "Show log";
    if (show) renderLog();
  });

  // ---------- settings ----------

  function renderSettings() {
    if (!status) return;
    const s = status.settings;
    document.querySelectorAll("#fpsSeg button").forEach((b) =>
      b.classList.toggle("on", b.dataset.fps === "auto" ? s.fps == null : Number(b.dataset.fps) === s.fps));
    $("fpsAuto").textContent = "Auto (" + status.auto_fps + ")";
    $("fpsAuto").title = status.refresh_hz ? "Your monitor runs at " + status.refresh_hz + " Hz" : "";
    document.querySelectorAll("#modeSeg button").forEach((b) =>
      b.classList.toggle("on", (b.dataset.mode === "full") === !!s.fullscreen));
    document.querySelectorAll("#resSeg button").forEach((b) => b.classList.toggle("on", b.dataset.res === (s.resolution || "")));
    $("nativeToggle").checked = !!s.native_primary;
    $("repoPath").textContent = status.repo_root || "Not found";
  }

  async function save(next) {
    $("settingsErr").hidden = true;
    try {
      status.settings = await invoke("save_settings", { settings: next });
      await refresh();
    } catch (e) {
      $("settingsErr").hidden = false;
      $("settingsErr").textContent = String(e);
    }
  }

  function showSettings(on) {
    $("settings").hidden = !on;
    $("home").hidden = on;
    $("settingsBtn").hidden = on;
  }

  $("settingsBtn").addEventListener("click", () => showSettings(true));
  $("backBtn").addEventListener("click", () => showSettings(false));
  document.querySelectorAll("#fpsSeg button").forEach((b) =>
    b.addEventListener("click", () =>
      save({ ...status.settings, fps: b.dataset.fps === "auto" ? null : Number(b.dataset.fps) })));
  document.querySelectorAll("#modeSeg button").forEach((b) =>
    b.addEventListener("click", () => save({ ...status.settings, fullscreen: b.dataset.mode === "full" })));
  document.querySelectorAll("#resSeg button").forEach((b) =>
    b.addEventListener("click", () => save({ ...status.settings, resolution: b.dataset.res })));
  $("nativeToggle").addEventListener("change", (e) => save({ ...status.settings, native_primary: e.target.checked }));
  $("repoPick").addEventListener("click", async () => {
    const path = await openDialog({ directory: true, multiple: false, title: "Choose the Lost: Via Domus folder" });
    if (path) save({ ...status.settings, repo_root: String(path) });
  });
  $("openLogs").addEventListener("click", () => invoke("open_log_dir").catch(() => {}));

  // ---------- backend events ----------

  function setProgress(percent, text) {
    const known = percent != null;
    $("bar").classList.toggle("known", known);
    $("bar").querySelector("i").style.width = known ? Math.max(1, Math.min(100, percent)).toFixed(1) + "%" : "";
    $("pct").textContent = known ? Math.floor(percent) + "%  " + (text || "") : "";
  }

  listen("progress", (e) => setProgress(e.payload.percent, e.payload.text));

  listen("step", (e) => {
    const p = e.payload;
    if (p.state === "running") {
      setProgress(null);
      stepText = p.task === "extract" ? p.message : (STEP_TEXT[p.step] || p.message);
      stepText = stepText.replace(/\.+$/, "") + "...";
    } else if (p.state === "failed") {
      error = p.message;
    }
    render();
  });

  listen("log-lines", (e) => {
    logLines.push(...e.payload);
    if (logLines.length > LOG_CAP) logLines.splice(0, logLines.length - LOG_CAP);
    if (!$("log").hidden) renderLog();
  });

  listen("task-finished", async () => {
    busy = null;
    stepText = "";
    await refresh();
  });

  refresh();
})();
