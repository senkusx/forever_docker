# Forever

A ready-to-run private server for the **World of Warcraft Forever** beta, client build **1.60.1.70532**, with an
account sign-up website and a launcher for Windows, Linux and macOS.

I made it to play the free beta with my friends. It is built on the
[`forever` branch of TrinityCore](https://github.com/advocaite/TrinityCore/tree/forever), packaged with Docker, plus
a few fixes of my own found with packet logs.

![In game](docs/screenshots/game.jpg)

| Launcher | Website |
|---|---|
| ![Launcher](docs/screenshots/launcher.png) | ![Website](docs/screenshots/website.png) |
| ![Launcher login](docs/screenshots/launcher-login.png) | ![Character selection](docs/screenshots/game-characters.jpg) |

## How it works

1. **The server** runs in Docker: MySQL, `worldserver`, `bnetserver` (Battle.net login over TLS) and the website.
2. **Players create their account on the website** (open sign-up, rate limited per IP). The website asks the
   worldserver to create it, exactly like the `bnetaccount create` console command.
3. **Players download the launcher** from the website, which offers the right file for their system. On first use
   the launcher prepares the PC: it adds the server to the hosts file and trusts the server's certificate.
4. **They log in once in the launcher and click Play**. The launcher remembers them for 30 days, logs them into
   the game automatically and puts the server's key in the game client (in memory, the game files are not changed).

The launcher and the website are in English, French, Spanish and Portuguese (Latin America), following the system
or browser language.

**Requirements on the player side**: the game client in exactly build **1.60.1.70532** (World of Warcraft Forever
beta, installed with Battle.net). Another build will not connect.

**No Blizzard assets**: this repository contains no game client, game data, artwork or logo (only screenshots of the
game running on this server). The launcher and the website use neutral visuals drawn in CSS. The game data the
server needs is extracted from your own client.

## Run the server

You need a Linux server with **Docker** and Docker Compose, 8 GB of RAM to build (4 GB to run), about 20 GB of disk,
and the game client (to extract its data).

```sh
git clone <this repository> forever && cd forever
cp .env.example .env               # passwords, PUBLIC_ADDRESS (public IP or DNS name), WOW_DIR
./scripts/gen-certs.sh             # your own CA + server certificate (never share certs/ca/forever_root_ca.key)
docker compose build               # compiles TrinityCore, takes a while
docker compose run --rm extractor  # extracts the game data from your client into ./data
docker compose up -d
docker compose logs -f worldserver # the first start imports the database (a few minutes)
```

### Ports

Forward these ports to the server on your router / firewall (**TCP only**, no UDP needed):

| Port | Service | What for |
|---|---|---|
| 1119 | bnetserver | Battle.net login (TLS) |
| 8081 | bnetserver | login REST API (launcher login, game login ticket) |
| 8085 (default) | worldserver | realm and in-world game connections; set `REALM_PORT` to change the published and container port |
| 80, 443 | Traefik / Dokploy | the website (HTTP for Let's Encrypt, HTTPS), only if you publish it |

Never expose MySQL (3306) or SOAP (7878): they stay on the internal Docker network.

### Docker images

The TrinityCore server/extractor and website images are published to
[GitHub Container Registry](https://github.com/senkusx?tab=packages&repo_name=forever_docker) for Linux `amd64`
and `arm64`. Start or update with `docker compose pull && docker compose up -d`; Docker Compose uses the published
`latest` images by default. To build them locally instead, run `docker compose build` (the TrinityCore build takes
a while). You can override the defaults with `TRINITY_IMAGE` and `WEB_IMAGE`.

### The website

It listens on `127.0.0.1:8088`. To publish it on a domain with HTTPS:

- **Traefik**: set `SITE_DOMAIN` in `.env` (and the Traefik network/entrypoint/resolver names if yours differ), then
  ```sh
  docker compose -f docker-compose.yml -f docker-compose.traefik.yml up -d
  ```
- **Dokploy** (what I use, it is simpler): Dokploy builds and runs the compose file from your repository and handles
  the domain with its own Traefik. Step by step: [docs/DOKPLOY.md](docs/DOKPLOY.md).

### Accounts from the console

```sh
docker compose attach worldserver   # leave with Ctrl+P, Ctrl+Q
bnetaccount create mail@example.com Password
```

## Publish the launcher for your server

The launcher is a [Tauri](https://tauri.app) app in `app/` (Rust + HTML/CSS/JS). It holds your server's address
and certificate, so each server publishes its own build. GitHub does it for you, no code to edit:

1. **Fork** this repository on GitHub, then open the **Actions** tab of your fork and enable the workflows
   (GitHub turns them off on forks).
2. In your fork, **Settings > Secrets and variables > Actions > Variables**, add these repository variables:

   | Variable | Value |
   |---|---|
   | `FOREVER_CA_CERT` | the whole text of `certs/ca/forever_root_ca.crt` (the public certificate, **never** the `.key`) |
   | `FOREVER_SERVER_ADDRESS` | public IP or DNS name of your server (`PUBLIC_ADDRESS` in `.env`) |
   | `FOREVER_WEBSITE` | optional, `https://...` of your website (sign-up link and news in the launcher) |
   | `FOREVER_SERVER_NAME` | optional, realm name shown in the launcher (default `Forever`) |

3. **Actions > Launcher > Run workflow**, type a version (e.g. `1.0.0`) and run. About 15 minutes later the
   release **v1.0.0** holds the Windows installer, the Linux AppImage and the macOS dmg. (Pushing a `v*` tag works
   too; without a version, the builds are only kept on the run page.)
4. Point your website at it: `LAUNCHER_REPO=<you>/<fork>` in `.env` (plus `GITHUB_TOKEN` if the fork is private).
   The website then offers each visitor the right file. Other options in `.env.example`.

Players can still change the server in Settings > Server (advanced).

**Building by hand**: set the same variables in your shell (or copy the certificate to
`app/src-tauri/resources/forever_root_ca.crt`), then `cd app && npx @tauri-apps/cli@2 build`
(Linux needs `libwebkit2gtk-4.1-dev`).

The macOS launcher works differently because of Apple's protections: [docs/MACOS.md](docs/MACOS.md). It is not
signed by Apple, so players run `xattr -cr "/Applications/Forever Launcher.app"` once.

## Good to know

- **Fixes on top of the forever branch**: `docker/patches/` (friend list and names with surnames), each described
  in [its README](docker/patches/README.md). The current branch already includes 360 action-button support.
- **Config overrides**: `Key = Value` lines in `overrides/worldserver.conf` or `overrides/bnetserver.conf`.
- **Update**: `docker compose build --no-cache && docker compose up -d`; new SQL updates are applied on start.
  If the branch merges one of the patches, the build stops at that patch: delete it.
- **Using an AI assistant**: [AGENTS.md](AGENTS.md) explains the project and the steps to it.
- **Newer game client**: set `CLIENT_BUILD` in `.env` to the new build number (experimental until the branch
  supports it).

## License

GNU GPL v2.0 or later, like TrinityCore. See [LICENSE](LICENSE).

Not affiliated with or endorsed by Blizzard Entertainment. World of Warcraft is a trademark of Blizzard
Entertainment, Inc. You need your own access to the game.
