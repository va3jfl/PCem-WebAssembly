/*
 * voodoo_ab.c — A/B framebuffer oracle for the Voodoo wasm span recompiler.
 *
 * The portable C rasteriser is the reference. This renders identical geometry
 * through BOTH paths — the C rasteriser (use_recompiler = 0) and the wasm span
 * JIT (use_recompiler = 1) — into a private framebuffer, and memcmp()s the
 * colour and depth buffers. Any mismatch is a bug in the emitted span.
 *
 * Sweeps:
 *   1. the original untextured core (write-mask x depth-op x gradients — the
 *      63-case milestone sweep, kept verbatim)
 *   2. the extended pixel pipe: depth bias/source/W-buffer, dither 4x4/2x2,
 *      fog in all five modes, alpha test (all funcs), alpha blending
 *      (src/dest factor matrix), combine-unit variations
 *   3. textured spans: point/bilinear x perspective/affine x wrap/clamp x
 *      mirror, LOD selection, chroma key, texture-fed combine paths
 *   4. fallback: uncovered modes must render byte-identical (both fall back
 *      to the C rasteriser) and must compile zero blocks.
 *
 * Exposed to JS as pcem_voodoo_ab_test(); voodoo_ab_test.mjs asserts zero
 * mismatches and zero wrongly-compiled fallback blocks.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#ifdef __EMSCRIPTEN__

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

#include "ibm.h"
#include "device.h"
#include "mem.h"
#include "thread.h"
#include "video.h"
#include "vid_svga.h"
#include "vid_voodoo.h"
#include "vid_voodoo_common.h"
#include "vid_voodoo_regs.h"
#include "vid_voodoo_render.h"
#include "wasm-emit.h" /* wasm_jit_live_count(): proves fallback cases install no blocks */

/* voodoo_triangle is the public rasteriser entry (vid_voodoo_render.c). */
extern void voodoo_triangle(voodoo_t *voodoo, voodoo_params_t *params, int odd_even);
/* gate probe (vid_voodoo_codegen_wasm.h): "would this state compile?" —
   for the register configs the C rasteriser fatal()s on, which can't be
   proven by an A/B render. */
extern int voodoo_wasm_gate_probe(voodoo_t *voodoo, voodoo_params_t *params, int xdir);

#define AB_W 200
#define AB_H 150
#define AB_FB_SIZE (4 * 1024 * 1024) /* colour + aux live in one fb_mem block */
#define AB_ROW (AB_W * 2)            /* bytes per row (16bpp) */
#define AB_DRAW_OFF 0
#define AB_AUX_OFF (AB_H * AB_ROW)

#define AB_TEX_WORDS (128 * 1024) /* all mip levels of a 256x256 texture */

static int ab_mismatches;
static int ab_cases;
static int ab_first_bad_case = -1;
static int ab_first_bad_off = -1;
static int ab_first_bad_ref = -1;
static int ab_first_bad_got = -1;
static int ab_fallback_mismatches;
static int ab_fallback_cases;
static int ab_fallback_installed;
static int ab_covered_installed; /* blocks compiled for the covered sweeps — proves the JIT path actually engaged */
static int ab_phase;

static uint32_t *ab_texdata;

/* Minimal voodoo_t: the covered span paths touch fb_mem, the tiling flags,
   use_recompiler, odd_even_mask, type, dual_tmus, bilinear/dithersub config
   and the texture cache entry the params point at. */
static voodoo_t *ab_make_voodoo(void) {
        voodoo_t *v = calloc(1, sizeof(voodoo_t));
        int i;
        if (!v)
                return NULL;
        v->fb_mem = calloc(1, AB_FB_SIZE);
        v->fb_mask = AB_FB_SIZE - 1;
        v->col_tiled = 0;
        v->aux_tiled = 0;
        v->odd_even_mask = 0;
        v->type = VOODOO_2;
        v->dual_tmus = 0;
        v->bilinear_enabled = 1;
        v->dithersub_enabled = 0;
        v->row_width = AB_ROW;
        v->aux_row_width = AB_ROW;

        /* deterministic RGBA texture data for every mip level */
        ab_texdata = malloc(AB_TEX_WORDS * 4);
        if (!ab_texdata) {
                free(v->fb_mem);
                free(v);
                return NULL;
        }
        {
                uint32_t lcg = 0x12345678;
                for (i = 0; i < AB_TEX_WORDS; i++) {
                        lcg = lcg * 1664525 + 1013904223;
                        ab_texdata[i] = lcg;
                }
        }
        v->texture_cache[0][0].data = ab_texdata;
        v->texture_cache[1][0].data = ab_texdata;
        return v;
}

static void ab_free_voodoo(voodoo_t *v) {
        free(ab_texdata);
        ab_texdata = NULL;
        free(v->fb_mem);
        free(v);
}

static void ab_params_flat_tri(voodoo_params_t *p, uint32_t fbzMode, int zop, uint32_t startZ, int32_t dZdX,
                               uint32_t startR, uint32_t startG, uint32_t startB, int32_t dRdX, int32_t dGdX, int32_t dBdX) {
        int l;
        memset(p, 0, sizeof(*p));
        /* a triangle covering much of the buffer (fixed-point .4 for x/y) */
        p->vertexAx = 10 << 4;
        p->vertexAy = 10 << 4;
        p->vertexBx = 190 << 4;
        p->vertexBy = 20 << 4;
        p->vertexCx = 100 << 4;
        p->vertexCy = 140 << 4;

        p->startR = startR;
        p->startG = startG;
        p->startB = startB;
        p->startA = 0xff << 12;
        p->startZ = startZ;
        p->dRdX = dRdX;
        p->dGdX = dGdX;
        p->dBdX = dBdX;
        p->dZdX = dZdX;
        /* small Y gradients so successive rows differ */
        p->dRdY = dRdX / 2;
        p->dGdY = dGdX / 2;
        p->dBdY = dBdX / 2;
        p->dZdY = dZdX / 2;

        p->fbzColorPath = CC_LOCALSELECT_ITER_RGB; /* combine passthrough → iterated RGB */
        p->fbzMode = fbzMode | ((zop & 7) << 5);
        p->alphaMode = 0;
        p->fogMode = 0;
        p->zaColor = 0;

        /* texture plumbing (unused unless FBZCP_TEXTURE_ENABLED is set):
           a 256x256 square texture with the full mip chain */
        for (l = 0; l <= LOD_MAX + 1; l++) {
                int sz = (l <= 8) ? (256 >> l) : 1;
                p->tex_w_mask[0][l] = sz - 1;
                p->tex_h_mask[0][l] = sz - 1;
                p->tex_shift[0][l] = (l <= 8) ? (8 - l) : 0;
                p->tex_lod[0][l] = (l <= 8) ? l : 8;
                p->tex_w_mask[1][l] = sz - 1;
                p->tex_h_mask[1][l] = sz - 1;
                p->tex_shift[1][l] = (l <= 8) ? (8 - l) : 0;
                p->tex_lod[1][l] = (l <= 8) ? l : 8;
        }
        p->tex_entry[0] = 0;
        p->tex_entry[1] = 0;
        p->tLOD[0] = 0x800; /* lod_min 0, lod_max 8<<8 */
        p->tLOD[1] = 0x800;
        p->tformat[0] = 0;
        p->tformat[1] = 0;

        p->draw_offset = AB_DRAW_OFF;
        p->aux_offset = AB_AUX_OFF;
        p->row_width = AB_ROW;
        p->aux_row_width = AB_ROW;
        p->col_tiled = 0;
        p->aux_tiled = 0;
        p->clipLeft = 0;
        p->clipRight = AB_W;
        p->clipLowY = 0;
        p->clipHighY = AB_H;
}

/* TMU1 texture coordinates (dual-TMU cases) — different gradients so the two
   TMUs sample different texels */
static void ab_params_texture_tmu1(voodoo_params_t *p, int perspective, int bilinear_mode) {
        p->textureMode[1] = bilinear_mode ? 6 : 0;
        if (perspective) {
                p->textureMode[1] |= 1;
                p->tmu[1].startS = (int64_t)70 << 20;
                p->tmu[1].startT = (int64_t)15 << 20;
                p->tmu[1].startW = (int64_t)1 << 23;
                p->tmu[1].dSdX = (int64_t)2 << 18;
                p->tmu[1].dTdX = (int64_t)3 << 17;
                p->tmu[1].dWdX = (int64_t)1 << 11;
                p->tmu[1].dSdY = (int64_t)3 << 17;
                p->tmu[1].dTdY = (int64_t)1 << 18;
                p->tmu[1].dWdY = (int64_t)1 << 10;
        } else {
                p->tmu[1].startS = (int64_t)30 << 32;
                p->tmu[1].startT = (int64_t)12 << 32;
                p->tmu[1].startW = 0;
                p->tmu[1].dSdX = (int64_t)1 << 30;
                p->tmu[1].dTdX = (int64_t)5 << 29;
                p->tmu[1].dWdX = 0;
                p->tmu[1].dSdY = (int64_t)5 << 29;
                p->tmu[1].dTdY = (int64_t)1 << 30;
                p->tmu[1].dWdY = 0;
        }
}

/* affine texture coordinates: texel s = startS>>32 at the A vertex, moving
   ds texels per pixel */
static void ab_params_texture(voodoo_params_t *p, int perspective, int bilinear_mode) {
        p->fbzColorPath |= FBZCP_TEXTURE_ENABLED;
        p->fbzColorPath = (p->fbzColorPath & ~3u) | CC_LOCALSELECT_TEX; /* decal */
        p->textureMode[0] = bilinear_mode ? 6 : 0;
        if (perspective) {
                p->textureMode[0] |= 1;
                p->tmu[0].startS = (int64_t)40 << 20;
                p->tmu[0].startT = (int64_t)25 << 20;
                p->tmu[0].startW = (int64_t)1 << 24;
                p->tmu[0].dSdX = (int64_t)3 << 18;
                p->tmu[0].dTdX = (int64_t)1 << 18;
                p->tmu[0].dWdX = (int64_t)1 << 12;
                p->tmu[0].dSdY = (int64_t)1 << 18;
                p->tmu[0].dTdY = (int64_t)2 << 18;
                p->tmu[0].dWdY = (int64_t)1 << 11;
        } else {
                p->tmu[0].startS = (int64_t)8 << 32;
                p->tmu[0].startT = (int64_t)4 << 32;
                p->tmu[0].startW = 0;
                p->tmu[0].dSdX = (int64_t)3 << 30; /* 3/4 texel per pixel */
                p->tmu[0].dTdX = (int64_t)1 << 29;
                p->tmu[0].dWdX = 0;
                p->tmu[0].dSdY = (int64_t)1 << 29;
                p->tmu[0].dTdY = (int64_t)3 << 30;
                p->tmu[0].dWdY = 0;
        }
}

static void ab_render(voodoo_t *v, voodoo_params_t *p, int use_recompiler) {
        memset(v->fb_mem, 0x35, AB_FB_SIZE); /* non-zero canvas: blends/depth reads see real data */
        v->params = *p;
        v->use_recompiler = use_recompiler;
        voodoo_triangle(v, p, 0);
}

static int ab_one_case(voodoo_t *v, voodoo_params_t *p) {
        static uint8_t ref[AB_FB_SIZE];
        int bad = 0;
        int n = AB_AUX_OFF + AB_H * AB_ROW;
        int i;

        if (ab_phase == 0)
                ab_cases++;
        else
                ab_fallback_cases++;

        n = AB_FB_SIZE; /* tiled layouts scatter across the block — compare it all */

        ab_render(v, p, 0); /* C rasteriser reference */
        memcpy(ref, v->fb_mem, n);

        ab_render(v, p, 1); /* wasm span JIT */

        if (memcmp(ref, v->fb_mem, n) != 0) {
                bad = 1;
                if (ab_phase == 0)
                        ab_mismatches++;
                else
                        ab_fallback_mismatches++;
                if (ab_first_bad_case < 0) {
                        ab_first_bad_case = (ab_phase == 0) ? ab_cases : 1000 + ab_fallback_cases;
                        for (i = 0; i < n; i++) {
                                if (ref[i] != ((uint8_t *)v->fb_mem)[i]) {
                                        ab_first_bad_off = i;
                                        ab_first_bad_ref = ref[i];
                                        ab_first_bad_got = ((uint8_t *)v->fb_mem)[i];
                                        break;
                                }
                        }
                }
        }
        return !bad;
}

/* the standard covered-sweep base state */
static void ab_base(voodoo_params_t *p, int zop) {
        ab_params_flat_tri(p, FBZ_RGB_WMASK | FBZ_DEPTH_ENABLE | FBZ_DEPTH_WMASK, zop, 0x8000u << 12, 0x40 << 12, 40 << 12,
                           90 << 12, 150 << 12, 3 << 12, (-1) << 12, 2 << 12);
}

EMSCRIPTEN_KEEPALIVE
int pcem_voodoo_ab_test(void) {
        voodoo_t *v = ab_make_voodoo();
        voodoo_params_t p;
        int di, wi, gi, i, j;
        static const uint32_t wmasks[] = {
                FBZ_RGB_WMASK,
                FBZ_RGB_WMASK | FBZ_DEPTH_ENABLE | FBZ_DEPTH_WMASK,
                FBZ_RGB_WMASK | FBZ_DEPTH_ENABLE, /* depth test, no depth write */
        };

        ab_mismatches = 0;
        ab_cases = 0;
        ab_first_bad_case = -1;
        ab_first_bad_off = ab_first_bad_ref = ab_first_bad_got = -1;
        ab_fallback_mismatches = 0;
        ab_fallback_cases = 0;
        ab_fallback_installed = 0;
        ab_covered_installed = 0;
        ab_phase = 0;
        if (!v)
                return -1;
        ab_covered_installed = -voodoo_recomp; /* count compiles, not cache residency */

        /* ---- 1. the original untextured core sweep (63 cases, verbatim) ---- */
        for (wi = 0; wi < (int)(sizeof(wmasks) / sizeof(wmasks[0])); wi++) {
                for (di = 1; di <= 7; di++) { /* depth ops LESSTHAN..ALWAYS */
                        for (gi = 0; gi < 3; gi++) {
                                int32_t dR = (gi == 0) ? (3 << 12) : ((gi == 1) ? (-2 << 12) : 0);
                                int32_t dG = (gi == 0) ? (-1 << 12) : ((gi == 1) ? (4 << 12) : (2 << 12));
                                int32_t dB = (gi == 0) ? (2 << 12) : 0;
                                uint32_t sZ = 0x8000u << 12;
                                int32_t dZ = (gi == 0) ? (0x40 << 12) : ((gi == 1) ? -(0x30 << 12) : (0x10 << 12));

                                ab_params_flat_tri(&p, wmasks[wi], di, sZ, dZ, 40 << 12, 90 << 12, 150 << 12, dR, dG, dB);
                                ab_one_case(v, &p);
                        }
                }
        }

        /* ---- 2a. depth pipe extras: bias / source / W-buffer --------------- */
        for (di = 0; di < 3; di++) { /* LT, GT, ALWAYS */
                int zops[3] = {DEPTHOP_LESSTHAN, DEPTHOP_GREATERTHAN, DEPTHOP_ALWAYS};
                ab_base(&p, zops[di]);
                p.fbzMode |= FBZ_DEPTH_BIAS;
                p.zaColor = 0x0180;
                ab_one_case(v, &p);
                ab_base(&p, zops[di]);
                p.fbzMode |= FBZ_DEPTH_BIAS;
                p.zaColor = 0xff40; /* negative int16 bias */
                ab_one_case(v, &p);
                ab_base(&p, zops[di]);
                p.fbzMode |= FBZ_DEPTH_SOURCE;
                p.zaColor = 0x4123;
                ab_one_case(v, &p);
                ab_base(&p, zops[di]);
                p.fbzMode |= FBZ_W_BUFFER;
                p.startW = (int64_t)1 << 30;
                p.dWdX = -((int64_t)1 << 22);
                p.dWdY = (int64_t)1 << 21;
                ab_one_case(v, &p);
                /* W-buffer edge regions: w tiny (>=0xffff0000 clamp) and w huge */
                ab_base(&p, zops[di]);
                p.fbzMode |= FBZ_W_BUFFER;
                p.startW = (int64_t)1 << 44; /* upper-word set -> wd = 0 */
                p.dWdX = -((int64_t)1 << 36);
                p.dWdY = (int64_t)1 << 30;
                ab_one_case(v, &p);
                ab_base(&p, zops[di]);
                p.fbzMode |= FBZ_W_BUFFER;
                p.startW = 0x8000; /* below 0x10000 -> wd = 0xf001 */
                p.dWdX = 0x40;
                p.dWdY = 0x20;
                ab_one_case(v, &p);
        }

        /* ---- 2b. dither ------------------------------------------------------ */
        for (i = 0; i < 2; i++) {
                for (gi = 0; gi < 2; gi++) {
                        ab_base(&p, gi ? DEPTHOP_LESSTHAN : DEPTHOP_ALWAYS);
                        p.fbzMode |= FBZ_DITHER | (i ? FBZ_DITHER_2x2 : 0);
                        ab_one_case(v, &p);
                }
        }

        /* ---- 2c. fog ---------------------------------------------------------- */
        for (i = 0; i < 7; i++) {
                ab_base(&p, DEPTHOP_ALWAYS);
                p.fogColor.r = 0x40;
                p.fogColor.g = 0x80;
                p.fogColor.b = 0xc0;
                for (j = 0; j < 64; j++) {
                        p.fogTable[j].fog = (uint8_t)(j * 4 + 1);
                        p.fogTable[j].dfog = (uint8_t)(255 - j * 3);
                }
                switch (i) {
                case 0:
                        p.fogMode = FOG_ENABLE | FOG_CONSTANT;
                        break;
                case 1: /* fog table (needs w) */
                        p.fogMode = FOG_ENABLE;
                        break;
                case 2:
                        p.fogMode = FOG_ENABLE | FOG_ADD;
                        break;
                case 3:
                        p.fogMode = FOG_ENABLE | FOG_MULT;
                        break;
                case 4:
                        p.fogMode = FOG_ENABLE | FOG_Z;
                        break;
                case 5:
                        p.fogMode = FOG_ENABLE | FOG_ALPHA;
                        p.startA = 0x30 << 12;
                        p.dAdX = 1 << 12;
                        p.dAdY = 1 << 11;
                        break;
                case 6:
                        p.fogMode = FOG_ENABLE | FOG_W;
                        break;
                }
                if (i == 1 || i == 2 || i == 3 || i == 6) {
                        p.startW = (int64_t)1 << 28;
                        p.dWdX = (int64_t)1 << 21;
                        p.dWdY = (int64_t)1 << 20;
                }
                ab_one_case(v, &p);
        }

        /* ---- 2d. alpha test (all funcs) --------------------------------------- */
        for (i = 0; i < 8; i++) {
                ab_base(&p, DEPTHOP_ALWAYS);
                p.alphaMode = 1 | (i << 1) | (0x80u << 24);
                p.startA = 0x20 << 12;
                p.dAdX = 2 << 12;
                p.dAdY = 1 << 12;
                ab_one_case(v, &p);
        }

        /* ---- 2e. alpha blending: factor matrix -------------------------------- */
        {
                static const int srcf[] = {0, 1, 2, 3, 4, 5, 6, 7, 10};   /* incl. an unhandled value */
                static const int dstf[] = {0, 1, 2, 4, 5, 7, 0xf, 12, 3}; /* incl. saturate + unhandled */
                for (i = 0; i < (int)(sizeof(srcf) / sizeof(srcf[0])); i++) {
                        ab_base(&p, DEPTHOP_ALWAYS);
                        p.alphaMode = (1 << 4) | (srcf[i] << 8) | (dstf[i] << 12);
                        p.startA = 0x35 << 12;
                        p.dAdX = 1 << 12;
                        p.dAdY = 1 << 11;
                        ab_one_case(v, &p);
                }
                /* blend + dither, blend + depth test */
                ab_base(&p, DEPTHOP_ALWAYS);
                p.alphaMode = (1 << 4) | (1 << 8) | (5 << 12);
                p.fbzMode |= FBZ_DITHER;
                p.startA = 0x60 << 12;
                p.dAdX = 1 << 12;
                ab_one_case(v, &p);
                ab_base(&p, DEPTHOP_LESSTHAN);
                p.alphaMode = (1 << 4) | (1 << 8) | (5 << 12);
                p.startA = 0x60 << 12;
                p.dAdX = 1 << 12;
                ab_one_case(v, &p);
                /* alpha test + blend together */
                ab_base(&p, DEPTHOP_ALWAYS);
                p.alphaMode = 1 | (AFUNC_GREATERTHAN << 1) | (1 << 4) | (1 << 8) | (5 << 12) | (0x40u << 24);
                p.startA = 0x20 << 12;
                p.dAdX = 2 << 12;
                ab_one_case(v, &p);
        }

        /* ---- 2f. combine-unit variations (untextured) -------------------------- */
        {
                static const uint32_t cps[] = {
                        /* zero_other */
                        CC_LOCALSELECT_ITER_RGB | (1 << 8),
                        /* sub_clocal (clocal = iterated) */
                        CC_LOCALSELECT_ITER_RGB | (1 << 9),
                        /* localselect color0 + sub_clocal */
                        CC_LOCALSELECT_ITER_RGB | (1 << 4) | (1 << 9),
                        /* mselect = clocal */
                        CC_LOCALSELECT_ITER_RGB | (1 << 10),
                        /* mselect = aother */
                        CC_LOCALSELECT_ITER_RGB | (2 << 10),
                        /* mselect = alocal */
                        CC_LOCALSELECT_ITER_RGB | (3 << 10),
                        /* mselect = clocal + reverse_blend */
                        CC_LOCALSELECT_ITER_RGB | (1 << 10) | (1 << 13),
                        /* add clocal */
                        CC_LOCALSELECT_ITER_RGB | (1 << 9) | (1 << 14),
                        /* add alocal */
                        CC_LOCALSELECT_ITER_RGB | (2 << 14),
                        /* invert output */
                        CC_LOCALSELECT_ITER_RGB | (1 << 16),
                        /* color1 select */
                        CC_LOCALSELECT_COLOR1,
                        /* cca: localselect color0, sub, mselect alocal, add, invert */
                        CC_LOCALSELECT_ITER_RGB | (1 << 5) | (1 << 18) | (1 << 19) | (1 << 23) | (1 << 25),
                        /* cca: localselect iter_z, mselect aother, reverse */
                        CC_LOCALSELECT_ITER_RGB | (2 << 5) | (2 << 19) | (1 << 22),
                        /* a_sel color1 + cca zero_other */
                        CC_LOCALSELECT_ITER_RGB | (2 << 2) | (1 << 17),
                };
                for (i = 0; i < (int)(sizeof(cps) / sizeof(cps[0])); i++) {
                        ab_base(&p, DEPTHOP_ALWAYS);
                        p.fbzColorPath = cps[i];
                        p.color0 = 0x55c03567;
                        p.color1 = 0x99206b8f;
                        p.startA = 0x48 << 12;
                        p.dAdX = 1 << 12;
                        p.dAdY = 1 << 11;
                        /* give the alpha pipe something to consume so cca paths matter */
                        p.alphaMode = (1 << 4) | (1 << 8) | (5 << 12);
                        ab_one_case(v, &p);
                }
        }

        /* ---- 3. textured spans -------------------------------------------------- */
        for (i = 0; i < 4; i++) { /* point/bilinear x affine/perspective */
                ab_base(&p, DEPTHOP_ALWAYS);
                ab_params_texture(&p, i & 2, i & 1);
                ab_one_case(v, &p);
        }
        /* clamp S+T (coords run past the edge) */
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 1);
        p.textureMode[0] |= TEXTUREMODE_TCLAMPS | TEXTUREMODE_TCLAMPT;
        p.tmu[0].startS = -((int64_t)40 << 32); /* negative -> clamps at 0 */
        p.tmu[0].dSdX = (int64_t)1 << 31;
        ab_one_case(v, &p);
        /* wrap with negative coords */
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 0);
        p.tmu[0].startS = -((int64_t)40 << 32);
        ab_one_case(v, &p);
        /* mirror S/T */
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 1);
        p.tLOD[0] |= LOD_TMIRROR_S | LOD_TMIRROR_T;
        ab_one_case(v, &p);
        /* larger LOD via bigger gradients (affine lod comes from triangle setup) */
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 1);
        p.tmu[0].dSdX = (int64_t)7 << 32; /* several texels per pixel -> mip 2-3 */
        p.tmu[0].dTdX = (int64_t)5 << 31;
        ab_one_case(v, &p);
        /* perspective with strong W gradient (per-pixel LOD walks the chain) */
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 1, 1);
        p.tmu[0].dWdX = (int64_t)1 << 16;
        ab_one_case(v, &p);
        /* chroma key: key on a colour actually present in the texture */
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 0);
        p.fbzMode |= FBZ_CHROMAKEY;
        p.chromaKey_r = (ab_texdata[8 + 4 * 256] >> 16) & 0xff;
        p.chromaKey_g = (ab_texdata[8 + 4 * 256] >> 8) & 0xff;
        p.chromaKey_b = ab_texdata[8 + 4 * 256] & 0xff;
        ab_one_case(v, &p);
        /* modulate: tex colour x iterated (mselect TEXRGB on iterated other) */
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 1);
        p.fbzColorPath = (p.fbzColorPath & ~3u) | CC_LOCALSELECT_ITER_RGB | (5 << 10);
        ab_one_case(v, &p);
        /* texture alpha feeding blend (a_sel = tex) */
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 0);
        p.fbzColorPath |= (1 << 2); /* a_sel = tex alpha */
        p.alphaMode = (1 << 4) | (1 << 8) | (5 << 12);
        ab_one_case(v, &p);
        /* mselect = tex alpha */
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 1);
        p.fbzColorPath = (p.fbzColorPath & ~3u) | CC_LOCALSELECT_ITER_RGB | (4 << 10);
        ab_one_case(v, &p);
        /* localselect_override (sel from tex alpha bit 7) */
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 0);
        p.fbzColorPath |= (1 << 7) | (1 << 9); /* override + sub_clocal so clocal matters */
        p.color0 = 0x20406080;
        ab_one_case(v, &p);
        /* textured + fog-table + depth */
        ab_base(&p, DEPTHOP_LESSTHAN);
        ab_params_texture(&p, 1, 1);
        p.fogMode = FOG_ENABLE;
        p.fogColor.r = 0x30;
        p.fogColor.g = 0x60;
        p.fogColor.b = 0x90;
        for (j = 0; j < 64; j++) {
                p.fogTable[j].fog = (uint8_t)(j * 3);
                p.fogTable[j].dfog = (uint8_t)(j + 40);
        }
        p.startW = (int64_t)1 << 28;
        p.dWdX = (int64_t)1 << 21;
        p.dWdY = (int64_t)1 << 20;
        ab_one_case(v, &p);
        /* textured + dither + alpha test */
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 1);
        p.fbzMode |= FBZ_DITHER;
        p.fbzColorPath |= (1 << 2); /* tex alpha */
        p.alphaMode = 1 | (AFUNC_GREATERTHAN << 1) | (0x60u << 24);
        ab_one_case(v, &p);

        /* ---- right-to-left spans: mirrored triangle, several modes -------------- */
        for (i = 0; i < 3; i++) {
                ab_base(&p, DEPTHOP_LESSTHAN);
                if (i == 1)
                        p.fbzMode |= FBZ_DITHER;
                if (i == 2)
                        ab_params_texture(&p, 0, 1);
                p.vertexAx = 190 << 4;
                p.vertexBx = 10 << 4;
                p.vertexCx = 100 << 4;
                p.sign = 1;
                ab_one_case(v, &p);
        }

        /* ---- 3b. the full stack: SLI, tiled, dual-TMU, dithersub, trex, LFB ---- */
        /* SLI: the span itself is SLI-agnostic (row interleave is caller-side) */
        v->fbiInit1 |= FBIINIT1_SLI_ENABLE;
        ab_base(&p, DEPTHOP_LESSTHAN);
        ab_one_case(v, &p);
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 1);
        ab_one_case(v, &p);
        v->fbiInit1 &= ~FBIINIT1_SLI_ENABLE;

        /* tiled colour / aux buffers (row_width is the 32-row block stride) */
        ab_base(&p, DEPTHOP_ALWAYS);
        p.col_tiled = 1;
        p.row_width = 4096;
        ab_one_case(v, &p);
        ab_base(&p, DEPTHOP_LESSTHAN);
        p.aux_tiled = 1;
        p.row_width = 4096; /* non-tiled colour rows share row_width in the C */
        p.aux_row_width = 4096;
        ab_one_case(v, &p);
        ab_base(&p, DEPTHOP_LESSTHAN);
        p.col_tiled = 1;
        p.aux_tiled = 1;
        p.row_width = 4096;
        p.aux_row_width = 4096;
        p.fbzMode |= FBZ_DITHER;
        p.alphaMode = (1 << 4) | (1 << 8) | (5 << 12);
        p.startA = 0x55 << 12;
        p.dAdX = 1 << 12;
        ab_one_case(v, &p);

        /* dual-TMU: TMU0 pass-through (samples TMU1 only) */
        v->dual_tmus = 1;
        for (i = 0; i < 2; i++) {
                ab_base(&p, DEPTHOP_ALWAYS);
                ab_params_texture(&p, 0, 0);
                p.textureMode[0] = i ? 6 : 0; /* no combine bits => PASSTHROUGH */
                ab_params_texture_tmu1(&p, i, i);
                ab_one_case(v, &p);
        }
        /* dual-TMU fetch-and-blend: main-combine variants */
        for (i = 0; i < 6; i++) {
                ab_base(&p, DEPTHOP_ALWAYS);
                ab_params_texture(&p, 0, 1);
                ab_params_texture_tmu1(&p, 0, i & 1);
                p.detail_bias[0] = 40;
                p.detail_scale[0] = 3;
                p.detail_max[0] = 200;
                switch (i) {
                case 0: /* other=tex1 * (clocal factor) + clocal */
                        p.textureMode[0] |= (1 << 13) | (1 << 14) | (1 << 18);
                        break;
                case 1: /* zero_other, mselect alocal, add alocal, alpha side */
                        p.textureMode[0] |= (1 << 12) | (3 << 14) | (1 << 19) | (1 << 22) | (1 << 23) | (1 << 27);
                        break;
                case 2: /* mselect lod_frac + reverse + invert */
                        p.textureMode[0] |= (5 << 14) | (1 << 17) | (1 << 20) | (1 << 29);
                        break;
                case 3: /* mselect detail + trilinear (lod&1 flips reverse) */
                        p.textureMode[0] |= (1 << 13) | (4 << 14) | (1 << 18) | TEXTUREMODE_TRILINEAR;
                        break;
                case 4: /* mselect aother; alpha aother */
                        p.textureMode[0] |= (2 << 14) | (2 << 23) | (1 << 28);
                        break;
                case 5: /* TMU1 self-combine feeding the main stage */
                        p.textureMode[1] |= (1 << 13) | (1 << 14) | (1 << 18) | (1 << 22) | (1 << 23) | (1 << 27);
                        p.textureMode[0] |= (1 << 13) | (1 << 14) | (1 << 18);
                        p.detail_bias[1] = 30;
                        p.detail_scale[1] = 2;
                        p.detail_max[1] = 180;
                        break;
                }
                ab_one_case(v, &p);
        }
        v->dual_tmus = 0;

        /* dither-subtraction of the blend destination (4x4 and 2x2) */
        v->dithersub_enabled = 1;
        for (i = 0; i < 2; i++) {
                ab_base(&p, DEPTHOP_ALWAYS);
                p.fbzMode |= FBZ_DITHER_SUB | (i ? FBZ_DITHER_2x2 : 0) | FBZ_DITHER;
                p.alphaMode = (1 << 4) | (1 << 8) | (5 << 12);
                p.startA = 0x50 << 12;
                p.dAdX = 1 << 12;
                ab_one_case(v, &p);
        }
        v->dithersub_enabled = 0;

        /* trexInit1 TMU-config override (tmuConfig > 255 exercises the uint8
           cother truncation and the raw TEXRGB mselect path) */
        v->trexInit1[0] = 1 << 18;
        v->tmuConfig = 0x1234;
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 0); /* decal: cother = tex (truncated) */
        ab_one_case(v, &p);
        ab_base(&p, DEPTHOP_ALWAYS);
        ab_params_texture(&p, 0, 0);
        p.fbzColorPath = (p.fbzColorPath & ~3u) | CC_LOCALSELECT_ITER_RGB | (5 << 10); /* mselect TEXRGB: raw */
        ab_one_case(v, &p);
        v->trexInit1[0] = 0;
        v->tmuConfig = 0;

        /* LFB colour select (cother = 0 in the triangle path) */
        ab_base(&p, DEPTHOP_ALWAYS);
        p.fbzColorPath = CC_LOCALSELECT_LFB | (1 << 14) | (1 << 4); /* + add clocal(color0) */
        p.color0 = 0x00405060;
        ab_one_case(v, &p);

        ab_covered_installed += voodoo_recomp; /* spans the covered sweeps compiled */

        /* ---- 4. gate probes: configs the C rasteriser fatal()s on (cannot be
           A/B-rendered) must be rejected, and representative covered states
           accepted. Probes never render, so they must compile nothing. ------- */
        ab_phase = 1;
        {
                int recomp_before = voodoo_recomp;
                static const struct {
                        uint32_t cp_or;   /* OR'd into fbzColorPath */
                        uint32_t am;      /* alphaMode override (if nonzero) */
                        int expect;
                } probes[] = {
                        {3 << 2, 0, 0},        /* a_sel 3: fatal */
                        {3 << 5, 0, 0},        /* cca_localselect 3: fatal */
                        {6 << 10, 0, 0},       /* cc_mselect 6: fatal */
                        {7 << 10, 0, 0},       /* cc_mselect 7: fatal */
                        {5 << 19, 0, 0},       /* cca_mselect 5: fatal */
                        {6 << 19, 0, 0},       /* cca_mselect 6: fatal */
                        {7 << 19, 0, 0},       /* cca_mselect 7: fatal */
                        {3 << 14, 0, 0},       /* cc_add 3: fatal */
                        {0, (1u << 4) | (0xfu << 8), 0}, /* blend + ACOLORBEFOREFOG: fatal */
                        /* untextured states referencing texture outputs: stale reads */
                        {CC_LOCALSELECT_TEX, 0, 0},
                        {4 << 10, 0, 0},       /* cc_mselect TEX */
                        {5 << 10, 0, 0},       /* cc_mselect TEXRGB */
                        {1 << 2, 0, 0},        /* a_sel TEX */
                        {4 << 19, 0, 0},       /* cca_mselect TEX */
                        {1 << 7, 0, 0},        /* cc_localselect_override */
                        /* sanity: representative covered states must be accepted */
                        {0, 0, 1},                                     /* plain iterated */
                        {CC_LOCALSELECT_TEX | FBZCP_TEXTURE_ENABLED, 0, 1}, /* textured decal */
                        {(1u << 9) | (1u << 16), (1u << 4) | (1u << 8) | (5u << 12), 1}, /* combine + blend */
                };
                for (i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
                        int got;
                        ab_base(&p, DEPTHOP_ALWAYS);
                        p.fbzColorPath |= probes[i].cp_or;
                        if (probes[i].am)
                                p.alphaMode = probes[i].am;
                        ab_fallback_cases++;
                        got = voodoo_wasm_gate_probe(v, &p, 1);
                        if ((got ? 1 : 0) != probes[i].expect) {
                                ab_fallback_mismatches++;
                                if (ab_first_bad_case < 0)
                                        ab_first_bad_case = 1000 + i;
                        }
                }

                ab_fallback_installed = voodoo_recomp - recomp_before;
                if (ab_fallback_installed < 0)
                        ab_fallback_installed = 0;
        }

        ab_free_voodoo(v);
        return ab_mismatches + ab_fallback_mismatches;
}

EMSCRIPTEN_KEEPALIVE int pcem_voodoo_ab_cases(void) { return ab_cases; }
EMSCRIPTEN_KEEPALIVE int pcem_voodoo_ab_first_bad(void) { return ab_first_bad_case; }
EMSCRIPTEN_KEEPALIVE int pcem_voodoo_ab_first_bad_off(void) { return ab_first_bad_off; }
EMSCRIPTEN_KEEPALIVE int pcem_voodoo_ab_first_bad_ref(void) { return ab_first_bad_ref; }
EMSCRIPTEN_KEEPALIVE int pcem_voodoo_ab_first_bad_got(void) { return ab_first_bad_got; }
EMSCRIPTEN_KEEPALIVE int pcem_voodoo_ab_fallback_cases(void) { return ab_fallback_cases; }
EMSCRIPTEN_KEEPALIVE int pcem_voodoo_ab_fallback_mismatches(void) { return ab_fallback_mismatches; }
EMSCRIPTEN_KEEPALIVE int pcem_voodoo_ab_fallback_installed(void) { return ab_fallback_installed; }
EMSCRIPTEN_KEEPALIVE int pcem_voodoo_ab_covered_installed(void) { return ab_covered_installed; }

#endif /* __EMSCRIPTEN__ */
