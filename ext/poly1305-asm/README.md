# Poly1305 assembly

Hand-written Poly1305 assembly by Andy Polyakov, taken from OpenSSL. Used in
place of the poly1305-donna C implementation in `node/Poly1305.cpp` on targets
that have it, enabled by `-DZT_USE_ASM_POLY1305`.

Poly1305 is roughly a quarter of the cost of armoring a ZeroTier packet, and on
32-bit targets `node/Poly1305.cpp` falls back to poly1305-donna's 32-bit
implementation, which is slow. Measured on a 1400-byte payload:

| target | Poly1305 donna | Poly1305 asm | whole-AEAD speedup |
| --- | ---: | ---: | ---: |
| AMD Athlon II P360 (K10, no AES-NI) | 3.24 c/B | 1.47 c/B | +23.5% |
| Annapurna AL21400 (Cortex-A15, ARMv7) | 4.29 c/B | 1.72 c/B | +29.1% |

On the RB4011 that moves a single core from 1.04 to 1.34 Gbit/sec of armored
payload. Gains rise with packet size: +36% at 4096 bytes on the Cortex-A15.

The wire format does not change. This is the same Poly1305, only faster, so
nodes built with and without it interoperate.

## Short messages use the C code

Below `ZT_POLY1305_ASM_MIN_LEN` (256 bytes) `Poly1305::compute()` calls the
donna code instead. The assembly pays for three non-inlinable calls with
register-saving prologues, while donna is inlined straight into `compute()`; on
a short message that fixed cost is most of the work. Without the threshold the
assembly is *slower* below ~256 bytes -- measured at -18% for a 64-byte payload
on the Athlon before the threshold was added, which would have penalised exactly
the small ACK and heartbeat packets a busy node sends most of.

Do not "simplify" this by always calling the assembly.

## Files

| file | what |
| --- | --- |
| `poly1305-x86_64.S` | generated, x86-64 ELF, base scalar path only |
| `poly1305-armv4.S` | generated, ARMv4+ ELF, includes the NEON path |
| `arm_arch.h` | OpenSSL header the ARM assembly `#include`s |
| `*.pl`, `*-xlate.pl` | the perlasm generators the `.S` files came from |

The generators are kept so the assembly can be regenerated and diffed rather
than taken on trust:

```sh
perl poly1305-armv4.pl  linux32 poly1305-armv4.S
perl poly1305-x86_64.pl elf     poly1305-x86_64.S
```

`poly1305-x86_64.S` is generated **without** `CC` set, which yields only the
base scalar path. That is deliberate: the AVX/AVX2/AVX512 paths dispatch on
`OPENSSL_ia32cap_P`, which would drag in OpenSSL's cpuid setup, and this fork
targets CPUs that predate AVX anyway. The base path uses 64-bit `mulq` and is
what OpenSSL itself selects on such hardware. Regenerating with `CC=gcc` would
add the vector paths and the `OPENSSL_ia32cap_P` dependency along with them.

## Interface

Both `.S` files export the OpenSSL primitive interface:

```c
int  poly1305_init(void *ctx, const unsigned char key[16], void *func);
void poly1305_blocks(void *ctx, const unsigned char *inp, size_t len, unsigned int padbit);
void poly1305_emit(void *ctx, unsigned char mac[16], const unsigned int nonce[4]);
```

`poly1305_init` writes a pair of function pointers through its third argument
**unconditionally** on both targets, so it must be given a real two-pointer
struct, never `NULL`. It returns non-zero when those pointers should be used in
preference to the plain `poly1305_blocks`/`poly1305_emit` symbols; on ARM that
is how the NEON path gets selected.

`node/Poly1305.cpp` declares these under `zt_`-prefixed names bound to the real
symbols with `__asm__("...")` labels, so they cannot collide with the
identically-named static donna functions in that same file.

## OPENSSL_armcap_P

The ARM assembly reads the global `OPENSSL_armcap_P` to decide whether to take
the NEON path. Nothing else in ZeroTier defines it, so `node/Poly1305.cpp`
defines it and sets bit 0 (`ARMV7_NEON`) from ZeroTier's existing runtime NEON
check, `zt_arm_has_neon()`, before the first `poly1305_init()`.

## Licence

These files are Copyright the OpenSSL Project Authors and licensed under the
Apache License 2.0. They keep their own licence headers and are not covered by
ZeroTier's MPL-2.0.
