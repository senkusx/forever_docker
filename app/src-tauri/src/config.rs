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

/// A Battle.net product of the WoW Forever client: its folder next to Data, its code in .build.info and its Windows
/// executables (x64 first). Battle.net moved the game from the Classic beta to its own product with 1.60.1.70685.
pub struct Product {
    pub folder: &'static str,
    pub code: &'static str,
    pub exes: &'static [&'static str],
}

/// Newest first.
pub const PRODUCTS: &[Product] = &[
    Product { folder: "_forever_", code: "wowf", exes: &["Wow.exe", "Wow-ARM64.exe"] },
    Product { folder: "_classic_beta_", code: "wow_classic_beta", exes: &["WowB.exe"] },
];

/// The product of a game folder: its .flavor.info (written by Battle.net), else its name.
pub fn product(game_dir: &Path) -> &'static Product {
    let flavor = std::fs::read_to_string(game_dir.join(".flavor.info")).unwrap_or_default();
    let name = game_dir.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    PRODUCTS
        .iter()
        .find(|p| flavor.lines().skip(1).any(|l| l.trim() == p.code))
        .or_else(|| PRODUCTS.iter().find(|p| p.folder == name))
        .unwrap_or(&PRODUCTS[0])
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct Settings {
    pub server: Server,
    /// The `_forever_` folder (contains Wow.exe, or the game .app on macOS); `_classic_beta_` before 1.60.1.70685.
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
        } else if let Some(moved) = moved_game_dir(Path::new(&s.game_dir)) {
            s.game_dir = moved;
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

    /// Wow.exe or WowB.exe (Windows, Linux/Wine) or the game .app (macOS).
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

/// The row of the folder's product in .build.info next to the game folder, as (column, value) pairs.
fn build_info_row(game_dir: &Path) -> Option<Vec<(String, String)>> {
    let info = std::fs::read_to_string(game_dir.parent()?.join(".build.info")).ok()?;
    let mut lines = info.lines();
    let header: Vec<String> = lines.next()?.split('|').map(|c| c.split('!').next().unwrap_or("").to_string()).collect();
    let code = product(game_dir).code;
    lines
        .map(|l| header.iter().cloned().zip(l.split('|').map(str::to_string)).collect::<Vec<_>>())
        .find(|row| row.iter().any(|(c, v)| c == "Product" && v == code))
}

fn build_info(game_dir: &Path, column: &str) -> Option<String> {
    build_info_row(game_dir)?.into_iter().find(|(c, _)| c == column).map(|(_, v)| v).filter(|v| !v.is_empty())
}

/// Language the client was installed with, from the Tags column of .build.info next to the game folder.
pub fn installed_locale(game_dir: &Path) -> Option<String> {
    let tags = build_info(game_dir, "Tags")?;
    LOCALES.iter().find(|l| tags.contains(&format!("{l} text"))).map(|l| l.to_string())
}

/// Full client version (e.g. 1.60.1.70685), from the Version column of .build.info next to the game folder.
pub fn installed_version(game_dir: &Path) -> Option<String> {
    build_info(game_dir, "Version")
}

/// A saved folder whose product Battle.net no longer lists, while another Forever product folder next to it is
/// listed (the move from `_classic_beta_` to `_forever_`): that other folder.
fn moved_game_dir(game_dir: &Path) -> Option<String> {
    if installed_version(game_dir).is_some() {
        return None;
    }
    let root = game_dir.parent()?;
    PRODUCTS
        .iter()
        .map(|p| root.join(p.folder))
        .find(|dir| dir != game_dir && installed_version(dir).is_some() && game_binary(dir).exists())
        .map(|dir| dir.to_string_lossy().into_owned())
}

/// The product's executable in the folder (Wow.exe, or Wow-ARM64.exe on Windows on Arm), else that of another
/// product (a folder picked by hand, without .flavor.info).
#[cfg(not(target_os = "macos"))]
fn game_binary(dir: &Path) -> PathBuf {
    let own = product(dir);
    std::iter::once(own)
        .chain(PRODUCTS.iter())
        .flat_map(|p| p.exes.iter().map(|e| dir.join(e)))
        .find(|p| p.exists())
        .unwrap_or_else(|| dir.join(own.exes[0]))
}

/// Re-signed copy of the game app that the launcher starts on macOS (see macos.rs), next to the original.
#[cfg(target_os = "macos")]
pub const MAC_COPY_NAME: &str = "World of Warcraft Forever.app";

#[cfg(target_os = "macos")]
fn app_version(app: &Path) -> Option<String> {
    let out = std::process::Command::new("plutil")
        .args(["-extract", "CFBundleVersion", "raw", "-o", "-"])
        .arg(app.join("Contents/Info.plist"))
        .output()
        .ok()?;
    let v = String::from_utf8_lossy(&out.stdout).trim().to_string();
    (out.status.success() && !v.is_empty()).then_some(v)
}

/// The game app of the installed version. Battle.net leaves the apps of earlier builds in the folder (in
/// `_forever_`: World of Warcraft Beta.app of 1.60.1.70532 next to World of Warcraft.app of 70685), and the app name
/// is not fixed, so the app is chosen by version: the installed one, else the newest, never the launcher's copy.
#[cfg(target_os = "macos")]
fn game_binary(dir: &Path) -> PathBuf {
    let numbers = |v: &str| v.split('.').map(|n| n.parse::<u32>().unwrap_or(0)).collect::<Vec<_>>();
    let installed = installed_version(dir);
    let mut apps: Vec<(PathBuf, String)> = std::fs::read_dir(dir)
        .map(|d| {
            d.flatten()
                .filter(|e| {
                    let name = e.file_name().to_string_lossy().into_owned();
                    !name.starts_with('.') && name != MAC_COPY_NAME && name.ends_with(".app")
                })
                .filter_map(|e| app_version(&e.path()).map(|v| (e.path(), v)))
                .collect()
        })
        .unwrap_or_default();
    apps.sort_by(|a, b| numbers(&b.1).cmp(&numbers(&a.1)).then_with(|| a.0.cmp(&b.0)));
    apps.iter()
        .find(|(_, v)| Some(v) == installed.as_ref())
        .or(apps.first())
        .map(|(p, _)| p.clone())
        .unwrap_or_else(|| dir.join("World of Warcraft.app"))
}

fn detect_game_dir() -> Option<String> {
    #[cfg_attr(not(target_os = "linux"), allow(unused_mut))]
    let mut candidates: Vec<PathBuf> = Vec::new();
    let mut roots: Vec<PathBuf> = Vec::new();
    #[cfg(windows)]
    for root in ["C:\\Program Files (x86)", "C:\\Program Files", "D:\\", "D:\\Games", "E:\\", "E:\\Games"] {
        roots.push(Path::new(root).join("World of Warcraft"));
    }
    #[cfg(target_os = "macos")]
    roots.extend([PathBuf::from("/Applications/World of Warcraft"), home().join("Applications/World of Warcraft")]);
    #[cfg(target_os = "linux")]
    {
        // a Lutris entry that already points at the game (WowB.exe, or Wow.exe in _forever_: Wow.exe alone is retail)
        if let Ok(dir) = std::fs::read_dir(home().join(".local/share/lutris/games")) {
            for entry in dir.flatten() {
                if let Ok(text) = std::fs::read_to_string(entry.path()) {
                    for line in text.lines() {
                        if let Some(exe) = line.trim().strip_prefix("exe:") {
                            let exe = exe.trim().trim_matches(|c| c == '\'' || c == '"');
                            let lower = exe.to_ascii_lowercase().replace('\\', "/");
                            if lower.ends_with("wowb.exe") || lower.contains("/_forever_/") {
                                if let Some(parent) = Path::new(exe).parent() {
                                    // the product folder next to it is the newer one after a move
                                    if let Some(root) = parent.parent() {
                                        roots.push(root.to_path_buf());
                                    }
                                    candidates.push(parent.to_path_buf());
                                }
                            }
                        }
                    }
                }
            }
        }
        for p in ["Games/battlenet/drive_c/Program Files (x86)/World of Warcraft", "Games/World of Warcraft"] {
            roots.push(home().join(p));
        }
    }
    let mut dirs: Vec<PathBuf> = roots.iter().flat_map(|r| PRODUCTS.iter().map(move |p| r.join(p.folder))).collect();
    dirs.extend(candidates);
    let found: Vec<PathBuf> = dirs.into_iter().filter(|p| game_binary(p).exists()).collect();
    // a product Battle.net still lists first (an older one may still have its folder)
    found
        .iter()
        .find(|p| installed_version(p).is_some())
        .or(found.first())
        .map(|p| p.to_string_lossy().into_owned())
}

/// File name of the game executable (Wow.exe, WowB.exe; the .app on macOS): the patcher looks for the running game
/// by it.
pub fn exe_name(settings: &Settings) -> String {
    settings.exe().file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| PRODUCTS[0].exes[0].into())
}

#[cfg(test)]
mod tests {
    use super::*;

    const BUILD_INFO: &str = "Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|Tags!STRING:0|Version!STRING:0|Product!STRING:0\n\
        us|1|aa|OSX x86_64 US? enUS speech?:OSX x86_64 US? frFR text?|2.5.6.70006|wow_classic_era_ptr\n\
        eu|1|bb|Windows x86_64 EU? deDE speech?:Windows x86_64 EU? deDE text?|1.60.1.70685|wowf\n";

    fn install(name: &str) -> PathBuf {
        let root = std::env::temp_dir().join(format!("forever-launcher-test-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        for p in PRODUCTS {
            std::fs::create_dir_all(root.join(p.folder)).unwrap();
        }
        std::fs::write(root.join(".build.info"), BUILD_INFO).unwrap();
        std::fs::write(root.join("_forever_/.flavor.info"), "Product Flavor!STRING:0\nwowf\n").unwrap();
        root
    }

    #[test]
    fn version_and_locale_of_the_folder_product() {
        let root = install("product");
        assert_eq!(installed_version(&root.join("_forever_")).as_deref(), Some("1.60.1.70685"));
        assert_eq!(installed_locale(&root.join("_forever_")).as_deref(), Some("deDE"));
        // no longer listed by Battle.net
        assert_eq!(installed_version(&root.join("_classic_beta_")), None);
        // the folder name decides without .flavor.info
        std::fs::remove_file(root.join("_forever_/.flavor.info")).unwrap();
        assert_eq!(product(&root.join("_forever_")).code, "wowf");
        assert_eq!(product(&root.join("_classic_beta_")).code, "wow_classic_beta");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[cfg(not(target_os = "macos"))]
    #[test]
    fn moved_from_the_classic_beta() {
        let root = install("moved");
        std::fs::write(root.join("_classic_beta_/WowB.exe"), b"").unwrap();
        assert_eq!(moved_game_dir(&root.join("_classic_beta_")), None, "no game in _forever_ yet");
        std::fs::write(root.join("_forever_/Wow.exe"), b"").unwrap();
        assert_eq!(moved_game_dir(&root.join("_classic_beta_")).map(PathBuf::from), Some(root.join("_forever_")));
        assert_eq!(moved_game_dir(&root.join("_forever_")), None);
        assert_eq!(game_binary(&root.join("_forever_")), root.join("_forever_/Wow.exe"));
        assert_eq!(game_binary(&root.join("_classic_beta_")), root.join("_classic_beta_/WowB.exe"));
        let _ = std::fs::remove_dir_all(&root);
    }
}
