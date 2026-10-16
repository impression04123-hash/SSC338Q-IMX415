#ifndef FPV_PLATFORM_H
#define FPV_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#include "fpv/config.h"

/* Capture emits a complete H.265 NALU without Annex B prefix. Timestamp uses 90 kHz. */
typedef int (*fpv_capture_callback)(const uint8_t *nal, size_t size, uint32_t timestamp,
                                    int end_of_access_unit, void *opaque);
int fpv_capture_open(const fpv_config *cfg, fpv_capture_callback callback, void *opaque,
                     char *error, size_t error_size);
int fpv_capture_poll(int timeout_ms);
void fpv_capture_close(void);

/* Display consumes complete Annex B NALUs. Hardware backend is singleton per process. */
int fpv_display_open(const fpv_config *cfg, char *error, size_t error_size);
int fpv_display_write(const uint8_t *annexb, size_t size, uint32_t timestamp,
                      int end_of_access_unit);
void fpv_display_close(void);
#endif
