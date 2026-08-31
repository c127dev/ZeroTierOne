# ZeroTier One compiler branch

The image is Alpine and musl, roughly 20 MB, so it fits a RouterOS container
store that lives on a RAM disk. The entrypoint takes its whole configuration
from the environment; see "Run".

Start `c127-build.yml` from the Actions tab with this branch selected, or over HTTP
with a fine-grained token holding Actions read and write:

```bash
curl -X POST \
    -H "Accept: application/vnd.github+json" \
    -H "Authorization: Bearer $GH_TOKEN" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    https://api.github.com/repos/c127dev/ZeroTierOne/actions/workflows/c127-build.yml/dispatches \
    -d '{"ref":"compiler"}'
```

It checks out `mikrozt1` for the source and this branch for the `Dockerfile`,
then publishes `linux/amd64`, `linux/arm64`, `linux/arm/v7` and `linux/riscv64`
to GHCR and to Docker Hub, plus one tuned image per board that sets
`ZT_VARIANT`. Each architecture is built on its own runner and the four are
joined into one multi-architecture tag afterwards.

Pushing to this branch starts a run that skips every job. That run exists only
to keep the workflow registered, since a dispatch cannot reach a workflow GitHub
has never indexed.

## Tags

The version is the upstream version from `version.h` with the run number
appended, so every build gets its own: `1.16.2.7` is upstream 1.16.2 built by
run 7.

| Tag | Contents |
| --- | --- |
| `latest` | Last build |
| `<version>` | That build, for example `1.16.2.7` |
| `sha-<short>` | The commit of `mikrozt1` it was built from |
| `<variant>` | Last build tuned for that board |
| `<version>-<variant>` | That board at that version |

`latest` and `<version>` also land on `docker.io/c127/zerotierone`, copied from
the GHCR index rather than rebuilt, so the digests match. The board tags are
GHCR only. Docker Hub is skipped when `DOCKERHUB_USERNAME` and
`DOCKERHUB_TOKEN` are unset.

The release is tagged `v<version>` and titled `ZeroTier One <version>`. It
carries `zerotier-one-amd64.tar.gz`, `zerotier-one-arm64.tar.gz`,
`zerotier-one-armv7.tar.gz`, `zerotier-one-riscv64.tar.gz` and one
`zerotier-one-<variant>-tuned.tar.gz` per board, for devices that cannot pull
from a registry.

## Local build

Run from a checkout of `mikrozt1`:

```bash
git worktree add .compiler compiler
podman build -f .compiler/Dockerfile -t zerotier-one .
```

Tuned, with the flags from a board file:

```bash
. boards/mikrotik-rb4011igs.conf
podman build -f .compiler/Dockerfile \
    --build-arg ZT_TUNE="$ZT_TUNE" --build-arg ZT_MAKE="$ZT_MAKE" \
    --platform "$ZT_PLATFORM" -t zerotier-one:rb4011igs .
```

## Run

```bash
podman run -d --name zerotier-one \
    --cap-add NET_ADMIN --device /dev/net/tun \
    -v zt-identity:/zt-identity \
    -p 9993:9993/udp \
    -e ZT_NETWORKS=d3ecf5726d00d4f0 \
    -e ZT_FORCE_SALSA_PEERS=8d4814ff8f \
    ghcr.io/c127dev/zerotierone:latest
```

Only `/zt-identity` has to persist. `local.conf` is rewritten from the
environment on every start, so the image carries no configuration and a setting
is changed by restarting with a different value.

| Variable | Default | Effect |
| --- | --- | --- |
| `ZT_NETWORKS` | none | Networks to join, comma or space separated |
| `ZT_IDENTITY_DIR` | `/zt-identity` | Where the identity is kept, seeded on first start |
| `ZT_PORT` | `9993` | `primaryPort` |
| `ZT_FORCE_SALSA_PEERS` | none | `forceSalsaPeers`, the peers kept off AES-GMAC-SIV |
| `ZT_UDP_GSO` | `false` | `udpGsoEnabled` |
| `ZT_MULTICORE` | `false` | `multicoreEnabled` |
| `ZT_CONCURRENCY` | `1` | `concurrency`, only read when multicore is on |
| `ZT_CPU_PINNING` | `false` | `cpuPinningEnabled` |
| `ZT_ALLOW_TCP_FALLBACK` | `true` | `allowTcpFallbackRelay` |
| `ZT_FORCE_TCP_RELAY` | `false` | `forceTcpRelay` |
| `ZT_PORT_MAPPING` | `true` | `portMappingEnabled` |
| `ZT_IFACE_BLACKLIST` | none | `interfacePrefixBlacklist` |
| `ZT_IP_FORWARD` | `false` | `sysctl net.ipv4.ip_forward=1` |
| `ZT_ROUTES` | none | `<cidr> via <gw>` entries, reapplied every 15s |
| `ZT_LOCAL_CONF` | none | Raw `local.conf` JSON, overrides every setting above |

`ZT_ROUTES` is reapplied on a loop because the `zt*` interface is recreated
whenever network membership changes, which drops routes pinned to it.

SSO is not built (`ZT_SSO_SUPPORTED=0`): zeroidc needs a glibc Rust toolchain
and the image is musl.

## RouterOS v7

Needs the `container` package, `mode=container` in `/system/device-mode`, and a
writable `root-dir` such as a USB or NVMe partition.

```
/interface/veth/add name=veth-zt address=172.17.0.2/24 gateway=172.17.0.1
/interface/bridge/add name=containers
/interface/bridge/port/add bridge=containers interface=veth-zt
/ip/address/add address=172.17.0.1/24 interface=containers

# The host goes in registry-url, not in remote-image. RouterOS prepends it.
/container/config/set registry-url=https://ghcr.io tmpdir=usb1/pull

/container/envs/add list=ztnode key=ZT_NETWORKS value=d3ecf5726d00d4f0
/container/envs/add list=ztnode key=ZT_FORCE_SALSA_PEERS value=8d4814ff8f
/container/envs/add list=ztnode key=ZT_UDP_GSO value=true
/container/envs/add list=ztnode key=ZT_MULTICORE value=true
/container/envs/add list=ztnode key=ZT_CONCURRENCY value=4

# The identity is the only state worth keeping off the container store.
/container/mounts/add name=ztid src=/ztid dst=/zt-identity

/container/add remote-image=c127dev/zerotierone:rb4011igs \
    interface=veth-zt envlist=ztnode mountlist=ztid \
    root-dir=usb1/zerotier logging=yes
/container/start 0
```

`root-dir` on a tmpfs works and keeps the writes off the NAND, but size it for
the image: the store holds the downloaded layers and the extracted rootfs at
once. A 100 MB RAM disk is enough for this image and was not for a
debian-slim one.

An offline device takes the image as a file instead. Download
`zerotier-one-rb4011igs-tuned.tar.gz` from a release, `gunzip` it, upload it:

```
/container/add file=zerotier-one-rb4011igs-tuned.tar interface=veth-zt \
    root-dir=usb1/zerotier
```

## License

Same as the source branch, see `LICENSE.txt`.
