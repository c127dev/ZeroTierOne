# Poly1305 assembly

Poly1305 assembly by Andy Polyakov, from OpenSSL. Replaces the poly1305-donna C
code in `node/Poly1305.cpp` when built with `-DZT_USE_ASM_POLY1305`. Wire format
is unchanged; nodes with and without it interoperate.

Measured on a 1400-byte payload:

| target | donna | asm | whole-AEAD |
| --- | ---: | ---: | ---: |
| AMD Athlon II P360 (no AES-NI) | 3.24 c/B | 1.47 c/B | +23.5% |
| Annapurna AL21400 (Cortex-A15) | 4.29 c/B | 1.72 c/B | +29.1% |

## Files

| file | what |
| --- | --- |
| `poly1305-x86_64.S` | generated, x86-64 ELF, base scalar path only |
| `poly1305-armv4.S` | generated, ARMv4+ ELF, includes the NEON path |
| `arm_arch.h` | OpenSSL header the ARM assembly includes |
| `*.pl`, `*-xlate.pl` | the perlasm generators |
| `cc-shim.sh` | fixes the generators' compiler probes, see below |

Regenerate with:

```sh
CC=./cc-shim.sh perl poly1305-armv4.pl  linux32 poly1305-armv4.S
CC=./cc-shim.sh perl poly1305-x86_64.pl elf     poly1305-x86_64.S
```

`cc-shim.sh` reports clang, so perlasm emits `.section ".note.gnu.property", "a"`
rather than the `#alloc` form clang's assembler rejects, and fails the `-Wa,-v`
probe so the AVX paths are left out. Those paths dispatch on
`OPENSSL_ia32cap_P`, which would require OpenSSL's cpuid setup.

## Interface

```c
int  poly1305_init(void *ctx, const unsigned char key[16], void *func);
void poly1305_blocks(void *ctx, const unsigned char *inp, size_t len, unsigned int padbit);
void poly1305_emit(void *ctx, unsigned char mac[16], const unsigned int nonce[4]);
```

`poly1305_init` writes two function pointers through its third argument on both
targets, so it must not be given `NULL`. A non-zero return means those pointers
supersede the plain symbols; that is how the ARM build selects NEON.

The ARM assembly reads `OPENSSL_armcap_P` to detect NEON. `node/Poly1305.cpp`
defines it as `zt_openssl_armcap_P` and the `.S` is compiled with
`-DOPENSSL_armcap_P=zt_openssl_armcap_P`, because the OpenSSL name collides with
libcrypto's own definition when both are linked.

## Short messages

`Poly1305::compute()` calls donna below `ZT_POLY1305_ASM_MIN_LEN` (256 bytes).
The assembly pays for three non-inlinable calls; donna inlines. Without the
threshold the assembly measured -18% for a 64-byte payload on the Athlon.

## Licence

Apache-2.0, Copyright the OpenSSL Project Authors. Not covered by ZeroTier's
MPL-2.0.
