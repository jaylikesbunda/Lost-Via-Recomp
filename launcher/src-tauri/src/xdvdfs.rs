//! Read-only XDVDFS (Xbox 360 game partition) extractor, ported from tools/xdvdfs.py.

use std::{
    fs,
    io::{Read, Seek, SeekFrom, Write},
    path::{Component, Path, PathBuf},
};

const SECTOR: u64 = 2048;
const MAGIC: &[u8] = b"MICROSOFT*XBOX*MEDIA";
/// Where the game partition starts in the image: raw, XGD2, XGD3, XGD1.
const OFFSETS: [u64; 4] = [0x0, 0xFD9_0000, 0x208_0000, 0x1830_0000];

struct Entry {
    path: String,
    sector: u32,
    size: u32,
}

fn find_base(f: &mut fs::File) -> Result<(u64, u32, u32), String> {
    for base in OFFSETS {
        let mut hdr = [0u8; 28];
        if f.seek(SeekFrom::Start(base + 32 * SECTOR)).is_err() || f.read_exact(&mut hdr).is_err() {
            continue;
        }
        if &hdr[..20] == MAGIC {
            let sector = u32::from_le_bytes(hdr[20..24].try_into().unwrap());
            let size = u32::from_le_bytes(hdr[24..28].try_into().unwrap());
            return Ok((base, sector, size));
        }
    }
    Err("This is not an Xbox 360 disc image (no XDVDFS volume found).".into())
}

/// Every file under the directory at `sector`/`size`, depth first.
fn walk(f: &mut fs::File, base: u64, sector: u32, size: u32, prefix: &str, out: &mut Vec<Entry>, depth: u32) -> Result<(), String> {
    if size == 0 || depth > 64 {
        return Ok(());
    }
    let mut data = vec![0u8; size as usize];
    f.seek(SeekFrom::Start(base + sector as u64 * SECTOR)).map_err(|e| e.to_string())?;
    f.read_exact(&mut data).map_err(|e| format!("Could not read the disc directory: {e}"))?;
    let mut seen = std::collections::HashSet::new();
    let mut stack = vec![0usize];
    while let Some(off) = stack.pop() {
        if !seen.insert(off) || off + 14 > data.len() {
            continue;
        }
        let left = u16::from_le_bytes([data[off], data[off + 1]]);
        let right = u16::from_le_bytes([data[off + 2], data[off + 3]]);
        if left == 0xFFFF && right == 0xFFFF {
            continue;
        }
        let start = u32::from_le_bytes(data[off + 4..off + 8].try_into().unwrap());
        let fsize = u32::from_le_bytes(data[off + 8..off + 12].try_into().unwrap());
        let attr = data[off + 12];
        let nlen = data[off + 13] as usize;
        let name: String = data.get(off + 14..off + 14 + nlen).unwrap_or(&[]).iter().map(|&b| b as char).collect();
        let path = format!("{prefix}{name}");
        if attr & 0x10 != 0 {
            walk(f, base, start, fsize, &format!("{path}/"), out, depth + 1)?;
        } else {
            out.push(Entry { path, sector: start, size: fsize });
        }
        if left != 0 {
            stack.push(left as usize * 4);
        }
        if right != 0 {
            stack.push(right as usize * 4);
        }
    }
    Ok(())
}

/// The output path of a disc file, refusing names that would leave `out`.
fn safe_join(out: &Path, disc_path: &str) -> Result<PathBuf, String> {
    let mut p = out.to_path_buf();
    for part in disc_path.split('/') {
        let c = Path::new(part);
        if part.is_empty() || !matches!(c.components().next(), Some(Component::Normal(_))) || c.components().count() != 1 {
            return Err(format!("Unsafe file name on the disc: {disc_path}"));
        }
        p.push(part);
    }
    Ok(p)
}

/// Extracts every file of the disc image `iso` into `out`. `progress(done_bytes, total_bytes, file)`
/// is called as data is written.
pub fn extract(iso: &Path, out: &Path, mut progress: impl FnMut(u64, u64, &str)) -> Result<usize, String> {
    let mut f = fs::File::open(iso).map_err(|e| format!("Could not open the disc image: {e}"))?;
    let (base, sector, size) = find_base(&mut f)?;
    let mut entries = Vec::new();
    walk(&mut f, base, sector, size, "", &mut entries, 0)?;
    entries.sort_by(|a, b| a.path.cmp(&b.path));
    let total: u64 = entries.iter().map(|e| e.size as u64).sum::<u64>().max(1);
    let mut done = 0u64;
    let mut buf = vec![0u8; 8 << 20];
    for e in &entries {
        let dst = safe_join(out, &e.path)?;
        if let Some(dir) = dst.parent() {
            fs::create_dir_all(dir).map_err(|err| format!("Could not create {}: {err}", dir.display()))?;
        }
        f.seek(SeekFrom::Start(base + e.sector as u64 * SECTOR)).map_err(|err| err.to_string())?;
        let mut o = fs::File::create(&dst).map_err(|err| format!("Could not write {}: {err}", dst.display()))?;
        let mut left = e.size as u64;
        while left > 0 {
            let n = left.min(buf.len() as u64) as usize;
            f.read_exact(&mut buf[..n]).map_err(|err| format!("Could not read {} from the disc: {err}", e.path))?;
            o.write_all(&buf[..n]).map_err(|err| format!("Could not write {}: {err}", dst.display()))?;
            left -= n as u64;
            done += n as u64;
            progress(done, total, &e.path);
        }
        if e.size == 0 {
            progress(done, total, &e.path);
        }
    }
    Ok(entries.len())
}
