/* Isolated inter-helper SIMD128 vs C-reference test.
 * Compile with emcc (-O2 -msimd128) together with wasm/vvc/inter.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/vvc/dsp.c"

#include "libavcodec/wasm/vvc/inter.h"

#define MAX_PB 128

static uint32_t rng_state = 987654321;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static int failures;

static int test_put_pixels(int w, int h)
{
    const int stride = w + 8;
    uint8_t *src = malloc((size_t)stride * (h + 2));
    int16_t a[MAX_PB * 32], b[MAX_PB * 32];
    int i;

    for (i = 0; i < stride * (h + 2); i++)
        src[i] = (uint8_t)rnd();
    for (i = 0; i < MAX_PB * 32; i++)
        a[i] = b[i] = (int16_t)0xabcdef;

    put_pixels_8(a, src + stride, stride, h, NULL, NULL, w);
    ff_vvc_put_pixels_8_simd128(b, src + stride, stride, h, NULL, NULL, w);
    for (i = 0; i < MAX_PB * 32; i++) {
        if (a[i] != b[i]) {
            printf("MISMATCH put_pixels w=%d h=%d idx=%d ref=%d sim=%d\n", w, h, i, a[i], b[i]);
            failures++;
            free(src);
            return 1;
        }
    }
    free(src);
    return 0;
}

static int test_avg(int w, int h, int draw)
{
    int16_t *s0 = malloc((size_t)MAX_PB * h * 2);
    int16_t *s1 = malloc((size_t)MAX_PB * h * 2);
    uint8_t a[MAX_PB * 32], b[MAX_PB * 32];
    int i;

    for (i = 0; i < MAX_PB * h; i++) {
        /* full-range int16 values stress the intermediate range */
        s0[i] = (int16_t)(rnd() & 0xffff);
        s1[i] = (int16_t)(rnd() & 0xffff);
    }
    memset(a, 0x5a, sizeof(a));
    memset(b, 0x5a, sizeof(b));

    avg_8(a, w + 4, s0, s1, w, h);
    ff_vvc_avg_8_simd128(b, w + 4, s0, s1, w, h);
    for (i = 0; i < MAX_PB * 32; i++) {
        if (a[i] != b[i]) {
            printf("MISMATCH avg w=%d h=%d draw=%d idx=%d ref=%u sim=%u\n", w, h, draw, i, a[i], b[i]);
            failures++;
            free(s0); free(s1);
            return 1;
        }
    }
    free(s0); free(s1);
    return 0;
}

static int test_sad(int w, int h, int dx, int dy)
{
    /* SAD starts at rows dy and 4 - dy, then samples every other row.
     * Reserve and initialize the complete h + 4 row search region. */
    const size_t count = (size_t)(h + 4) * MAX_PB;
    int16_t *s0 = malloc(count * sizeof(*s0));
    int16_t *s1 = malloc(count * sizeof(*s1));
    int ref, sim;

    for (size_t i = 0; i < count; i++) {
        s0[i] = (int16_t)(rnd() & 0xffff);
        s1[i] = (int16_t)(rnd() & 0xffff);
    }
    ref = vvc_sad(s0, s1, dx, dy, w, h);
    sim = ff_vvc_sad_8_simd128(s0, s1, dx, dy, w, h);
    free(s0); free(s1);
    if (ref != sim) {
        printf("MISMATCH sad w=%d h=%d dx=%d dy=%d ref=%d sim=%d\n", w, h, dx, dy, ref, sim);
        failures++;
        return 1;
    }
    return 0;
}


/* Only the last sampled row differs. Check an independently known sum so
 * matching C/SIMD overreads or missing initialization cannot pass by equality. */
static int test_sad_boundary(int w, int h, int dx, int dy)
{
    const size_t count = (size_t)(h + 4) * MAX_PB;
    int16_t *s0 = calloc(count, sizeof(*s0));
    int16_t *s1 = calloc(count, sizeof(*s1));
    const int row0 = dy + h - 2, row1 = 4 - dy + h - 2;
    const int expected = w * 65535;
    int ref, sim;

    for (int x = 0; x < w; x++) {
        s0[row0 * MAX_PB + dx + x] = INT16_MIN;
        s1[row1 * MAX_PB + 4 - dx + x] = INT16_MAX;
    }
    ref = vvc_sad(s0, s1, dx, dy, w, h);
    sim = ff_vvc_sad_8_simd128(s0, s1, dx, dy, w, h);
    free(s0); free(s1);
    if (ref != expected || sim != expected) {
        printf("MISMATCH sad boundary w=%d h=%d dx=%d dy=%d expected=%d ref=%d sim=%d\n",
               w, h, dx, dy, expected, ref, sim);
        failures++;
        return 1;
    }
    return 0;
}

static const int8_t luma_filters[][8] = {
    { 0, 0, 0, 0, 1, -3, 63, 4 },
    { -10, 4, -1, -1, 4, -11, 45, 34 },
    { -5, 2, -1, 0, 1, -2, 4, 63 },
    { 63, 4, -3, 1, 0, 0, 0, 0 },
};
static const int8_t chroma_filters[][4] = {
    { 0, 0, -1, 63 },
    { 14, -2, -4, 55 },
    { 30, -4, -4, 39 },
    { 63, -1, 0, 0 },
};

static int test_qpel(int chroma, int w, int h, int which, const int8_t *hf,
                     const int8_t *vf, int draw)
{
    const int pad = 16, stride = w + 2 * pad;
    uint8_t *src = malloc((size_t)stride * (h + 2 * pad));
    int16_t a[MAX_PB * 32], b[MAX_PB * 32];
    int i;

    for (i = 0; i < stride * (h + 2 * pad); i++)
        src[i] = (uint8_t)rnd();
    for (i = 0; i < MAX_PB * 32; i++)
        a[i] = b[i] = (int16_t)0x1357;

    if (!chroma) {
        if (which == 0)      put_luma_h_8(a, src + pad * stride + pad, stride, h, hf, vf, w);
        else if (which == 1) put_luma_v_8(a, src + pad * stride + pad, stride, h, hf, vf, w);
        else                 put_luma_hv_8(a, src + pad * stride + pad, stride, h, hf, vf, w);
        if (which == 0)      ff_vvc_put_luma_h_8_simd128(b, src + pad * stride + pad, stride, h, hf, vf, w);
        else if (which == 1) ff_vvc_put_luma_v_8_simd128(b, src + pad * stride + pad, stride, h, hf, vf, w);
        else                 ff_vvc_put_luma_hv_8_simd128(b, src + pad * stride + pad, stride, h, hf, vf, w);
    } else {
        if (which == 0)      put_chroma_h_8(a, src + pad * stride + pad, stride, h, hf, vf, w);
        else if (which == 1) put_chroma_v_8(a, src + pad * stride + pad, stride, h, hf, vf, w);
        else                 put_chroma_hv_8(a, src + pad * stride + pad, stride, h, hf, vf, w);
        if (which == 0)      ff_vvc_put_chroma_h_8_simd128(b, src + pad * stride + pad, stride, h, hf, vf, w);
        else if (which == 1) ff_vvc_put_chroma_v_8_simd128(b, src + pad * stride + pad, stride, h, hf, vf, w);
        else                 ff_vvc_put_chroma_hv_8_simd128(b, src + pad * stride + pad, stride, h, hf, vf, w);
    }

    for (i = 0; i < MAX_PB * 32; i++) {
        if (a[i] != b[i]) {
            printf("MISMATCH qpel %s w=%d h=%d which=%d draw=%d idx=%d ref=%d sim=%d\n",
                   chroma ? "chroma" : "luma", w, h, which, draw, i, a[i], b[i]);
            failures++;
            free(src);
            return 1;
        }
    }
    free(src);
    return 0;
}

int main(void)
{
    static const int widths[]  = { 2, 4, 8, 16, 32, 64, 128 };
    static const int heights[] = { 1, 2, 4, 8, 16 };
    int nb = 0, nf = 0;

    for (size_t wi = 0; wi < sizeof(widths) / sizeof(*widths); wi++) {
        for (size_t hi = 0; hi < sizeof(heights) / sizeof(*heights); hi++) {
            nb++; nf += test_put_pixels(widths[wi], heights[hi]);
            for (int draw = 0; draw < 3; draw++) {
                nb++; nf += test_avg(widths[wi], heights[hi], draw);
            }
        }
    }
    for (size_t fi = 0; fi < sizeof(luma_filters) / sizeof(*luma_filters); fi++) {
        for (int which = 0; which < 3; which++) {
            for (size_t wi = 0; wi < sizeof(widths) / sizeof(*widths); wi++) {
                for (size_t hi = 0; hi < sizeof(heights) / sizeof(*heights); hi++) {
                    nb++; nf += test_qpel(0, widths[wi], heights[hi], which,
                                          luma_filters[fi], luma_filters[(fi + 1) % 4], fi);
                }
            }
        }
    }
    for (size_t fi = 0; fi < sizeof(chroma_filters) / sizeof(*chroma_filters); fi++) {
        for (int which = 0; which < 3; which++) {
            for (size_t wi = 0; wi < sizeof(widths) / sizeof(*widths); wi++) {
                for (size_t hi = 0; hi < sizeof(heights) / sizeof(*heights); hi++) {
                    nb++; nf += test_qpel(1, widths[wi], heights[hi], which,
                                          chroma_filters[fi], chroma_filters[(fi + 1) % 4], fi);
                }
            }
        }
    }
    for (int w = 4; w <= 32; w += 4) {
        for (int h = 4; h <= 16; h += 4) {
            for (int dx = 0; dx <= 4; dx++) {
                for (int dy = 0; dy <= 4; dy++) {
                    nb++; nf += test_sad(w, h, dx, dy);
                    nb++; nf += test_sad_boundary(w, h, dx, dy);
                }
            }
        }
    }
    printf("%s: %d cases, %d failures\n", failures ? "FAIL" : "PASS", nb, nf);
    return failures ? 1 : 0;
}
