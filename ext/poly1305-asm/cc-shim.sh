#!/bin/sh
# Generator shim, see README.md. Reports clang so perlasm emits portable
# section syntax, and stays silent for the -Wa,-v probe so the AVX paths (which
# would need OpenSSL cpuid support we do not carry) are left out.
case "$1" in
  --version) echo "clang version 20.0.0" ;;
  *) exit 1 ;;
esac
