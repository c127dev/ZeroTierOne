# Build context is a checkout of mikrozt1; this file lives on the compiler
# branch and is passed by path.
#
# Alpine, so the image is small enough for a RouterOS container on a device
# whose container store is a RAM disk: roughly 15 MB against 100 MB for the
# same build on debian-slim. musl is a supported target for the daemon; what it
# costs is zeroidc, which needs a glibc Rust toolchain, so SSO is off.

FROM alpine:3.22 AS build

RUN apk add --no-cache g++ make linux-headers

WORKDIR /src
COPY . .

# Per-board settings from boards/*.conf on the source branch.
# ZT_TUNE is compiler flags, ZT_MAKE is make variables such as ZT_ARM_NEON=1.
ARG ZT_TUNE=""
ARG ZT_MAKE=""

# A local checkout carries object files from the host toolchain, and the build
# context has no .dockerignore to keep them out.
#
# CFLAGS and CXXFLAGS use ?= in make-linux.mk, so passing them here replaces
# the optimization defaults and nothing else: the warning, include and DEFS
# flags are appended with override. ZT_SSO_SUPPORTED is a plain assignment
# there, so the command line wins.
RUN make clean \
    && make -j "$(nproc)" one ZT_SSO_SUPPORTED=0 ${ZT_MAKE} \
        CFLAGS="-O3 -fstack-protector ${ZT_TUNE}" \
        CXXFLAGS="-O3 -fstack-protector ${ZT_TUNE}" \
    && strip zerotier-one

FROM alpine:3.22

# iproute2 and iptables are what the entrypoint needs for ZT_ROUTES and
# ZT_IP_FORWARD; the rest is the C++ runtime.
RUN apk add --no-cache libstdc++ libgcc iproute2 iptables ca-certificates

COPY --from=build /src/zerotier-one /usr/sbin/zerotier-one
RUN ln -sf /usr/sbin/zerotier-one /usr/sbin/zerotier-idtool \
    && ln -sf /usr/sbin/zerotier-one /usr/sbin/zerotier-cli \
    && mkdir -p /var/lib/zerotier-one /zt-identity

COPY .compiler/entrypoint.sh /entrypoint.sh
RUN chmod 755 /entrypoint.sh

VOLUME /zt-identity
EXPOSE 9993/udp
ENTRYPOINT ["/entrypoint.sh"]
CMD []
