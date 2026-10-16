#ifndef FPV_CAPTURE_ANNEXB_H
#define FPV_CAPTURE_ANNEXB_H
#include "fpv/platform.h"

/* Return the payload offset following an Annex B prefix, and its first zero. */
static size_t fpv_hw_prefix(const uint8_t *p, size_t n, size_t from, size_t *zero)
{
    size_t zeros = 0;
    for (size_t i = from; i < n; ++i) {
        if (p[i] == 0)
            ++zeros;
        else {
            if (p[i] == 1 && zeros >= 2) {
                *zero = i - zeros;
                return i + 1;
            }
            zeros = 0;
        }
    }
    *zero = n;
    return SIZE_MAX;
}

/* VENC bByFrame yields a complete access unit. Validate before any callback. */
static int fpv_hw_emit_au(const uint8_t *p, size_t n, uint32_t timestamp,
                          fpv_capture_callback callback, void *opaque)
{
    if (!p || !callback || !n)
        return -1;
    size_t first_zero, first = fpv_hw_prefix(p, n, 0, &first_zero);
    if (first >= n || first_zero != 0)
        return -1;
    for (int pass = 0; pass < 2; ++pass) {
        size_t start = first;
        while (start < n) {
            size_t end, next = fpv_hw_prefix(p, n, start, &end);
            if (next == n)
                return -1; /* A terminal start code has no NAL. */
            if (next == SIZE_MAX) {
                end = n;
                while (end > start && p[end - 1] == 0)
                    --end;
            }
            if (end < start || end - start < 2 || end - start > 4U * 1024U * 1024U ||
                (p[start] & 0x80) || !(p[start + 1] & 7))
                return -1;
            if (pass && callback(p + start, end - start, timestamp, next == SIZE_MAX, opaque) != 0)
                return -1;
            start = next == SIZE_MAX ? n : next;
        }
    }
    return 0;
}
#endif
