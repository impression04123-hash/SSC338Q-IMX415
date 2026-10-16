#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${1:-"$root/build/wfb-host"}
mkdir -p -- "$build"
build=$(cd -- "$build" && pwd)
[[ "$build" != "$root/third_party/wfb-ng" ]] || { echo 'Use a separate build directory' >&2; exit 2; }
# Upstream uses in-tree objects. Build a private source copy, preserving the snapshot.
mkdir -p -- "$build/source" "$build/bin"
cp -a -- "$root/third_party/wfb-ng/." "$build/source/"
cc=${CC:-gcc}; cxx=${CXX:-g++}
pc=${PKG_CONFIG:-pkg-config}
if [[ ${CROSS_COMPILE:-} || ${FPV_SYSROOT:-} ]]; then
  : "${FPV_SYSROOT:?Cross builds require the target sysroot}"
  : "${PKG_CONFIG_LIBDIR:?Set ONLY target pkgconfig directories}"
  export PKG_CONFIG_SYSROOT_DIR="$FPV_SYSROOT"
  export PKG_CONFIG_PATH=
  cc=${CC:-${CROSS_COMPILE:?Set CROSS_COMPILE or explicit CC/CXX}gcc}
  cxx=${CXX:-${CROSS_COMPILE:?Set CROSS_COMPILE or explicit CC/CXX}g++}
fi
"$pc" --exists libsodium libpcap libevent_core
cppflags=$("$pc" --cflags libsodium libpcap libevent_core)
ldflags=$("$pc" --libs libsodium libpcap libevent_core)
make -C "$build/source" clean
make -C "$build/source" -j "${JOBS:-2}" all_bin "CC=$cc" "CXX=$cxx" \
  "CFLAGS=${CFLAGS:-} $cppflags ${FPV_SYSROOT:+--sysroot=$FPV_SYSROOT}" \
  "LDFLAGS=${LDFLAGS:-} $ldflags ${FPV_SYSROOT:+--sysroot=$FPV_SYSROOT}" \
  COMMIT=ae2122c1fb70cd215bbc2e1b517b7b91f9401220 VERSION=26.2.3.34667 SOURCE_DATE_EPOCH=1770111467
for binary in wfb_tx wfb_rx wfb_keygen wfb_tx_cmd wfb_tun; do
  install -m 755 -- "$build/source/$binary" "$build/bin/$binary"
done
printf 'WFB binaries: %s/bin\n' "$build"
