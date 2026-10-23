# Deploying on Dokploy

[Dokploy](https://dokploy.com) runs the whole `docker-compose.yml` (MySQL, worldserver, bnetserver, website),
pulls it from your Git repository, pulls the published application images from GitHub Container Registry, and
handles the website's domain and HTTPS through its own Traefik. This is the simplest way to host it.

The TrinityCore image can still be built from source with `docker compose build worldserver`; this requires more
memory and time than pulling the prebuilt package. `BUILD_JOBS` controls the local source build.

## Server requirements

- **Disk**: ~20 GB free (the image is ~7 GB, plus the database and the build).
- **RAM**: 4 GB to play. The build needs ~2 GB per job: set `BUILD_JOBS` accordingly
  (e.g. `BUILD_JOBS=2` with 4 GB + swap, `BUILD_JOBS=6` with 16 GB).
- **Firewall / router**: forward **TCP 1119, 8081 and the `REALM_PORT`** (default `8085`) to the server. The realm and in-world game connection both use this port. Never open 3306 (MySQL) or 7878 (SOAP).

## 1. Create the application

1. *Create Service* → **Compose**.
2. Provider: your Git repository (fork of this one), branch `main`, path `docker-compose.yml`.
3. Write down the *App Name* Dokploy shows (e.g. `forever-abc123`), used in step 3.

## 2. Environment variables (*Environment* tab)

```env
MYSQL_ROOT_PASSWORD=<random>
DB_PASSWORD=<random>
SOAP_PASSWORD=<random>
PUBLIC_ADDRESS=<public IP or DNS name of the server>
REALM_NAME=Forever
REALM_PORT=8085
CLIENT_BUILD=70532
BUILD_JOBS=2
DATA_DIR=../files/data
CERTS_DIR=../files/certs
DOWNLOADS_DIR=../files/downloads
```

Generate passwords with `openssl rand -hex 16` (no `'` or `"`). See `.env.example` for the other settings
(launcher download links, sign-up rate limit...).

## 3. Copy the game data and the certificates

They are not in the repository: the data comes from your game client, the certificates are your server's own.
Dokploy keeps persistent files in `/etc/dokploy/compose/<App Name>/files/`. From the project folder on your PC:

```sh
./scripts/upload-dokploy.sh root@your-server            # finds the Dokploy application by itself
```

or by hand (replace `SERVER` and `APP`):

```sh
APP=forever-abc123
ssh root@SERVER "mkdir -p /etc/dokploy/compose/$APP/files/certs/ca"
rsync -a data/ root@SERVER:/etc/dokploy/compose/$APP/files/data/
scp certs/forever.cert.pem certs/forever.key.pem root@SERVER:/etc/dokploy/compose/$APP/files/certs/
scp certs/ca/forever_root_ca.crt root@SERVER:/etc/dokploy/compose/$APP/files/certs/ca/
```

**Never copy `certs/ca/forever_root_ca.key`**: the CA key stays on your PC.

## 4. Website domain (*Domains* tab)

Service **web**, port **8000**, your domain, HTTPS with Let's Encrypt.

## 5. Deploy

Click **Deploy**. The first time:

1. TrinityCore is compiled (30 minutes to over an hour depending on the server);
2. on its first start the worldserver imports the database (a few minutes): **do not redeploy meanwhile**,
   an interrupted import leaves a half-filled `world` database;
3. the bnetserver starts as soon as the realm exists;
4. the website creates its SOAP admin account and shows "Online" once the worldserver accepts connections.

Keep *Auto Deploy* off if your server is slow to build: every push would rebuild.

## Administration

- **Server console**: Dokploy terminal on the `worldserver` container, or over SSH
  `docker attach <App Name>-worldserver-1` (leave with `Ctrl+P`, `Ctrl+Q`).
- **Backup of accounts and characters**:
  ```sh
  docker exec <App Name>-mysql-1 sh -c 'mysqldump -uroot -p"$MYSQL_ROOT_PASSWORD" --single-transaction --databases auth characters' > backup.sql
  ```
- **Update**: *Redeploy* rebuilds the image; the worldserver applies new SQL files on start.
