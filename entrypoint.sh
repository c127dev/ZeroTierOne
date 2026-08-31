#!/bin/sh
# Generic ZeroTier One container entrypoint.
#
# Config comes from environment variables and is rewritten on every start, so
# the image carries no configuration of its own. Identity is the only state
# that must survive: it lives in ZT_IDENTITY_DIR, which is expected to be a
# mount. If no identity is there the daemon generates one and it is copied
# back, so the first start seeds the mount and later starts reuse it.
#
# Nothing secret is ever read from the environment.

set -u

D=/var/lib/zerotier-one
IDDIR="${ZT_IDENTITY_DIR:-/zt-identity}"
PORT="${ZT_PORT:-9993}"

mkdir -p "$D/networks.d" "$IDDIR"

# --- identity: mount wins, otherwise the daemon generates and we seed the mount
if [ -f "$IDDIR/identity.secret" ]; then
    cp "$IDDIR/identity.secret" "$D/identity.secret"
    [ -f "$IDDIR/identity.public" ] && cp "$IDDIR/identity.public" "$D/identity.public"
    echo "identity: loaded from $IDDIR"
else
    echo "identity: none in $IDDIR, daemon will generate one"
fi
[ -f "$D/identity.secret" ] && chmod 600 "$D/identity.secret"
[ -f "$D/identity.public" ] && chmod 644 "$D/identity.public"

# --- networks to auto-join, comma or space separated
for nw in $(echo "${ZT_NETWORKS:-}" | tr ',' ' '); do
    [ -n "$nw" ] && : > "$D/networks.d/$nw.conf" && echo "network: joining $nw"
done

# --- local.conf, regenerated from the environment every start
json_bool() {
    case "$(echo "${1:-}" | tr 'A-Z' 'a-z')" in
        1|true|yes|on) echo true ;;
        *) echo false ;;
    esac
}
json_list() {
    out=""
    for item in $(echo "${1:-}" | tr ',' ' '); do
        [ -z "$item" ] && continue
        [ -n "$out" ] && out="$out, "
        out="$out\"$item\""
    done
    echo "[$out]"
}

if [ -n "${ZT_LOCAL_CONF:-}" ]; then
    # Escape hatch: raw JSON wins over every other setting variable.
    printf '%s' "$ZT_LOCAL_CONF" > "$D/local.conf"
    echo "local.conf: taken verbatim from ZT_LOCAL_CONF"
else
    cat > "$D/local.conf" <<JSON
{
  "settings": {
    "primaryPort": ${PORT},
    "allowTcpFallbackRelay": $(json_bool "${ZT_ALLOW_TCP_FALLBACK:-true}"),
    "forceTcpRelay": $(json_bool "${ZT_FORCE_TCP_RELAY:-false}"),
    "portMappingEnabled": $(json_bool "${ZT_PORT_MAPPING:-true}"),
    "multicoreEnabled": $(json_bool "${ZT_MULTICORE:-false}"),
    "concurrency": ${ZT_CONCURRENCY:-1},
    "cpuPinningEnabled": $(json_bool "${ZT_CPU_PINNING:-false}"),
    "udpGsoEnabled": $(json_bool "${ZT_UDP_GSO:-false}"),
    "forceSalsaPeers": $(json_list "${ZT_FORCE_SALSA_PEERS:-}"),
    "interfacePrefixBlacklist": $(json_list "${ZT_IFACE_BLACKLIST:-}")
  }
}
JSON
    echo "local.conf: generated from environment"
fi

# --- routing: only when asked for, since a plain node should not forward
if [ "$(json_bool "${ZT_IP_FORWARD:-false}")" = "true" ]; then
    sysctl -w net.ipv4.ip_forward=1 >/dev/null 2>&1 \
        && echo "ip_forward: enabled" \
        || echo "ip_forward: FAILED to set"
fi

/usr/sbin/zerotier-one -p"$PORT" 2>"$D/stderr.log" &
ZTPID=$!

seeded=0
while kill -0 "$ZTPID" 2>/dev/null; do
    # Seed the mount the first time the daemon writes an identity.
    if [ "$seeded" -eq 0 ] && [ ! -f "$IDDIR/identity.secret" ] && [ -f "$D/identity.secret" ]; then
        cp "$D/identity.secret" "$IDDIR/identity.secret"
        cp "$D/identity.public" "$IDDIR/identity.public" 2>/dev/null
        chmod 600 "$IDDIR/identity.secret"
        seeded=1
        echo "identity: generated and saved to $IDDIR"
    fi

    ZTIF=$(ip -o link show | sed -n 's/.*: \(zt[a-z0-9]*\):.*/\1/p' | head -1)

    # ZT_ROUTES: "<cidr> via <gw>" entries, comma separated. Reapplied every
    # cycle because the ZeroTier interface is recreated on network changes.
    if [ -n "${ZT_ROUTES:-}" ] && [ -n "$ZTIF" ]; then
        echo "${ZT_ROUTES}" | tr ',' '\n' | while read -r spec; do
            set -- $spec
            [ "$#" -eq 3 ] && [ "$2" = "via" ] && ip route replace "$1" via "$3" dev "$ZTIF" 2>/dev/null
        done
    fi

    {
        date
        echo "ztif=$ZTIF"
        echo "ip_forward=$(cat /proc/sys/net/ipv4/ip_forward 2>/dev/null)"
        echo "--- info ---"
        /usr/sbin/zerotier-one -q info
        echo "--- networks ---"
        /usr/sbin/zerotier-one -q listnetworks
        echo "--- addr ---"
        ip -br addr
        echo "--- route ---"
        ip route
    } > "$D/status.new" 2>&1
    mv "$D/status.new" "$D/status.txt"
    sleep 15
done

wait "$ZTPID"
