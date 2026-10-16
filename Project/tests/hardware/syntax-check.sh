#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
cc=${CC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror -Iinclude -Isrc/platform tests/hardware/test_capture_annexb.c -o /tmp/fpv-test-capture-annexb
/tmp/fpv-test-capture-annexb
"$cc" -std=c11 -Wall -Wextra -Werror -fsyntax-only -Iinclude -isystem third_party/sdk/sigmastar/include src/platform/sigmastar_capture.c
"$cc" -std=gnu11 -Wall -Wextra -Werror -fsyntax-only -Iinclude -isystem third_party/sdk/hisilicon/include src/platform/hisilicon_display.c
