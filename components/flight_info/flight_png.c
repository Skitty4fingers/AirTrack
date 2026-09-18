#include "flight_png.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*
 * Just enough PNG for airline logo tiles: the logo service returns small
 * 8-bit RGBA images, and anything outside the supported subset is refused
 * rather than guessed at, so the caller falls back to the text-only header.
 */

#define PNG_COLOR_GREY 0U
#define PNG_COLOR_RGB 2U
#define PNG_COLOR_PALETTE 3U
#define PNG_COLOR_GREY_ALPHA 4U
#define PNG_COLOR_RGBA 6U
#define PNG_MAX_IDAT_BYTES (32U * 1024U)

static uint32_t be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    const int p = (int)a + (int)b - (int)c;
    const int pa = abs(p - (int)a);
    const int pb = abs(p - (int)b);
    const int pc = abs(p - (int)c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

/* Undo the per-scanline filters in place; rows keep their filter byte. */
static esp_err_t unfilter(uint8_t *data, uint32_t width, uint32_t height,
                          uint32_t channels)
{
    const size_t stride = (size_t)width * channels;
    const uint8_t *previous = NULL;
    for (uint32_t y = 0U; y < height; ++y) {
        uint8_t *row = data + (size_t)y * (stride + 1U);
        const uint8_t filter = row[0];
        uint8_t *pixels = row + 1;
        for (size_t x = 0U; x < stride; ++x) {
            const uint8_t left = x >= channels ? pixels[x - channels] : 0U;
            const uint8_t up = previous != NULL ? previous[x] : 0U;
            const uint8_t up_left =
                previous != NULL && x >= channels ? previous[x - channels] : 0U;
            switch (filter) {
            case 0U:
                break;
            case 1U:
                pixels[x] = (uint8_t)(pixels[x] + left);
                break;
            case 2U:
                pixels[x] = (uint8_t)(pixels[x] + up);
                break;
            case 3U:
                pixels[x] = (uint8_t)(pixels[x] + (uint8_t)(((unsigned)left + up) / 2U));
                break;
            case 4U:
                pixels[x] = (uint8_t)(pixels[x] + paeth(left, up, up_left));
                break;
            default:
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        previous = pixels;
    }
    return ESP_OK;
}

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t color_type;
    uint32_t channels;
    uint8_t palette[256][3];
    uint8_t palette_alpha[256];
    uint32_t palette_count;
} png_header_t;

static void source_pixel(const png_header_t *header, const uint8_t *data,
                         uint32_t x, uint32_t y, uint8_t rgba[4])
{
    const size_t stride = (size_t)header->width * header->channels;
    const uint8_t *p = data + (size_t)y * (stride + 1U) + 1U +
                       (size_t)x * header->channels;
    switch (header->color_type) {
    case PNG_COLOR_GREY:
        rgba[0] = rgba[1] = rgba[2] = p[0];
        rgba[3] = 255U;
        break;
    case PNG_COLOR_GREY_ALPHA:
        rgba[0] = rgba[1] = rgba[2] = p[0];
        rgba[3] = p[1];
        break;
    case PNG_COLOR_RGB:
        rgba[0] = p[0];
        rgba[1] = p[1];
        rgba[2] = p[2];
        rgba[3] = 255U;
        break;
    case PNG_COLOR_PALETTE: {
        const uint8_t index = p[0];
        if (index < header->palette_count) {
            rgba[0] = header->palette[index][0];
            rgba[1] = header->palette[index][1];
            rgba[2] = header->palette[index][2];
            rgba[3] = header->palette_alpha[index];
        } else {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0U;
        }
        break;
    }
    case PNG_COLOR_RGBA:
    default:
        rgba[0] = p[0];
        rgba[1] = p[1];
        rgba[2] = p[2];
        rgba[3] = p[3];
        break;
    }
}

esp_err_t flight_png_decode_rgb565(const uint8_t *png, size_t length,
                                   uint16_t *out, uint16_t out_width,
                                   uint16_t out_height, uint32_t background_rgb)
{
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (png == NULL || out == NULL || out_width == 0U || out_height == 0U ||
        length < sizeof(signature) || memcmp(png, signature, sizeof(signature)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    png_header_t *header = calloc(1U, sizeof(*header));
    if (header == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(header->palette_alpha, 0xff, sizeof(header->palette_alpha));

    /* First pass: header, palette, and the total size of the IDAT stream. */
    bool have_header = false;
    bool have_end = false;
    size_t idat_total = 0U;
    esp_err_t result = ESP_OK;
    size_t offset = sizeof(signature);
    while (result == ESP_OK && !have_end) {
        if (length - offset < 12U) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        const uint32_t chunk_length = be32(png + offset);
        const uint8_t *type = png + offset + 4U;
        const uint8_t *body = png + offset + 8U;
        if (chunk_length > length - offset - 12U) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (memcmp(type, "IHDR", 4U) == 0) {
            if (have_header || chunk_length != 13U) {
                result = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            header->width = be32(body);
            header->height = be32(body + 4U);
            const uint8_t depth = body[8];
            header->color_type = body[9];
            const bool supported = depth == 8U && body[10] == 0U &&
                                   body[11] == 0U && body[12] == 0U;
            header->channels = header->color_type == PNG_COLOR_GREY ? 1U
                : header->color_type == PNG_COLOR_GREY_ALPHA ? 2U
                : header->color_type == PNG_COLOR_RGB ? 3U
                : header->color_type == PNG_COLOR_PALETTE ? 1U
                : header->color_type == PNG_COLOR_RGBA ? 4U : 0U;
            if (!supported || header->channels == 0U || header->width == 0U ||
                header->height == 0U || header->width > FLIGHT_PNG_MAX_DIMENSION ||
                header->height > FLIGHT_PNG_MAX_DIMENSION) {
                result = ESP_ERR_NOT_SUPPORTED;
                break;
            }
            have_header = true;
        } else if (!have_header) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        } else if (memcmp(type, "PLTE", 4U) == 0) {
            if (chunk_length % 3U != 0U || chunk_length > 768U) {
                result = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            header->palette_count = chunk_length / 3U;
            memcpy(header->palette, body, chunk_length);
        } else if (memcmp(type, "tRNS", 4U) == 0) {
            if (header->color_type == PNG_COLOR_PALETTE && chunk_length <= 256U) {
                memcpy(header->palette_alpha, body, chunk_length);
            }
        } else if (memcmp(type, "IDAT", 4U) == 0) {
            idat_total += chunk_length;
            if (idat_total > PNG_MAX_IDAT_BYTES) {
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
        } else if (memcmp(type, "IEND", 4U) == 0) {
            have_end = true;
        } else if ((type[0] & 0x20U) == 0U) {
            /* An unknown critical chunk changes how the image is read. */
            result = ESP_ERR_NOT_SUPPORTED;
            break;
        }
        offset += 12U + chunk_length;
    }
    if (result == ESP_OK &&
        (!have_header || idat_total == 0U ||
         (header->color_type == PNG_COLOR_PALETTE && header->palette_count == 0U))) {
        result = ESP_ERR_INVALID_RESPONSE;
    }

    const size_t expected = (size_t)header->height *
                            ((size_t)header->width * header->channels + 1U);
    uint8_t *idat = NULL;
    uint8_t *data = NULL;
    if (result == ESP_OK) {
        idat = malloc(idat_total);
        data = malloc(expected);
        if (idat == NULL || data == NULL) {
            result = ESP_ERR_NO_MEM;
        }
    }
    if (result == ESP_OK) {
        /* Second pass: gather the IDAT payload into one zlib stream. */
        size_t used = 0U;
        offset = sizeof(signature);
        while (offset + 12U <= length) {
            const uint32_t chunk_length = be32(png + offset);
            if (memcmp(png + offset + 4U, "IDAT", 4U) == 0) {
                memcpy(idat + used, png + offset + 8U, chunk_length);
                used += chunk_length;
            } else if (memcmp(png + offset + 4U, "IEND", 4U) == 0) {
                break;
            }
            offset += 12U + chunk_length;
        }
        result = flight_png_inflate(idat, used, data, expected);
    }
    free(idat);
    if (result == ESP_OK) {
        result = unfilter(data, header->width, header->height, header->channels);
    }
    if (result == ESP_OK) {
        const uint32_t bg_r = (background_rgb >> 16) & 0xffU;
        const uint32_t bg_g = (background_rgb >> 8) & 0xffU;
        const uint32_t bg_b = background_rgb & 0xffU;
        for (uint32_t oy = 0U; oy < out_height; ++oy) {
            /* Source rows and columns covered by this output pixel. */
            const uint32_t y0 = oy * header->height / out_height;
            uint32_t y1 = (oy + 1U) * header->height / out_height;
            y1 = y1 > y0 ? y1 : y0 + 1U;
            for (uint32_t ox = 0U; ox < out_width; ++ox) {
                const uint32_t x0 = ox * header->width / out_width;
                uint32_t x1 = (ox + 1U) * header->width / out_width;
                x1 = x1 > x0 ? x1 : x0 + 1U;
                uint32_t sum[3] = {0U, 0U, 0U};
                uint32_t samples = 0U;
                for (uint32_t y = y0; y < y1; ++y) {
                    for (uint32_t x = x0; x < x1; ++x) {
                        uint8_t rgba[4];
                        source_pixel(header, data, x, y, rgba);
                        const uint32_t a = rgba[3];
                        sum[0] += (rgba[0] * a + bg_r * (255U - a)) / 255U;
                        sum[1] += (rgba[1] * a + bg_g * (255U - a)) / 255U;
                        sum[2] += (rgba[2] * a + bg_b * (255U - a)) / 255U;
                        ++samples;
                    }
                }
                const uint32_t r = sum[0] / samples;
                const uint32_t g = sum[1] / samples;
                const uint32_t b = sum[2] / samples;
                out[(size_t)oy * out_width + ox] =
                    (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }
    free(data);
    free(header);
    return result;
}
