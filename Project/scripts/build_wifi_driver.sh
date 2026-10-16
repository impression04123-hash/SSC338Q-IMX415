#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
: "${KSRC:?Set KSRC to the configured target kernel build tree}"
: "${ARCH:?Set target ARCH, normally arm}"
: "${CROSS_COMPILE:?Set target compiler prefix ending in -}"
[[ -f "$KSRC/Makefile" && -f "$KSRC/include/generated/autoconf.h" ]] || { echo 'KSRC must be a prepared target kernel tree' >&2; exit 2; }
build=${1:-"$root/build/wifi-$ARCH"}
mkdir -p -- "$build/modules"
build=$(cd -- "$build" && pwd)
[[ "$KSRC" != *[[:space:]]* && "$CROSS_COMPILE" != *[[:space:]]* ]] || { echo 'Kernel Makefiles need KSRC/compiler paths without whitespace; use symlinks' >&2; exit 2; }
scratch=$(mktemp -d /tmp/fpv-wifi.XXXXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
cp -a -- "$root/third_party/rtl88x2eu-20230815/." "$scratch/"
EXTRA_CFLAGS="${EXTRA_CFLAGS:-} -DCONFIG_LITTLE_ENDIAN -DCONFIG_IOCTL_CFG80211 -DRTW_USE_CFG80211_STA_EVENT" \
make -C "$scratch" -j "${JOBS:-2}" "KSRC=$KSRC" "ARCH=$ARCH" \
  "CROSS_COMPILE=$CROSS_COMPILE" CONFIG_PLATFORM_I386_PC=n modules
find "$scratch" -maxdepth 1 -name '*.ko' -exec cp -- '{}' "$build/modules/" \;
printf 'Target modules: %s/modules (not installed)\n' "$build"
