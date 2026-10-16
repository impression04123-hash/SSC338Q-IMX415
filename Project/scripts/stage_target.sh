#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
[[ $# -ge 2 && $# -le 3 ]] || { echo "Usage: $0 cmake-build staging-directory [wfb-build]" >&2; exit 2; }
build=$(cd -- "$1" && pwd)
[[ ! -e "$2" ]] || { echo 'Staging destination must not exist; choose a fresh directory' >&2; exit 2; }
mkdir -p -- "$2"
stage=$(cd -- "$2" && pwd)
DESTDIR="$stage" cmake --install "$build" --prefix /usr
if [[ $# == 3 ]]; then
  for binary in wfb_tx wfb_rx wfb_keygen wfb_tx_cmd wfb_tun; do
    install -Dm755 -- "$3/bin/$binary" "$stage/usr/bin/$binary"
  done
fi
mkdir -p -- "$stage/usr/share/fpv/licenses"
cp -a -- "$root/THIRD_PARTY.md" "$root/third_party/SOURCES.json" "$stage/usr/share/fpv/licenses/"
licenses="$stage/usr/share/fpv/licenses"
install -Dm644 -- "$root/third_party/sdk/RESEARCH-LICENSE" "$licenses/OpenIPC-research/LICENSE"
if [[ -x "$stage/usr/bin/wfb_tx" || -x "$stage/usr/bin/wfb_rx" ]]; then
  install -Dm644 -- "$root/third_party/wfb-ng/LICENSE.txt" "$licenses/wfb-ng/LICENSE.txt"
fi
# Include OSD notices as well: BUILD.md permits adding the separately built
# msposd executable to this fresh stage after the main install.
if [[ -d "$root/third_party/msposd" ]]; then
  while IFS= read -r -d '' notice; do
    relative=${notice#"$root/third_party/msposd/"}
    install -Dm644 -- "$notice" "$licenses/msposd/$relative"
  done < <(find "$root/third_party/msposd" -type f \( -iname '*license*' -o -iname '*copying*' -o -iname '*notice*' \) -print0)
fi
printf 'Staged files only: %s\nReview configuration and keys before transferring to the target.\n' "$stage"
