/* Isolated ALF SIMD128 vs C-reference test.
 * Compile with emcc (-O2 -msimd128) together with wasm/vvc/alf.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/vvc/dsp.c"

#include "libavcodec/wasm/vvc/alf.h"

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

#ifndef TEST_DEPTH
#define TEST_DEPTH 8
#endif
#define TEST_CAT(name, depth) name ## _ ## depth
#define TEST_NAME(name, depth) TEST_CAT(name, depth)
#define REF(name) TEST_NAME(name, TEST_DEPTH)
#define SIMD_SUFFIX(name) name ## _simd128
#define SIMD_EXPAND(name) SIMD_SUFFIX(name)
#define SIMD(name) SIMD_EXPAND(REF(ff_vvc_ ## name))
#if TEST_DEPTH == 8
typedef uint8_t test_pixel;
#else
typedef uint16_t test_pixel;
#endif

static int failures;

static int run_case(int chroma, int w, int h, int vb_pos, int draw)
{
    const int pad = 10;
    const int stride = w + 2 * pad;
    const size_t total = (size_t)stride * (h + 2 * pad);
    test_pixel *src = malloc(total * sizeof(*src));
    test_pixel *ref = malloc(total * sizeof(*ref));
    test_pixel *sim = malloc(total * sizeof(*sim));
    test_pixel *sp, *rp, *mp;
    static int16_t filter[ALF_MAX_BLOCKS_IN_CTU * 12], clip[ALF_MAX_BLOCKS_IN_CTU * 12];
    const int ntap = chroma ? 6 : 12;
    const int nsets = (w / ALF_BLOCK_SIZE) * (h / ALF_BLOCK_SIZE);
    int i;

    for (i = 0; i < nsets * ntap; i++) {
        filter[i] = (int16_t)((int)(rnd() % 257) - 128);
        clip[i]   = (int16_t)(1 << (TEST_DEPTH - (int[]){0, 3, 5, 7}[rnd() & 3]));
    }

    /* random source including the padding (unwritten bytes must not be used) */
    for (size_t k = 0; k < total; k++)
        src[k] = rnd() & ((1 << TEST_DEPTH) - 1);
    memset(ref, 0xa5, total * sizeof(*ref));
    memset(sim, 0xa5, total * sizeof(*sim));

    sp = src + pad * stride + pad;
    rp = ref + pad * stride + pad;
    mp = sim + pad * stride + pad;

    if (!chroma)
        REF(alf_filter_luma)((uint8_t *)rp, stride * sizeof(*rp), (const uint8_t *)sp, stride * sizeof(*sp), w, h, filter, clip, vb_pos);
    else
        REF(alf_filter_chroma)((uint8_t *)rp, stride * sizeof(*rp), (const uint8_t *)sp, stride * sizeof(*sp), w, h, filter, clip, vb_pos);
    if (!chroma)
        SIMD(alf_filter_luma)((uint8_t *)mp, stride * sizeof(*rp), (const uint8_t *)sp, stride * sizeof(*sp), w, h, filter, clip, vb_pos);
    else
        SIMD(alf_filter_chroma)((uint8_t *)mp, stride * sizeof(*rp), (const uint8_t *)sp, stride * sizeof(*sp), w, h, filter, clip, vb_pos);

    /* compare the whole buffer: filtered area and untouched padding */
    for (size_t k = 0; k < total; k++) {
        if (ref[k] != sim[k]) {
            const int y = (int)(k / stride) - pad;
            const int x = (int)(k % stride) - pad;
            printf("MISMATCH %s w=%d h=%d vb=%d draw=%d at (x=%d,y=%d): ref=%u sim=%u\n",
                   chroma ? "chroma" : "luma", w, h, vb_pos, draw, x, y, ref[k], sim[k]);
            failures++;
            free(src); free(ref); free(sim);
            return 1;
        }
    }
    free(src); free(ref); free(sim);
    return 0;
}


static int test_classify(int w, int h, int vb_pos, int draw)
{
    const int pad = 8, stride = w + 2 * pad;
    const size_t total = (size_t)stride * (h + 2 * pad);
    test_pixel *src = malloc(total * sizeof(*src));
    const int nb = (w / 4) * (h / 4);
    const int gtotal = ((h + 4) / 2) * ((w + 4) / 2) * 4;
    static int g0[64 * 64 * 4], g1[64 * 64 * 4];
    int c0[4096], c1[4096], t0[4096], t1[4096];
    const test_pixel *sp;
    int i, rc = 0;

    for (i = 0; i < (int)total; i++)
        src[i] = rnd() & ((1 << TEST_DEPTH) - 1);
    sp = src + pad * stride + pad;
    memset(g0, 0x3c, sizeof(g0));
    memset(g1, 0x5a, sizeof(g1));

    REF(alf_classify)(c0, t0, (const uint8_t *)sp, stride * sizeof(*sp), w, h, vb_pos, g0);
    SIMD(alf_classify)(c1, t1, (const uint8_t *)sp, stride * sizeof(*sp), w, h, vb_pos, g1);

    for (i = 0; i < gtotal; i++) {
        if (g0[i] != g1[i]) {
            printf("MISMATCH classify-grad w=%d h=%d vb=%d draw=%d idx=%d ref=%d sim=%d\n",
                   w, h, vb_pos, draw, i, g0[i], g1[i]);
            rc = 1;
            break;
        }
    }
    for (i = 0; i < nb && !rc; i++) {
        if (c0[i] != c1[i] || t0[i] != t1[i]) {
            printf("MISMATCH classify w=%d h=%d vb=%d draw=%d idx=%d ref=(%d,%d) sim=(%d,%d)\n",
                   w, h, vb_pos, draw, i, c0[i], t0[i], c1[i], t1[i]);
            rc = 1;
            break;
        }
    }
    if (rc)
        failures++;
    free(src);
    return rc;
}

int main(void)
{
    static const int widths[]  = { 4, 8, 12, 16, 20, 24, 64 };
    static const int heights[] = { 4, 8, 16, 32 };
    int nb = 0, nf = 0;

    for (int chroma = 0; chroma <= 1; chroma++) {
        for (size_t wi = 0; wi < sizeof(widths) / sizeof(*widths); wi++) {
            for (size_t hi = 0; hi < sizeof(heights) / sizeof(*heights); hi++) {
                const int w = widths[wi], h = heights[hi];
                const int vbs[] = { -40, 0, 1, 2, 3, 4, 7, h - 3, h - 2, h - 1, h,
                                    h + 1, h + 3, h + 9, 1000 };
                for (size_t vi = 0; vi < sizeof(vbs) / sizeof(*vbs); vi++) {
                    for (int draw = 0; draw < 3; draw++) {
                        nb++;
                        nf += run_case(chroma, w, h, vbs[vi], draw);
                    }
                }
            }
        }
    }
    {
        static const int cw[] = { 4, 8, 16, 32, 64 };
        static const int ch[] = { 4, 8, 16, 32 };
        for (size_t wi = 0; wi < sizeof(cw) / sizeof(*cw); wi++) {
            for (size_t hi = 0; hi < sizeof(ch) / sizeof(*ch); hi++) {
                const int vbs[] = { -40, 0, 1, 2, 3, 4, 7, 15, 31, 1000 };
                for (size_t vi = 0; vi < sizeof(vbs) / sizeof(*vbs); vi++) {
                    for (int draw = 0; draw < 2; draw++) {
                        nb++; nf += test_classify(cw[wi], ch[hi], vbs[vi], draw);
                    }
                }
            }
        }
    }
    printf("%s: %d cases, %d failures\n", failures ? "FAIL" : "PASS", nb, nf);
    return failures ? 1 : 0;
}
