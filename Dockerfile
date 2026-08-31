# Build context is a checkout of mikrozt1; this file lives on the compiler
# branch and is passed by path.

FROM debian:trixie AS build

RUN apt-get update -qq \
    && apt-get install -y --no-install-recommends \
        build-essential make pkg-config curl ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# zeroidc is linked on the architectures where make-linux.mk sets
# ZT_SSO_SUPPORTED=1. Debian's rustc is too old for it, so take the toolchain
# from rustup.
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --profile minimal --default-toolchain stable
ENV PATH=/root/.cargo/bin:$PATH

WORKDIR /src
COPY . .

# Per-board settings from boards/*.conf on the source branch.
# ZT_TUNE is compiler flags, ZT_MAKE is make variables such as ZT_ARM_NEON=1.
ARG ZT_TUNE=""
ARG ZT_MAKE=""

# CFLAGS and CXXFLAGS use ?= in make-linux.mk, so passing them here replaces
# the optimization defaults and nothing else: the warning, include and DEFS
# flags are appended with override.
# A local checkout carries object files from the host toolchain, and the build
# context has no .dockerignore to keep them out.
RUN make clean \
    && make -j "$(nproc)" one ${ZT_MAKE} \
        CFLAGS="-O3 -fstack-protector ${ZT_TUNE}" \
        CXXFLAGS="-O3 -fstack-protector ${ZT_TUNE}"

FROM debian:trixie-slim

RUN apt-get update -qq \
    && apt-get install -y --no-install-recommends \
        iproute2 openssl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/zerotier-one /usr/sbin/zerotier-one
RUN ln -sf /usr/sbin/zerotier-one /usr/sbin/zerotier-idtool \
    && ln -sf /usr/sbin/zerotier-one /usr/sbin/zerotier-cli

COPY --from=build /src/entrypoint.sh.release /entrypoint.sh
RUN chmod 755 /entrypoint.sh

EXPOSE 9993/udp
ENTRYPOINT ["/entrypoint.sh"]
CMD []
