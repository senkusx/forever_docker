# Instructions for AI coding assistants

This repository runs a private server for the **World of Warcraft Forever** beta, client build **1.60.1.70532**,
based on the [`forever` branch of TrinityCore](https://github.com/advocaite/TrinityCore/tree/forever). Read
`README.md` first; this file adds what an assistant needs to help someone set it up.

## Layout

| Path | What it is |
|---|---|
| `Dockerfile`, `docker/` | Builds TrinityCore from the forever branch, applies `docker/patches/*.patch`, writes the configs at start (`docker/entrypoint.sh`). |
| `docker-compose.yml` | mysql, worldserver, bnetserver, web (website), extractor (profile `tools`). `docker-compose.traefik.yml` publishes the website through Traefik. |
| `web/` | Website (FastAPI + Jinja): account sign-up through worldserver SOAP, status, launcher downloads, 4 languages (`web/app/i18n.py`); companions panel `/companions` (`companions.py`, texts in `companions_i18n.py`). |
| `app/` | Launcher (Tauri 2: Rust in `app/src-tauri/src`, UI in `app/ui`), 4 languages (`i18n.rs`, `ui/i18n.js`). |
| `scripts/` | `gen-certs.sh` (CA + server certificate), `upload-dokploy.sh`, `pkt-dump.py` (packet log reader), `gen-tactics.py` (boss tactics SQL). |
| `docker/companions/` | Companion bots: the C++ module (`src/`, copied into scripts/Custom/Companions) the optional LLM `brain/` (Claude, Ollama, OpenAI-compatible; local or remote worker via `web/app/companions_ai.py`), and the boss tactics `tactics/*.json` (turned into SQL by `scripts/gen-tactics.py`). |
| `docs/` | `DOKPLOY.md` (deployment), `MACOS.md` (how the macOS launcher works), `COMPANIONS.md` (bots). |

## Helping someone run their server

1. Linux server with Docker. `cp .env.example .env`: set the three passwords, `PUBLIC_ADDRESS` (public IP or DNS
   name), `WOW_DIR` (the "World of Warcraft" folder of a 1.60.1.70532 client, for the data extraction).
2. `./scripts/gen-certs.sh`, `docker compose build`, `docker compose run --rm extractor`, `docker compose up -d`.
3. Ports: TCP 1119, 8081, 8085 forwarded to the server; 80/443 for the website through Traefik or Dokploy.
4. Dokploy: follow `docs/DOKPLOY.md` (data and certificates go to `/etc/dokploy/compose/<app>/files/`).

## Publishing their launcher (GitHub Actions)

Explain the steps of "Publish the launcher for your server" in `README.md`: fork (and enable Actions in it), repository **variables**
`FOREVER_CA_CERT` (text of `certs/ca/forever_root_ca.crt`) and `FOREVER_SERVER_ADDRESS` (optional
`FOREVER_WEBSITE`, `FOREVER_SERVER_NAME`), then Actions > Launcher > Run workflow with a version. With the GitHub CLI:

```sh
gh variable set FOREVER_CA_CERT < certs/ca/forever_root_ca.crt
gh variable set FOREVER_SERVER_ADDRESS --body "play.example.com"
gh variable set FOREVER_WEBSITE --body "https://play.example.com"
gh workflow run launcher.yml -f version=1.0.0
```

Then `LAUNCHER_REPO=<owner>/<repo>` in the server's `.env` so the website offers the downloads.

## Rules

- **Never** commit, upload or paste `certs/ca/forever_root_ca.key`, `certs/forever.key.pem`, `.env` or passwords.
  `forever_root_ca.crt` is public and is what the launcher needs.
- Do not add Blizzard files: no game client, game data, artwork, logos or download links to the client.
- The currently supported client build is 1.60.1.70532; do not suggest ways to get the beta without access.
- Do not redeploy or restart the worldserver during its first database import (an interrupted import leaves a
  half-filled `world` database).
- If the build stops at "applying /patches/...", the forever branch probably merged that fix: check upstream and
  delete the patch rather than forcing it.
- Keep the 4 languages in sync when changing texts (FR, EN, ES, PT); `cargo test --lib` checks the Rust messages.

## Checks

```sh
docker compose config -q                                    # compose files
cd app/src-tauri && FOREVER_CA_CERT="$(cat ../../certs/ca/forever_root_ca.crt)" cargo test --lib
```
