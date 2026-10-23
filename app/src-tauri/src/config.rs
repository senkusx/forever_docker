//! Settings (game folder, language, Linux runner...), server definition and the saved login session.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Public half of the Ed25519 key built into stock TrinityCore (used by the forever branch too).
pub const STOCK_PUBLIC_KEY: &str = "02596F0D0C061A8B30745988FD72C59E29EC367FB0F341F28E0F08D037BAFC69";
/// Public certificate of the server's private CA (never the key).
pub const CA_PEM: &str = include_str!(concat!(env!("OUT_DIR"), "/forever_root_ca.crt"));

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct Server {
    pub name: String,
    /// Host name the client and the certificate use.
    pub portal: String,
    /// Where that host name really is (IP or DNS name of the server).
    pub address: String,
    pub bnet_port: u16,
    pub rest_port: u16,
    pub world_port: u16,
    pub public_key: String,
    pub region_group: u32,
    pub website: String,
}

/// A build-time setting (FOREVER_SERVER_ADDRESS...), set by GitHub Actions from the repository variables.
fn built_in(value: Option<&'static str>, default: &str) -> String {
    value.filter(|v| !v.trim().is_empty()).unwrap_or(default).trim().to_string()
}

/// Your server, from the FOREVER_SERVER_NAME / FOREVER_SERVER_ADDRESS / FOREVER_WEBSITE variables at build time.
/// Players can still change them in Settings > Server (advanced).
impl Default for Server {
    fn default() -> Self {
        Self {
            name: built_in(option_env!("FOREVER_SERVER_NAME"), "Forever"),
            portal: "trinity.actual.battle.net".into(),
            address: built_in(option_env!("FOREVER_SERVER_ADDRESS"), "127.0.0.1"),
            bnet_port: 1119,
            rest_port: 8081,
            world_port: 8085,
            public_key: STOCK_PUBLIC_KEY.into(),
            region_group: 8,
            website: built_in(option_env!("FOREVER_WEBSITE"), ""),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct LinuxSettings {
    /// Proton folder (run through umu-run) or "wine".
    pub runner: String,
    pub prefix: String,
    pub gamescope: bool,
    pub gamescope_args: String,
    pub keyboard: String,
}

impl Default for LinuxSettings {
    fn default() -> Self {
        Self {
            runner: String::new(),
            prefix: home().join("Games/forever-wow").to_string_lossy().into_owned(),
            gamescope: false,
            gamescope_args: "-f".into(),
            keyboard: String::new(),
        }
    }
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct Settings {
    pub server: Server,
    /// The `_classic_beta_` folder (contains WowB.exe, or the game .app on macOS).
    pub game_dir: String,
    /// textLocale / audioLocale, e.g. frFR.
    pub locale: String,
    /// Launcher language: fr, en, es, pt, or empty for the system language.
    pub language: String,
    pub linux: LinuxSettings,
}

/// Login kept between launches: the 30-day launcher token of the bnetserver, never the password.
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Session {
    pub email: String,
    pub token: String,
    pub expires: u64,
}

pub const LOCALES: &[&str] = &["frFR", "enUS", "enGB", "deDE", "esES", "esMX", "itIT", "ptBR", "ruRU", "koKR", "zhCN", "zhTW"];

pub fn home() -> PathBuf {
    dirs::home_dir().unwrap_or_else(|| PathBuf::from("."))
}

pub fn config_dir() -> PathBuf {
    dirs::config_dir().unwrap_or_else(home).join("forever-launcher-app")
}

fn read_json<T: for<'de> Deserialize<'de>>(path: &Path) -> Option<T> {
    serde_json::from_slice(&std::fs::read(path).ok()?).ok()
}

fn write_private(path: &Path, data: &[u8]) -> std::io::Result<()> {
    std::fs::create_dir_all(path.parent().unwrap())?;
    let tmp = path.with_extension("tmp");
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        use std::io::Write;
        let mut f = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp)?;
        f.write_all(data)?;
    }
    #[cfg(not(unix))]
    std::fs::write(&tmp, data)?;
    std::fs::rename(tmp, path)
}

impl Settings {
    pub fn load() -> Self {
        let mut s: Settings = read_json(&config_dir().join("settings.json")).unwrap_or_default();
        if s.game_dir.is_empty() {
            s.game_dir = detect_game_dir().unwrap_or_default();
        }
        if s.locale.is_empty() {
            s.locale = installed_locale(Path::new(&s.game_dir)).unwrap_or_else(|| "enUS".into());
        }
        #[cfg(target_os = "linux")]
        {
            if s.linux.runner.is_empty() {
                s.linux.runner = crate::system::default_runner();
            }
            if s.linux.keyboard.is_empty() {
                s.linux.keyboard = crate::system::keyboard_layout();
            }
        }
        s
    }

    pub fn save(&self) -> std::io::Result<()> {
        write_private(&config_dir().join("settings.json"), &serde_json::to_vec_pretty(self).unwrap())
    }

    /// WowB.exe (Windows, Linux/Wine) or the game .app (macOS).
    pub fn exe(&self) -> PathBuf {
        game_binary(Path::new(&self.game_dir))
    }

    pub fn game_found(&self) -> bool {
        !self.game_dir.is_empty() && self.exe().exists()
    }
}

impl Session {
    pub fn load() -> Option<Self> {
        read_json::<Session>(&config_dir().join("session.json")).filter(|s| !s.token.is_empty())
    }

    pub fn save(&self) -> std::io::Result<()> {
        write_private(&config_dir().join("session.json"), &serde_json::to_vec(self).unwrap())
    }

    pub fn clear() {
        let _ = std::fs::remove_file(config_dir().join("session.json"));
    }
}

/// Language the client was installed with, from the Tags column of .build.info next to the game folder.
pub fn installed_locale(game_dir: &Path) -> Option<String> {
    let info = std::fs::read_to_string(game_dir.parent()?.join(".build.info")).ok()?;
    let line = info.lines().find(|l| l.contains("wow_classic_beta"))?;
    LOCALES.iter().find(|l| line.contains(&format!("{l} text"))).map(|l| l.to_string())
}

/// Full client version (e.g. 1.60.1.70532), from the Version column of .build.info next to the game folder.
pub fn installed_version(game_dir: &Path) -> Option<String> {
    let info = std::fs::read_to_string(game_dir.parent()?.join(".build.info")).ok()?;
    let mut lines = info.lines();
    let header: Vec<&str> = lines.next()?.split('|').map(|c| c.split('!').next().unwrap_or("")).collect();
    let version = header.iter().position(|c| *c == "Version")?;
    let product = header.iter().position(|c| *c == "Product")?;
    lines
        .map(|l| l.split('|').collect::<Vec<_>>())
        .find(|row| row.get(product) == Some(&"wow_classic_beta"))
        .and_then(|row| row.get(version).map(|v| v.to_string()))
        .filter(|v| !v.is_empty())
}

#[cfg(not(target_os = "macos"))]
fn game_binary(dir: &Path) -> PathBuf {
    dir.join("WowB.exe")
}

/// Re-signed copy of the game app that the launcher starts on macOS (see macos.rs), next to the original.
#[cfg(target_os = "macos")]
pub const MAC_COPY_NAME: &str = "World of Warcraft Forever.app";

/// The first .app in the folder (the Classic beta app name is not fixed), other than the launcher's copy.
#[cfg(target_os = "macos")]
fn game_binary(dir: &Path) -> PathBuf {
    std::fs::read_dir(dir)
        .ok()
        .and_then(|d| {
            let mut apps: Vec<PathBuf> = d
                .flatten()
                .filter(|e| {
                    let name = e.file_name().to_string_lossy().into_owned();
                    !name.starts_with('.') && name != MAC_COPY_NAME
                })
                .map(|e| e.path())
                .filter(|p| p.extension().is_some_and(|x| x == "app"))
                .collect();
            apps.sort();
            apps.into_iter().next()
        })
        .unwrap_or_else(|| dir.join("World of Warcraft.app"))
}

fn detect_game_dir() -> Option<String> {
    let mut candidates: Vec<PathBuf> = Vec::new();
    #[cfg(windows)]
    for root in ["C:\\Program Files (x86)", "C:\\Program Files", "D:\\", "D:\\Games", "E:\\", "E:\\Games"] {
        candidates.push(Path::new(root).join("World of Warcraft").join("_classic_beta_"));
    }
    #[cfg(target_os = "macos")]
    for root in ["/Applications/World of Warcraft", &home().join("Applications/World of Warcraft").to_string_lossy()] {
        candidates.push(Path::new(root).join("_classic_beta_"));
    }
    #[cfg(target_os = "linux")]
    {
        // a Lutris entry that already points at WowB.exe
        if let Ok(dir) = std::fs::read_dir(home().join(".local/share/lutris/games")) {
            for entry in dir.flatten() {
                if let Ok(text) = std::fs::read_to_string(entry.path()) {
                    for line in text.lines() {
                        if let Some(exe) = line.trim().strip_prefix("exe:") {
                            let exe = exe.trim().trim_matches(|c| c == '\'' || c == '"');
                            if exe.to_ascii_lowercase().ends_with("wowb.exe") {
                                if let Some(parent) = Path::new(exe).parent() {
                                    candidates.push(parent.to_path_buf());
                                }
                            }
                        }
                    }
                }
            }
        }
        for p in ["Games/battlenet/drive_c/Program Files (x86)/World of Warcraft/_classic_beta_",
                  "Games/World of Warcraft/_classic_beta_"] {
            candidates.push(home().join(p));
        }
    }
    candidates.into_iter().find(|p| game_binary(p).exists()).map(|p| p.to_string_lossy().into_owned())
}
