#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
: "${CC:?Set CC to the SigmaStar target compiler}"
: "${FPV_SYSROOT:?Set the matching SigmaStar sysroot}"
: "${PKG_CONFIG_LIBDIR:?Set only target pkgconfig directories}"
export PKG_CONFIG_SYSROOT_DIR="$FPV_SYSROOT" PKG_CONFIG_PATH=
pc=${PKG_CONFIG:-pkg-config}
"$pc" --exists libevent_core
build=${1:-"$root/build/msposd-air"}
mkdir -p -- "$build/source" "$build/bin"
build=$(cd -- "$build" && pwd)
# The upstream Makefile expands -I$(PWD), -L$(DRV), and OUTPUT unquoted.
# Compile in a temporary path without spaces, then copy the artifact out.
scratch=$(mktemp -d /tmp/fpv-msposd.XXXXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
cp -a -- "$root/third_party/msposd/." "$scratch/"
cp -a -- "$root/third_party/sdk/sigmastar/lib" "$scratch/driver-libs"
# Set the pinned version explicitly: a source snapshot has no .git directory.
printf '#ifndef VERSION_H\n#define VERSION_H\n#define GIT_VERSION "ad0175ce01c4e09b3eb4223fd683735f9df22715"\n#endif\n' > "$scratch/version.h"
# Upstream star6e intentionally uses its bundled sdk/infinity6 headers, not MI capture headers.
[[ "$FPV_SYSROOT" != *[[:space:]]* && "$CC" != *[[:space:]]* ]] || { echo 'Upstream Makefile requires a sysroot/compiler path without whitespace; use symlinks' >&2; exit 2; }
CFLAGS="${CFLAGS:-} --sysroot=$FPV_SYSROOT $("$pc" --cflags libevent_core) $("$pc" --libs-only-L libevent_core)" \
make -C "$scratch" star6e "CC=$CC" "TOOLCHAIN=$FPV_SYSROOT" "PWD=$scratch" \
  "DRV=$scratch/driver-libs" "OUTPUT=$scratch/msposd-built" VERSION_STRING=20260927_202429 \
  VERSION=ad0175ce01c4e09b3eb4223fd683735f9df22715 \
  SDK=./sdk/infinity6
install -m755 -- "$scratch/msposd-built" "$build/bin/msposd"
