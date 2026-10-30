//! Linux: the game runs under Wine/Proton; its memory is reached through /proc/<pid>/mem (allowed because the
//! launcher is an ancestor of the game with the default kernel.yama.ptrace_scope = 1).

use super::Memory;
use std::fs::File;
use std::os::unix::fs::FileExt;

pub struct ProcessMemory {
    pid: u32,
    mem: File,
}

pub fn rss_pages(pid: u32) -> u64 {
    std::fs::read_to_string(format!("/proc/{pid}/statm"))
        .ok()
        .and_then(|s| s.split_whitespace().nth(1).and_then(|v| v.parse().ok()))
        .unwrap_or(0)
}

impl ProcessMemory {
    pub fn open(pid: u32) -> Option<Self> {
        let mem = std::fs::OpenOptions::new().read(true).write(true).open(format!("/proc/{pid}/mem")).ok()?;
        Some(Self { pid, mem })
    }

    pub fn rss(&self) -> u64 {
        rss_pages(self.pid)
    }
}

impl Memory for ProcessMemory {
    fn regions(&self) -> Vec<(u64, u64)> {
        let Ok(maps) = std::fs::read_to_string(format!("/proc/{}/maps", self.pid)) else { return Vec::new() };
        maps.lines()
            .filter_map(|line| {
                let mut f = line.split_whitespace();
                let range = f.next()?;
                let perms = f.next()?;
                let inode = f.nth(2)?;
                let path = f.next().unwrap_or("");
                if perms != "rw-p" || inode != "0" || path.starts_with("[v") || path == "[stack]" {
                    return None;
                }
                let (lo, hi) = range.split_once('-')?;
                Some((u64::from_str_radix(lo, 16).ok()?, u64::from_str_radix(hi, 16).ok()?))
            })
            .collect()
    }

    fn read(&self, addr: u64, len: usize) -> Option<Vec<u8>> {
        let mut buf = vec![0u8; len];
        let n = self.mem.read_at(&mut buf, addr).ok()?;
        if n == 0 {
            return None;
        }
        buf.truncate(n);
        Some(buf)
    }

    fn write(&self, addr: u64, data: &[u8]) -> bool {
        matches!(self.mem.write_at(data, addr), Ok(n) if n == data.len())
    }

    fn alive(&self) -> bool {
        std::path::Path::new(&format!("/proc/{}", self.pid)).exists() && self.rss() > 0
    }

    fn pid(&self) -> u32 {
        self.pid
    }
}

/// `exe_name`: the game's file name in lower case (wow.exe, wowb.exe).
fn is_client(pid: u32, exe_name: &str) -> bool {
    // Wine sets argv[0] to the .exe path; on drives mounted without exec the .exe is not mapped from the file
    let argv0 = std::fs::read(format!("/proc/{pid}/cmdline"))
        .ok()
        .map(|c| String::from_utf8_lossy(c.split(|b| *b == 0).next().unwrap_or(&[])).replace('\\', "/").to_lowercase())
        .unwrap_or_default();
    let exe = std::fs::read_link(format!("/proc/{pid}/exe"))
        .ok()
        .and_then(|p| p.file_name().map(|n| n.to_string_lossy().to_lowercase()))
        .unwrap_or_default();
    let suffix = format!("/{exe_name}");
    if (argv0.ends_with(&suffix) || argv0 == exe_name) && exe.starts_with("wine") {
        return true;
    }
    std::fs::read_to_string(format!("/proc/{pid}/maps"))
        .map(|m| m.lines().any(|l| l.trim_end().to_lowercase().ends_with(&suffix)))
        .unwrap_or(false)
}

/// The game process: the Wine process of the game's executable (`exe_name`, e.g. Wow.exe) using the most memory
/// (the launcher stub uses ~30 MB).
pub fn find_client(exe_name: &str) -> Option<ProcessMemory> {
    let exe_name = exe_name.to_lowercase();
    let me = std::process::id();
    let mut best: Option<(u64, u32)> = None;
    for entry in std::fs::read_dir("/proc").ok()?.flatten() {
        let Some(pid) = entry.file_name().to_str().and_then(|s| s.parse::<u32>().ok()) else { continue };
        if pid == me || !is_client(pid, &exe_name) {
            continue;
        }
        let rss = rss_pages(pid);
        if best.map_or(true, |(r, _)| rss > r) {
            best = Some((rss, pid));
        }
    }
    best.and_then(|(_, pid)| ProcessMemory::open(pid))
}
