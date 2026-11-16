//! Checks the generated C++ for two code generator hazards, ported from
//! tools/check_vector_aliasing.py:
//! * a translated instruction reading a vector register's bytes after it already overwrote them
//!   (the destination aliases a source: vpkd3d128 v0,v0 lost its sign this way);
//! * vexptefp without the underflow clamp of tools/patches/rexglue-bd833a2-vexptefp-underflow.patch.
//! The `regex` crate has no lookahead, so the three `(?!...)` checks are done by hand.

use regex::Regex;
use std::{collections::HashMap, fs, path::Path, sync::LazyLock};

const KINDS: &str = "u8|s8|u16|s16|u32|s32|f32|u64|s64|f64";

static ELEM: LazyLock<Regex> = LazyLock::new(|| Regex::new(&format!(r"ctx\.(v\d+)\.({KINDS})\[(\d+)\]")).unwrap());
static WHOLE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"ctx\.(v\d+)(\.u8)?").unwrap());
static WRITE: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(&format!(r"^\s*(ctx\.v\d+\.(?:{KINDS})\[\d+\])\s*(=|\|=|&=|\^=|\+=|-=)")).unwrap()
});
static STORE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"simde_mm_store\w*\(\(simde__m128i?\*\)ctx\.(v\d+)\.u8").unwrap());
static WHOLE_ASSIGN: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^\s*ctx\.(v\d+)\s*=").unwrap());
static CALL: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"\b(sub_[0-9A-F]+|ppc_setjmp|ppc_longjmp|REX_CALL_INDIRECT_FUNC)\s*\(|\bHostToGuest").unwrap());
static MNEMONIC: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^// [a-z][\w.]*( |$)").unwrap());

fn lane_size(kind: &str) -> u32 {
    match kind {
        "u8" | "s8" => 1,
        "u16" | "s16" => 2,
        "u32" | "s32" | "f32" => 4,
        _ => 8,
    }
}

/// Bit set of the bytes of lane `index` of a vector of `kind`.
fn byte_mask(kind: &str, index: u32) -> u32 {
    let size = lane_size(kind);
    let start = index.saturating_mul(size);
    (0..size).filter(|i| start + i < 32).fold(0, |m, i| m | 1 << (start + i))
}

fn next_is_eq(code: &str, at: usize) -> bool {
    code[at..].starts_with('=')
}

/// Whole-register uses (`ctx.v12` or `ctx.v12.u8` not followed by a word character, `.` or `[`).
fn whole_uses(rhs: &str) -> Vec<&str> {
    let mut out = Vec::new();
    for c in WHOLE.captures_iter(rhs) {
        let end = c.get(0).unwrap().end();
        let free = |at: usize| !rhs[at..].starts_with(|ch: char| ch.is_alphanumeric() || ch == '_' || ch == '.' || ch == '[');
        // With `.u8` the next character must be free; failing that, Python backtracks to the
        // bare register, which is followed by `.` and so never matches either.
        if free(end) {
            out.push(c.get(1).unwrap().as_str());
        }
    }
    out
}

/// The number of hazards in one translated instruction's lines.
fn check_block(lines: &[String]) -> usize {
    let mut written: HashMap<String, u32> = HashMap::new();
    let mut hits = 0;
    for line in lines {
        let code = line.split("//").next().unwrap_or("");
        if code.trim().is_empty() {
            continue;
        }
        if CALL.is_match(code) {
            written.clear();
            continue;
        }
        let mut rhs = code;
        let mut target: Option<(String, u32)> = None;
        if let Some(m) = WRITE.captures(code) {
            let end = m.get(0).unwrap().end();
            if !next_is_eq(code, end) {
                let t = m.get(1).unwrap().as_str();
                let e = ELEM.captures(t).unwrap();
                target = Some((e[1].to_string(), byte_mask(&e[2], e[3].parse().unwrap_or(0))));
                rhs = &code[end..];
            }
        }
        let store = STORE.captures(code).map(|c| (c[1].to_string(), c.get(0).unwrap().end()));
        if let Some((_, end)) = &store {
            rhs = &code[*end..];
        }
        let whole = WHOLE_ASSIGN.captures(code).and_then(|c| {
            let end = c.get(0).unwrap().end();
            (!next_is_eq(code, end)).then(|| (c[1].to_string(), end))
        });
        if let Some((_, end)) = &whole {
            rhs = &code[*end..];
        }
        for c in ELEM.captures_iter(rhs) {
            let w = written.get(&c[1]).copied().unwrap_or(0);
            if w & byte_mask(&c[2], c[3].parse().unwrap_or(0)) != 0 {
                hits += 1;
            }
        }
        for reg in whole_uses(rhs) {
            if written.get(reg).copied().unwrap_or(0) != 0 {
                hits += 1;
            }
        }
        if let Some((reg, mask)) = target {
            *written.entry(reg).or_default() |= mask;
        }
        if let Some((reg, _)) = store {
            *written.entry(reg).or_default() |= 0xFFFF;
        }
        if let Some((reg, _)) = whole {
            *written.entry(reg).or_default() |= 0xFFFF;
        }
    }
    hits
}

/// Scans `*_recomp.*.cpp` in `dir`. Returns the hazards found, by mnemonic.
pub fn check(dir: &Path) -> Result<Vec<(String, usize)>, String> {
    let mut files: Vec<_> = fs::read_dir(dir)
        .map_err(|e| format!("Could not read {}: {e}", dir.display()))?
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            let n = p.file_name().and_then(|n| n.to_str()).unwrap_or("");
            n.contains("_recomp.") && n.ends_with(".cpp")
        })
        .collect();
    files.sort();
    let mut found: HashMap<String, usize> = HashMap::new();
    let mut finish = |mnemonic: &Option<String>, block: &[String]| {
        let Some(m) = mnemonic else { return };
        if block.is_empty() {
            return;
        }
        let mut hits = check_block(block);
        if m.starts_with("vexptefp") && !block.iter().any(|b| b.contains("cmpunord")) {
            hits += 1;
        }
        if hits > 0 {
            *found.entry(m.split(' ').next().unwrap_or("").to_string()).or_default() += hits;
        }
    };
    for path in files {
        let bytes = fs::read(&path).map_err(|e| format!("Could not read {}: {e}", path.display()))?;
        let text = String::from_utf8_lossy(&bytes);
        let mut mnemonic: Option<String> = None;
        let mut block: Vec<String> = Vec::new();
        for line in text.lines() {
            let s = line.trim();
            if s.starts_with("// ") && MNEMONIC.is_match(s) {
                finish(&mnemonic, &block);
                mnemonic = Some(s[3..].to_string());
                block.clear();
            } else if mnemonic.is_some() {
                block.push(line.to_string());
            }
        }
        finish(&mnemonic, &block);
    }
    let mut v: Vec<_> = found.into_iter().collect();
    v.sort_by(|a, b| b.1.cmp(&a.1));
    Ok(v)
}
