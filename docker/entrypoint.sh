#!/bin/bash
# Generates bnetserver.conf / worldserver.conf from the .dist files on every start
# (same keys as contrib/linux/configure_server.sh), then starts the requested server.
#
# Usage: entrypoint.sh worldserver|bnetserver|<any command>
set -euo pipefail

ROOT=/opt/trinitycore
ETC="$ROOT/etc"
CERTS="$ROOT/certs"
OVERRIDES="$ROOT/overrides"

: "${DB_HOST:=mysql}"
: "${DB_PORT:=3306}"
: "${DB_USER:=trinity}"
: "${DB_PASSWORD:?DB_PASSWORD must be set}"
: "${PUBLIC_ADDRESS:=127.0.0.1}"
: "${REALM_LOCAL_ADDRESS:=127.0.0.1}"
: "${REALM_LOCAL_SUBNET_MASK:=255.255.255.0}"
: "${REALM_NAME:=Forever}"
: "${REALM_PORT:=8085}"
: "${REALM_ID:=70}"
: "${LOGIN_HOSTNAME:=trinity.actual.battle.net}"

validate_realm_port() {
    if ! [[ "$REALM_PORT" =~ ^[0-9]+$ ]] || (( REALM_PORT < 1 || REALM_PORT > 65535 )); then
        echo "[entrypoint] ERROR: REALM_PORT must be between 1 and 65535, got '$REALM_PORT'"
        exit 1
    fi
}

# setkey <file> <key> <value>: replaces "key = ..." or appends it when the .dist has no such line
# (awk + ENVIRON, so passwords containing & \ | / are written verbatim)
setkey() {
    local file="$1" tmp
    tmp="$(mktemp)"
    K="$2" V="$3" awk '
        BEGIN { k = ENVIRON["K"]; v = ENVIRON["V"]; found = 0 }
        index($0, k) == 1 && substr($0, length(k) + 1) ~ /^[ \t]*=/ { print k " = " v; found = 1; next }
        { print }
        END { if (!found) { print ""; print k " = " v } }
    ' "$file" > "$tmp"
    cat "$tmp" > "$file"
    rm -f "$tmp"
}

# apply_overrides <conf> <name>: "Key = Value" lines from overrides/<name>.conf win over everything
apply_overrides() {
    local file="$1" src="$OVERRIDES/$2.conf" line key value
    [ -f "$src" ] || return 0
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%%$'\r'}"
        [[ "$line" =~ ^[[:space:]]*(#|$) ]] && continue
        key="$(echo "${line%%=*}" | xargs)"
        value="$(echo "${line#*=}" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"
        setkey "$file" "$key" "$value"
    done < "$src"
    echo "[entrypoint] applied overrides from $src"
}

db_info() { echo "\"${DB_HOST};${DB_PORT};${DB_USER};${DB_PASSWORD};$1\""; }

mysql_q() {
    MYSQL_PWD="$DB_PASSWORD" mysql -h"$DB_HOST" -P"$DB_PORT" -u"$DB_USER" -N -B -e "$1"
}

wait_for_mysql() {
    echo "[entrypoint] waiting for MySQL at ${DB_HOST}:${DB_PORT}..."
    until mysql_q "SELECT 1" >/dev/null 2>&1; do sleep 2; done
}

# DBC.Locale: DBC_LOCALE if set, else the first language folder found in data/dbc (the client's language)
dbc_locale() {
    local name="${DBC_LOCALE:-}" i
    local -a names=(enUS koKR frFR deDE zhCN zhTW esES esMX ruRU none ptBR itIT)
    if [ -z "$name" ]; then
        if [ -d "$ROOT/data/dbc/enUS" ]; then
            name=enUS
        else
            name="$(ls "$ROOT/data/dbc" 2>/dev/null | head -n1)"
        fi
    fi
    for i in "${!names[@]}"; do
        [ "${names[$i]}" = "$name" ] && { echo "$i"; return; }
    done
    echo 0
}

configure_worldserver() {
    local f="$ETC/worldserver.conf"
    cp "$ETC/worldserver.conf.dist" "$f"
    setkey "$f" LoginDatabaseInfo     "$(db_info auth)"
    setkey "$f" WorldDatabaseInfo     "$(db_info world)"
    setkey "$f" CharacterDatabaseInfo "$(db_info characters)"
    setkey "$f" HotfixDatabaseInfo    "$(db_info hotfixes)"
    setkey "$f" RealmID 70
    setkey "$f" Expansion 0
    setkey "$f" BindIP '"0.0.0.0"'
    setkey "$f" DataDir "\"$ROOT/data\""
    setkey "$f" LogsDir "\"$ROOT/logs\""
    setkey "$f" SourceDirectory '"/src/TrinityCore"'
    setkey "$f" MySQLExecutable '"/usr/bin/mysql"'
    setkey "$f" Network.SkipBuildAuthKeyCheck 1
    setkey "$f" Network.EnterEncryptedModeRegionGroup 8
    setkey "$f" Realm.CfgContentSetID 136
    setkey "$f" Realm.SuperDistrictID 2
    setkey "$f" Updates.EnableDatabases 15
    setkey "$f" Updates.AutoSetup 1
    setkey "$f" DBC.Locale "$(dbc_locale)"
    setkey "$f" mmap.enablePathFinding "${MMAP_PATHFINDING:-0}"
    # PACKET_LOG=1: every world packet in logs/World.pkt (diagnostics only, grows quickly; read with scripts/pkt-dump.py)
    if [ "${PACKET_LOG:-0}" = 1 ]; then
        setkey "$f" PacketLogFile '"World.pkt"'
    fi
    # ~1 million harmless DB warnings per start: file only, not console
    setkey "$f" Logger.sql.sql 5,DBErrors
    setkey "$f" Ra.Enable 0
    # SOAP: used by the account website, reachable only on the internal Docker network (port never published)
    if [ "${SOAP_ENABLED:-0}" = 1 ]; then
        setkey "$f" SOAP.Enabled 1
        setkey "$f" SOAP.IP '"0.0.0.0"'
        setkey "$f" SOAP.Port 7878
    else
        setkey "$f" SOAP.Enabled 0
    fi
    # same gameplay settings as the repack
    setkey "$f" Classic.GuildFinder 1
    setkey "$f" Classic.Shop.Enabled 1
    setkey "$f" AccountInstancesPerHour 5
    setkey "$f" PlayerSave.Stats.MinLevel 1
    # Companions (player bots): the brain switch from .env; every other Companions.* key goes in overrides/worldserver.conf
    if [ "${COMPANIONS_LLM:-0}" = 1 ]; then
        setkey "$f" Companions.LLM.Enable 1
    fi
    apply_overrides "$f" worldserver
    setkey "$f" WorldServerPort "$REALM_PORT"
}

configure_bnetserver() {
    local f="$ETC/bnetserver.conf"
    cp "$ETC/bnetserver.conf.dist" "$f"
    setkey "$f" LoginDatabaseInfo "$(db_info auth)"
    setkey "$f" BindIP '"0.0.0.0"'
    setkey "$f" LoginREST.ExternalAddress "$LOGIN_HOSTNAME"
    setkey "$f" LoginREST.LocalAddress "$LOGIN_HOSTNAME"
    setkey "$f" CertificatesFile "\"$CERTS/forever.cert.pem\""
    setkey "$f" PrivateKeyFile "\"$CERTS/forever.key.pem\""
    setkey "$f" LogsDir "\"$ROOT/logs\""
    setkey "$f" Realm.CfgContentSetID 136
    setkey "$f" Realm.SuperDistrictID 2
    apply_overrides "$f" bnetserver
}

check_data() {
    local missing=0 d
    for d in dbc maps vmaps; do
        [ -d "$ROOT/data/$d" ] || { echo "[entrypoint] ERROR: $ROOT/data/$d is missing"; missing=1; }
    done
    [ -f "$ROOT/data/vmaps/GameObjectModels.dtree" ] || { echo "[entrypoint] ERROR: vmaps/GameObjectModels.dtree is missing"; missing=1; }
    if [ "$missing" = 1 ]; then
        echo "[entrypoint] Extract dbc, gt, maps, vmaps (cameras) from the client and put them in ./data (see README)."
        exit 1
    fi
}

check_certs() {
    if [ ! -f "$CERTS/forever.cert.pem" ] || [ ! -f "$CERTS/forever.key.pem" ]; then
        echo "[entrypoint] ERROR: $CERTS/forever.cert.pem and forever.key.pem are required."
        echo "[entrypoint] Generate them with ./scripts/gen-certs.sh (see README)."
        exit 1
    fi
}

# CLIENT_BUILD (e.g. 70450): accepts a client build newer than the branch supports (experimental).
# Written as the last sql/custom/auth file, so worldserver's updater applies it (and re-applies it when the value
# changes) before the realm list and build list are loaded.
OVERRIDE_SQL=/src/TrinityCore/sql/custom/auth/2099_12_31_99_auth_client_build_override.sql
write_client_build_override() {
    if [ -z "${CLIENT_BUILD:-}" ]; then
        rm -f "$OVERRIDE_SQL"
        return 0
    fi
    if ! [[ "$CLIENT_BUILD" =~ ^[0-9]+$ ]]; then
        echo "[entrypoint] ERROR: CLIENT_BUILD must be a number (e.g. 70450), got '$CLIENT_BUILD'"
        exit 1
    fi
    cat > "$OVERRIDE_SQL" <<SQL
-- Generated by the Docker entrypoint from CLIENT_BUILD (experimental, not from the forever branch)
DELETE FROM \`build_info\` WHERE \`build\`=${CLIENT_BUILD};
INSERT INTO \`build_info\` (\`build\`,\`majorVersion\`,\`minorVersion\`,\`bugfixVersion\`,\`hotfixVersion\`) VALUES
(${CLIENT_BUILD},1,60,1,NULL);
UPDATE \`realmlist\` SET \`gamebuild\`=${CLIENT_BUILD} WHERE \`id\` IN (70,71,72,73);
SQL
    echo "[entrypoint] CLIENT_BUILD=${CLIENT_BUILD}: realm build override written (experimental)"
}

# The realm row is created by the auth updates that worldserver applies on its first start.
update_realmlist() {
    echo "[entrypoint] waiting for realm ${REALM_ID} in auth.realmlist (worldserver creates it on first start)..."
    until [ "$(mysql_q "SELECT COUNT(*) FROM auth.realmlist WHERE id=${REALM_ID}" 2>/dev/null || echo 0)" = "1" ]; do
        sleep 5
    done
    mysql_q "UPDATE auth.realmlist SET name='${REALM_NAME//\'/\'\'}', address='${PUBLIC_ADDRESS}', \
        localAddress='${REALM_LOCAL_ADDRESS}', localSubnetMask='${REALM_LOCAL_SUBNET_MASK}', \
        port=${REALM_PORT}, contentSetId=136 WHERE id=${REALM_ID}"
    echo "[entrypoint] realm ${REALM_ID}: address=${PUBLIC_ADDRESS} port=${REALM_PORT} localAddress=${REALM_LOCAL_ADDRESS}"
}

case "${1:-worldserver}" in
    worldserver)
        shift || true
        validate_realm_port
        configure_worldserver
        write_client_build_override
        check_data
        wait_for_mysql
        exec "$ROOT/bin/worldserver" -c "$ETC/worldserver.conf" "$@"
        ;;
    bnetserver)
        shift || true
        validate_realm_port
        configure_bnetserver
        check_certs
        wait_for_mysql
        update_realmlist
        exec "$ROOT/bin/bnetserver" -c "$ETC/bnetserver.conf" "$@"
        ;;
    *)
        exec "$@"
        ;;
esac
