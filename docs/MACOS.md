# The macOS launcher

The native macOS client (`_forever_/World of Warcraft.app`, `_classic_beta_/World of Warcraft Beta.app` before
1.60.1.70685, universal arm64 + x86_64) is signed with Apple's hardened runtime, so unlike on Windows and Linux the
launcher cannot write the server key into the running game, and the key store is not stored in clear in the binary.
What the launcher does instead:

1. **Game copy**: `_forever_/World of Warcraft Forever.app` is a copy of the official app, re-signed ad hoc
   without the hardened runtime (original entitlements plus `get-task-allow`, `disable-library-validation`,
   `allow-dyld-environment-variables`). It is redone when the official executable changes (game update).
   The original app is never modified. Battle.net leaves the apps of earlier builds in the folder (a 70532
   `World of Warcraft Beta.app` next to the 70685 `World of Warcraft.app`): the launcher copies the app whose
   version is the one `.build.info` lists.
2. **Key patch module**: `app/macos/keypatch.c`, built by `build.rs` into a universal dylib embedded in the
   launcher, is loaded into the copy with `DYLD_INSERT_LIBRARIES`. It finds the key store (12 entries of 40 bytes)
   in the game's memory and writes the server's public key in the region group 8 entry, like the Windows/Linux
   patcher (`app/src-tauri/src/patcher/`).
3. **PC setup**: the hosts entry is written through `osascript ... with administrator privileges`; the CA is
   trusted in the user's default keychain with `security add-trusted-cert -r trustRoot`.
4. **Automatic login**: the login ticket goes to the `net.battle` preferences (`Launch Options/WoW/WEB_TOKEN`,
   AES-128-CBC with a PBKDF2 key derived from the client's entropy and `$USER`), then the copy is started with
   `-launcherlogin -config ForeverLauncher.wtf`.

Known issue: the very first realm login of a session can fail ("server identity check failed") because the game
creates and uses its key store before the module replaces the key. Click **Reconnect** in the game, the second
attempt works.

The app is not signed by Apple. Players run once:

```sh
xattr -cr "/Applications/Forever Launcher.app"
```
