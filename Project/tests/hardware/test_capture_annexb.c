#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "capture_annexb.h"

static int calls, markers, fail_at;
static int receive(const uint8_t *p, size_t n, uint32_t ts, int end, void *opaque)
{
    (void)opaque;
    assert(n == 3 && p[1] == 1 && ts == UINT32_MAX);
    ++calls;
    markers += end;
    return calls == fail_at ? -1 : 0;
}
int main(void)
{
    const uint8_t au[] = {0, 0, 0, 0, 1, 0x40, 1, 8, 0, 0, 1, 0x26, 1, 9, 0, 0};
    const uint8_t missing[] = {0x26, 1, 2};
    const uint8_t empty[] = {0, 0, 1, 0, 0, 1, 0x26, 1, 2};
    const uint8_t truncated[] = {0, 0, 1, 0x26};
    const uint8_t invalid[] = {0, 0, 1, 0x26, 0, 2};
    const uint8_t terminal[] = {0, 0, 1, 0x26, 1, 2, 0, 0, 1};
    assert(fpv_hw_emit_au(au, sizeof au, UINT32_MAX, receive, NULL) == 0);
    assert(calls == 2 && markers == 1);
    calls = markers = 0;
    assert(fpv_hw_emit_au(missing, sizeof missing, 0, receive, NULL) < 0);
    assert(fpv_hw_emit_au(empty, sizeof empty, 0, receive, NULL) < 0);
    assert(fpv_hw_emit_au(truncated, sizeof truncated, 0, receive, NULL) < 0);
    assert(fpv_hw_emit_au(invalid, sizeof invalid, 0, receive, NULL) < 0);
    assert(fpv_hw_emit_au(terminal, sizeof terminal, 0, receive, NULL) < 0);
    assert(calls == 0);
    fail_at = 1;
    assert(fpv_hw_emit_au(au, sizeof au, UINT32_MAX, receive, NULL) < 0);
    assert(calls == 1 && markers == 0);
    puts("hardware Annex B boundary tests passed");
    return 0;
}
