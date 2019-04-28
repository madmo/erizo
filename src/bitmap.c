#include "bitmap.h"
#include "mat.h"

void bitmap_write_header(FILE* f, unsigned width, unsigned height) {
    const uint32_t size = width * 3 * height;

#define BYTES_U32(i) (i), ((i) >> 8), ((i) >> 16), ((i) >> 24)
#define BYTES_U16(i) (i), ((i) >> 8)
    const uint8_t header[] = {
        // Header
        'B', 'M',
        BYTES_U32(size + 54),   // bfSize
        BYTES_U16(0),           // bfReserved
        BYTES_U16(0),           // bfReserved
        BYTES_U32(54),          // bfOffbits
        // Info
        BYTES_U32(40),          // biSize
        BYTES_U32(width),       // biWidth
        BYTES_U32(height),      // biHeight
        BYTES_U16(1),           // biPlanes
        BYTES_U16(24),          // biBitCount
        BYTES_U32(0),           // biCompression
        BYTES_U32(size),        // biSizeImage
        BYTES_U32(0),           // biXPelsPerMeter
        BYTES_U32(0),           // biYPelsPerMeter
        BYTES_U32(0),           // biClrUsed
        BYTES_U32(0),           // biClrImportant
    };
    assert(sizeof(header) == 54);

    for (unsigned i=0; i < sizeof(header); ++i) {
        fprintf(f, "%c", header[i]);
    }
}

void bitmap_write_depth(FILE* f, unsigned w, unsigned h, const uint32_t* data) {
    for (unsigned y=0; y < h; ++y) {
        for (unsigned x=0; x < w; ++x) {
            for (unsigned i=0; i < 3; ++i) {
                fprintf(f, "%c", (*data) >> 24);
            }
            ++data;
        }
    }
}

void bitmap_write_rgb(FILE* f, unsigned w, unsigned h,
                      const float (*data)[4])
{
    for (unsigned y=0; y < h; ++y) {
        for (unsigned x=0; x < w; ++x) {
            for (unsigned i=0; i < 3; ++i) {
                const float d = (*data)[i];
                fprintf(f, "%c", (uint8_t)(d * 255));
            }
            ++data;
        }
    }
}

void bitmap_write_rays(FILE* f, unsigned w, unsigned h,
                       const float (*data)[4], uint32_t rays)
{
    for (unsigned y=0; y < h; ++y) {
        for (unsigned x=0; x < w; ++x) {
            const float len = vec3_length((const float*)data);
            const float scale = (*data)[3] / rays;
            for (unsigned i=0; i < 3; ++i) {
                const float d = (*data)[i] / len * scale / 2.0f + 0.5f;
                fprintf(f, "%c", (uint8_t)(d * 255));
            }
            ++data;
        }
    }
}