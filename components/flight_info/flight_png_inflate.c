#include "flight_png.h"

#include <stdlib.h>

#include "miniz.h"

/*
 * Inflate with the miniz tinfl that the ESP32-C6 ROM already carries.  Its
 * decompressor state is about 11 KiB, so it is taken from the heap for the
 * duration of one logo rather than from the calling task's stack.
 */
esp_err_t flight_png_inflate(const uint8_t *in, size_t in_length, uint8_t *out,
                             size_t expected)
{
    tinfl_decompressor *inflater = malloc(sizeof(*inflater));
    if (inflater == NULL) {
        return ESP_ERR_NO_MEM;
    }
    tinfl_init(inflater);
    size_t in_used = in_length;
    size_t out_used = expected;
    const tinfl_status status = tinfl_decompress(
        inflater, in, &in_used, out, out, &out_used,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(inflater);
    return status == TINFL_STATUS_DONE && out_used == expected
               ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
