#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest source image accepted; the inflated scanlines of a 64x64 RGBA
 * image are about 16 KiB, which bounds the decoder's heap use. */
#define FLIGHT_PNG_MAX_DIMENSION 64U

/**
 * Decode a small 8-bit, non-interlaced PNG (grey, grey+alpha, RGB, RGBA or
 * palette) into out_width x out_height RGB565 pixels, box-sampled when the
 * sizes differ and alpha-blended onto background_rgb (0xRRGGBB).
 */
esp_err_t flight_png_decode_rgb565(const uint8_t *png, size_t length,
                                   uint16_t *out, uint16_t out_width,
                                   uint16_t out_height, uint32_t background_rgb);

/**
 * zlib-wrapped inflate into a buffer of exactly expected bytes.  Supplied by
 * flight_png_inflate.c on the device (ROM miniz) and by the host tests.
 */
esp_err_t flight_png_inflate(const uint8_t *in, size_t in_length, uint8_t *out,
                             size_t expected);

#ifdef __cplusplus
}
#endif
