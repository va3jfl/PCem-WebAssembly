/*
 * vid_voodoo_codegen_wasm.h — WebAssembly span recompiler for the Voodoo.
 *
 * Included by vid_voodoo_render.c under __EMSCRIPTEN__, exactly where the
 * native build includes vid_voodoo_codegen_x86-64.h. Provides
 * voodoo_get_block(), which emits a runtime wasm module implementing the
 * per-pixel span loop specialised to the current render state, and installs
 * it into the calling render thread's function table (wasm/jit/wasm-emit.c).
 *
 * Coverage (full pixel pipeline): iterated-RGB and single-TMU textured spans
 * (point + bilinear filtering, perspective correction, per-pixel LOD with
 * mirror/clamp/wrap), the complete colour/alpha combine unit, chroma key,
 * fog (constant / Z / alpha / W / fog-table), alpha test, alpha blending,
 * dithering (4x4 and 2x2), and the Z/W depth pipe (all depth ops, bias,
 * depth-source, W-buffer) — in both span directions. The emitted code bumps
 * state->pixel_count (and texel_count when texturing) exactly like the
 * native x86-64 recompiler, so fbiPixelsIn keeps advancing.
 *
 * Deliberately NOT covered (voodoo_wasm_supported() returns NULL and the
 * caller keeps using the portable C rasteriser — partial coverage
 * accelerates without ever corrupting):
 *   - SLI and tiled colour/aux buffers (Banshee)
 *   - dual-TMU fetch-and-blend (TMU0 not in LOCAL mode on dual-TMU boards)
 *   - trexInit1 TMU-config output override
 *   - dither-subtraction before alpha blend (dithersub_enabled)
 *   - combine-unit selects that fatal() in the C rasteriser
 *     (cc_mselect 6/7, cca_mselect 5..7, cc_add 3, cca_localselect 3,
 *      a_sel 3, src_afunc ACOLORBEFOREFOG) and the LFB colour select
 *
 * The A/B framebuffer oracle (wasm/jit/voodoo_ab.c) proves every covered
 * mode byte-for-byte against the C rasteriser, and proves the gate rejects
 * (rather than corrupts) everything else.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#ifndef _VID_VOODOO_CODEGEN_WASM_H_
#define _VID_VOODOO_CODEGEN_WASM_H_

#include <stddef.h>
#include "wasm-emit.h"

int voodoo_recomp = 0;

#define LOD_MASK (LOD_TMIRROR_S | LOD_TMIRROR_T)

/* Per-thread cache of compiled spans, full-field compare exactly like the
   native recompiler's block cache (vid_voodoo_codegen_x86-64.h). */
#define VOODOO_WASM_CACHE 32

typedef struct voodoo_wasm_block_t {
        int valid;
        int xdir;
        uint32_t alphaMode, fbzMode, fogMode, fbzColorPath;
        uint32_t textureMode0, textureMode1;
        uint32_t tLOD0, tLOD1;
        uint32_t trex18;
        int col_tiled, aux_tiled;
        uint32_t fn_idx;
} voodoo_wasm_block_t;

static __thread voodoo_wasm_block_t voodoo_wasm_cache[VOODOO_WASM_CACHE];
static __thread uint8_t voodoo_wasm_buf[98304];
static __thread uint8_t voodoo_wasm_mod[106496];

/* ---- emit-time field offsets -------------------------------------------- */
#define SO(f) ((uint32_t)offsetof(voodoo_state_t, f))
#define PO(f) ((uint32_t)offsetof(voodoo_params_t, f))

/* Locals. Wasm layout: params (i32 x4) are locals 0..3; declared locals
   follow, i64 group first, then i32 (see wasm_emit_wrap_module). */
enum {
        VL_STATE = 0,
        VL_PARAMS = 1,
        VL_XARG = 2,
        VL_REALY = 3,
        /* i64 locals */
        V64_S = 4,  /* tmu0_s iterator */
        V64_T,      /* tmu0_t iterator */
        V64_W,      /* tmu0_w iterator */
        V64_S1,     /* tmu1_s iterator */
        V64_T1,     /* tmu1_t iterator */
        V64_W1,     /* tmu1_w iterator */
        V64_WI,     /* w iterator      */
        V64_A,      /* scratch         */
        V64_B,      /* scratch (_w reciprocal) */
        /* i32 locals */
        VL_X,
        VL_IR,
        VL_IG,
        VL_IB,
        VL_IA,
        VL_Z,
        VL_FBMEM,
        VL_AUXMEM,
        VL_PIXCNT,
        VL_TEXCNT,
        VL_NDEPTH,
        VL_WDEPTH,
        VL_SR,      /* combine output src_r/g/b/a */
        VL_SG,
        VL_SB,
        VL_SA,
        VL_TR,      /* texture output tex_r/g/b/a (TMU0 / final) */
        VL_TG,
        VL_TB,
        VL_TA,
        VL_TR1,     /* TMU1 texture output */
        VL_TG1,
        VL_TB1,
        VL_TA1,
        VL_DR,      /* dest (fb) expanded */
        VL_DG,
        VL_DB,
        VL_LOD,
        VL_LOD1,    /* TMU1 lod */
        VL_LODF,    /* TMU0 lod_frac */
        VL_LODF1,   /* TMU1 lod_frac */
        VL_XT,      /* x_tiled */
        VL_FR,      /* TMU combine factor r/g/b/a — persist across stages */
        VL_FG,
        VL_FB2,
        VL_FA,
        VL_TL,      /* tex_lod[lod]    */
        VL_SMASK,
        VL_TMASK,
        VL_TBASE,   /* texel base ptr  */
        VL_TXS,     /* tex_s           */
        VL_TXT,     /* tex_t           */
        VL_S,
        VL_T,
        VL_DS,
        VL_DT,
        VL_D0,
        VL_D1,
        VL_D2,
        VL_D3,
        VL_T0,
        VL_T1,
        VL_T2,
        VL_T3,
        VL_CLR,     /* clocal r/g/b    */
        VL_CLG,
        VL_CLB,
        VL_ALOC,    /* alocal          */
        VL_AOTH,    /* aother          */
        VL_TMP,
        VL_TMP2,
        VL_TMP3,
        VL_END
};
#define VOODOO_N_I64 (VL_X - V64_S)
#define VOODOO_N_I32 (VL_END - VL_X)

/* ------------------------------------------------------------------------- */
/* small emission helpers                                                     */
/* ------------------------------------------------------------------------- */

/* clamp the i32 on the stack to [0,hi] (0xff / 0xffff) via tmp local */
static void ve_clamp(wasm_emit_t *e, int hi, int tmploc) {
        wemit_local_tee(e, tmploc);
        wemit_i32_const(e, 0);
        wemit_op(e, WOP_I32_LT_S);
        wemit_if(e);
        wemit_i32_const(e, 0);
        wemit_local_set(e, tmploc);
        wemit_end(e);
        wemit_local_get(e, tmploc);
        wemit_i32_const(e, hi);
        wemit_op(e, WOP_I32_GT_S);
        wemit_if(e);
        wemit_i32_const(e, hi);
        wemit_local_set(e, tmploc);
        wemit_end(e);
        wemit_local_get(e, tmploc);
}

/* dst = i32 load of state field */
static void ve_ld_state(wasm_emit_t *e, uint32_t off, int dst) {
        wemit_local_get(e, VL_STATE);
        wemit_load(e, WOP_I32_LOAD, off, 0);
        wemit_local_set(e, dst);
}
/* push i32 load of state field */
static void ve_push_state(wasm_emit_t *e, uint32_t off) {
        wemit_local_get(e, VL_STATE);
        wemit_load(e, WOP_I32_LOAD, off, 0);
}
/* push i32 load of params field */
static void ve_push_params(wasm_emit_t *e, uint32_t off) {
        wemit_local_get(e, VL_PARAMS);
        wemit_load(e, WOP_I32_LOAD, off, 0);
}
/* push u8 load of params field */
static void ve_push_params8(wasm_emit_t *e, uint32_t off) {
        wemit_local_get(e, VL_PARAMS);
        wemit_load(e, WOP_I32_LOAD8_U, off, 0);
}
/* store i32 local into state field */
static void ve_st_state(wasm_emit_t *e, uint32_t off, int src) {
        wemit_local_get(e, VL_STATE);
        wemit_local_get(e, src);
        wemit_store(e, WOP_I32_STORE, off, 0);
}
/* dst = i64 load of state field */
static void ve_ld_state64(wasm_emit_t *e, uint32_t off, int dst) {
        wemit_local_get(e, VL_STATE);
        wemit_load(e, WOP_I64_LOAD, off, 0);
        wemit_local_set(e, dst);
}
/* iterate: loc = loc +/- params->field (i32) */
static void ve_iter(wasm_emit_t *e, int loc, uint32_t off, int sub) {
        wemit_local_get(e, loc);
        ve_push_params(e, off);
        wemit_op(e, sub ? WOP_I32_SUB : WOP_I32_ADD);
        wemit_local_set(e, loc);
}
/* iterate 64-bit */
static void ve_iter64(wasm_emit_t *e, int loc, uint32_t off, int sub) {
        wemit_local_get(e, loc);
        wemit_local_get(e, VL_PARAMS);
        wemit_load(e, WOP_I64_LOAD, off, 0);
        wemit_op(e, sub ? WOP_I64_SUB : WOP_I64_ADD);
        wemit_local_set(e, loc);
}
/* push (loc >> 12) clamped to 0..0xff — the iterated-channel fetch */
static void ve_push_iter_clamped(wasm_emit_t *e, int loc) {
        wemit_local_get(e, loc);
        wemit_i32_const(e, 12);
        wemit_op(e, WOP_I32_SHR_S);
        ve_clamp(e, 0xff, VL_TMP);
}
/* top-of-stack = (top * pushed_other)/255 — caller pushed both */
static void ve_mul_div255(wasm_emit_t *e) {
        wemit_op(e, WOP_I32_MUL);
        wemit_i32_const(e, 255);
        wemit_op(e, WOP_I32_DIV_S);
}

/* clamp-or-wrap a texture coordinate local into [0, mask_local] (clamp) or
   & mask_local (wrap). Identical to C tex_read's handling in ALL cases:
   the C fast path only skips this when the coordinate is already in range,
   where these operations are the identity. */
static void ve_tex_coord(wasm_emit_t *e, int coord, int mask, int do_clamp) {
        if (do_clamp) {
                wemit_local_get(e, coord);
                wemit_i32_const(e, 0);
                wemit_op(e, WOP_I32_LT_S);
                wemit_if(e);
                wemit_i32_const(e, 0);
                wemit_local_set(e, coord);
                wemit_end(e);
                wemit_local_get(e, coord);
                wemit_local_get(e, mask);
                wemit_op(e, WOP_I32_GT_S);
                wemit_if(e);
                wemit_local_get(e, mask);
                wemit_local_set(e, coord);
                wemit_end(e);
        } else {
                wemit_local_get(e, coord);
                wemit_local_get(e, mask);
                wemit_op(e, WOP_I32_AND);
                wemit_local_set(e, coord);
        }
}

/* Load texel at (s_loc, t_loc) [after clamp/wrap] into dst:
   dst = *(uint32*)(TBASE + (s + (t << (8 - TL))) * 4) */
static void ve_tex_load(wasm_emit_t *e, int s_loc, int t_loc, int dst) {
        wemit_local_get(e, VL_TBASE);
        wemit_local_get(e, s_loc);
        wemit_local_get(e, t_loc);
        wemit_i32_const(e, 8);
        wemit_local_get(e, VL_TL);
        wemit_op(e, WOP_I32_SUB);
        wemit_op(e, WOP_I32_SHL);
        wemit_op(e, WOP_I32_ADD);
        wemit_i32_const(e, 2);
        wemit_op(e, WOP_I32_SHL);
        wemit_op(e, WOP_I32_ADD);
        wemit_load(e, WOP_I32_LOAD, 0, 0);
        wemit_local_set(e, dst);
}

/* push channel c of 4 bilinear texels weighted: sum((Tn>>sh & 0xff) * Dn) >> 8 */
static void ve_bilerp_channel(wasm_emit_t *e, int shift) {
        int t;
        static const int TS[4] = {VL_T0, VL_T1, VL_T2, VL_T3};
        static const int DS_[4] = {VL_D0, VL_D1, VL_D2, VL_D3};
        for (t = 0; t < 4; t++) {
                wemit_local_get(e, TS[t]);
                if (shift) {
                        wemit_i32_const(e, shift);
                        wemit_op(e, WOP_I32_SHR_U);
                }
                wemit_i32_const(e, 0xff);
                wemit_op(e, WOP_I32_AND);
                wemit_local_get(e, DS_[t]);
                wemit_op(e, WOP_I32_MUL);
                if (t)
                        wemit_op(e, WOP_I32_ADD);
        }
        wemit_i32_const(e, 8);
        wemit_op(e, WOP_I32_SHR_U);
}

/* ------------------------------------------------------------------------- */
/* support gate                                                               */
/* ------------------------------------------------------------------------- */
static int voodoo_wasm_supported(voodoo_t *voodoo, voodoo_params_t *params, voodoo_state_t *state, int odd_even) {
        uint32_t fbzCP = params->fbzColorPath;
        int rgb_sel = fbzCP & 3;
        int asel = (fbzCP >> 2) & 3;
        int ccalocal = (fbzCP >> 5) & 3;
        int mselect = (fbzCP >> 10) & 7;
        int camselect = (fbzCP >> 19) & 7;
        int add_sel = (fbzCP >> 14) & 3;

        (void)voodoo;
        (void)state;
        (void)odd_even;
        (void)rgb_sel;

        /* The only remaining exclusions are register configurations the C
           rasteriser itself fatal()s on — returning NULL preserves upstream's
           loud failure behaviour instead of silently rendering something —
           plus untextured states whose combine unit references the texture
           outputs (the C rasteriser would read stale/uninitialised
           state->tex_*; not a behaviour worth reproducing). Everything else
           compiles: textured (single-TMU, TMU0-passthrough and dual-TMU
           blend), tiled buffers, SLI, dithersub, trexInit1 override, LFB
           select, both span directions. */
        if (asel == 3)
                return 0;
        if (ccalocal == 3)
                return 0;
        if (mselect > CC_MSELECT_TEXRGB)
                return 0;
        if (camselect > CCA_MSELECT_TEX)
                return 0;
        if (add_sel == 3)
                return 0;
        if ((params->alphaMode & (1 << 4)) && ((params->alphaMode >> 8) & 0xf) == AFUNC_ACOLORBEFOREFOG)
                return 0;
        if (!(fbzCP & FBZCP_TEXTURE_ENABLED)) {
                if ((fbzCP & 3) == CC_LOCALSELECT_TEX || mselect == CC_MSELECT_TEX || mselect == CC_MSELECT_TEXRGB ||
                    asel == A_SEL_TEX || camselect == CCA_MSELECT_TEX || (fbzCP & (1 << 7)))
                        return 0;
        }
        return 1;
}

/* ------------------------------------------------------------------------- */
/* emission                                                                   */
/* ------------------------------------------------------------------------- */

/* Emit a texture fetch for one TMU — the wasm rendition of
   voodoo_tmu_fetch()/voodoo_get_texture()/tex_read{,_4}(). Reads the s64
   iterator trio (s64, s64+1 = t, s64+2 = w), leaves tex_r/g/b/a in
   ob..ob+3, the (>>8) lod in lodloc and lod_frac in lodfloc. */
static void voodoo_wasm_emit_tex_fetch(wasm_emit_t *e, voodoo_t *voodoo, voodoo_params_t *params, int tmu, int ob, int s64,
                                       int lodloc, int lodfloc) {
        uint32_t texmode = params->textureMode[tmu];
        int bilinear = voodoo->bilinear_enabled && (texmode & 6);
        int clamp_s = (texmode & TEXTUREMODE_TCLAMPS) != 0;
        int clamp_t = (texmode & TEXTUREMODE_TCLAMPT) != 0;
        uint32_t so_tmulod = tmu ? SO(tmu[1].lod) : SO(tmu[0].lod);
        uint32_t so_lodmin = tmu ? SO(lod_min[1]) : SO(lod_min[0]);
        uint32_t so_lodmax = tmu ? SO(lod_max[1]) : SO(lod_max[0]);
        uint32_t so_texlod = tmu ? SO(tex_lod[1]) : SO(tex_lod[0]);
        uint32_t so_wmask = tmu ? SO(tex_w_mask[1]) : SO(tex_w_mask[0]);
        uint32_t so_hmask = tmu ? SO(tex_h_mask[1]) : SO(tex_h_mask[0]);
        uint32_t so_tex = tmu ? SO(tex[1][0]) : SO(tex[0][0]);
        int c;

        if (texmode & 1) {
                /* perspective: _w = tmuN_w ? (1<<48)/tmuN_w : 0 (unsigned div) */
                wemit_i64_const(e, 0);
                wemit_local_set(e, V64_B);
                wemit_local_get(e, s64 + 2);
                wemit_op(e, WOP_I64_EQZ);
                wemit_op(e, WOP_I32_EQZ);
                wemit_if(e);
                wemit_i64_const(e, 1LL << 48);
                wemit_local_get(e, s64 + 2);
                wemit_op(e, WOP_I64_DIV_U);
                wemit_local_set(e, V64_B);
                wemit_end(e);
                /* tex_s = (int32)(((((tmu_s + (1<<13)) >> 14) * _w) + (1<<29)) >> 30) */
                for (c = 0; c < 2; c++) {
                        wemit_local_get(e, s64 + c);
                        wemit_i64_const(e, 1 << 13);
                        wemit_op(e, WOP_I64_ADD);
                        wemit_i64_const(e, 14);
                        wemit_op(e, WOP_I64_SHR_S);
                        wemit_local_get(e, V64_B);
                        wemit_op(e, WOP_I64_MUL);
                        wemit_i64_const(e, 1 << 29);
                        wemit_op(e, WOP_I64_ADD);
                        wemit_i64_const(e, 30);
                        wemit_op(e, WOP_I64_SHR_S);
                        wemit_op(e, WOP_I32_WRAP_I64);
                        wemit_local_set(e, c ? VL_TXT : VL_TXS);
                }
                /* lod = tmu[N].lod + fastlog(_w) - (19<<8) */
                wemit_local_get(e, V64_B);
                wemit_op(e, WOP_I64_EQZ);
                wemit_if(e);
                wemit_i32_const(e, (int32_t)0x80000000);
                wemit_local_set(e, VL_TMP);
                wemit_else(e);
                wemit_i32_const(e, 63);
                wemit_local_get(e, V64_B);
                wemit_op(e, WOP_I64_CLZ);
                wemit_op(e, WOP_I32_WRAP_I64);
                wemit_op(e, WOP_I32_SUB);
                wemit_local_set(e, VL_TMP2); /* exp */
                wemit_local_get(e, VL_TMP2);
                wemit_i32_const(e, 8);
                wemit_op(e, WOP_I32_GE_S);
                wemit_if(e);
                wemit_local_get(e, V64_B);
                wemit_local_get(e, VL_TMP2);
                wemit_i32_const(e, 8);
                wemit_op(e, WOP_I32_SUB);
                wemit_op(e, WOP_I64_EXTEND_I32_U);
                wemit_op(e, WOP_I64_SHR_U);
                wemit_op(e, WOP_I32_WRAP_I64);
                wemit_local_set(e, VL_TMP3);
                wemit_else(e);
                wemit_local_get(e, V64_B);
                wemit_i32_const(e, 8);
                wemit_local_get(e, VL_TMP2);
                wemit_op(e, WOP_I32_SUB);
                wemit_op(e, WOP_I64_EXTEND_I32_U);
                wemit_op(e, WOP_I64_SHL);
                wemit_op(e, WOP_I32_WRAP_I64);
                wemit_local_set(e, VL_TMP3);
                wemit_end(e);
                wemit_local_get(e, VL_TMP2);
                wemit_i32_const(e, 8);
                wemit_op(e, WOP_I32_SHL);
                wemit_i32_const(e, (int32_t)(uintptr_t)logtable);
                wemit_local_get(e, VL_TMP3);
                wemit_i32_const(e, 0xff);
                wemit_op(e, WOP_I32_AND);
                wemit_op(e, WOP_I32_ADD);
                wemit_load(e, WOP_I32_LOAD8_U, 0, 0);
                wemit_op(e, WOP_I32_OR);
                wemit_local_set(e, VL_TMP);
                wemit_end(e);
                ve_push_state(e, so_tmulod);
                wemit_local_get(e, VL_TMP);
                wemit_op(e, WOP_I32_ADD);
                wemit_i32_const(e, 19 << 8);
                wemit_op(e, WOP_I32_SUB);
                wemit_local_set(e, lodloc);
        } else {
                for (c = 0; c < 2; c++) {
                        wemit_local_get(e, s64 + c);
                        wemit_i64_const(e, 14 + 14);
                        wemit_op(e, WOP_I64_SHR_S);
                        wemit_op(e, WOP_I32_WRAP_I64);
                        wemit_local_set(e, c ? VL_TXT : VL_TXS);
                }
                ve_ld_state(e, so_tmulod, lodloc);
        }

        /* clamp lod to [lod_min, lod_max]; lod_frac = lod & 0xff; lod >>= 8 */
        wemit_local_get(e, lodloc);
        ve_push_state(e, so_lodmin);
        wemit_op(e, WOP_I32_LT_S);
        wemit_if(e);
        ve_ld_state(e, so_lodmin, lodloc);
        wemit_end(e);
        wemit_local_get(e, lodloc);
        ve_push_state(e, so_lodmax);
        wemit_op(e, WOP_I32_GT_S);
        wemit_if(e);
        ve_ld_state(e, so_lodmax, lodloc);
        wemit_end(e);
        wemit_local_get(e, lodloc);
        wemit_i32_const(e, 0xff);
        wemit_op(e, WOP_I32_AND);
        wemit_local_set(e, lodfloc);
        wemit_local_get(e, lodloc);
        wemit_i32_const(e, 8);
        wemit_op(e, WOP_I32_SHR_S);
        wemit_local_set(e, lodloc);

        /* per-LOD tables: tex_lod / w_mask / h_mask are int* in state; the
           texel base is state->tex[tmu][lod] */
        ve_push_state(e, so_texlod);
        wemit_local_get(e, lodloc);
        wemit_i32_const(e, 2);
        wemit_op(e, WOP_I32_SHL);
        wemit_op(e, WOP_I32_ADD);
        wemit_load(e, WOP_I32_LOAD, 0, 0);
        wemit_local_set(e, VL_TL);
        ve_push_state(e, so_wmask);
        wemit_local_get(e, lodloc);
        wemit_i32_const(e, 2);
        wemit_op(e, WOP_I32_SHL);
        wemit_op(e, WOP_I32_ADD);
        wemit_load(e, WOP_I32_LOAD, 0, 0);
        wemit_local_set(e, VL_SMASK);
        ve_push_state(e, so_hmask);
        wemit_local_get(e, lodloc);
        wemit_i32_const(e, 2);
        wemit_op(e, WOP_I32_SHL);
        wemit_op(e, WOP_I32_ADD);
        wemit_load(e, WOP_I32_LOAD, 0, 0);
        wemit_local_set(e, VL_TMASK);
        wemit_local_get(e, VL_STATE);
        wemit_local_get(e, lodloc);
        wemit_i32_const(e, 2);
        wemit_op(e, WOP_I32_SHL);
        wemit_op(e, WOP_I32_ADD);
        wemit_load(e, WOP_I32_LOAD, so_tex, 0);
        wemit_local_set(e, VL_TBASE);

        /* mirror-S/T (baked from tLOD bits): if (tex_s & 0x1000) tex_s = ~tex_s */
        if (params->tLOD[tmu] & LOD_TMIRROR_S) {
                wemit_local_get(e, VL_TXS);
                wemit_i32_const(e, 0x1000);
                wemit_op(e, WOP_I32_AND);
                wemit_if(e);
                wemit_local_get(e, VL_TXS);
                wemit_i32_const(e, -1);
                wemit_op(e, WOP_I32_XOR);
                wemit_local_set(e, VL_TXS);
                wemit_end(e);
        }
        if (params->tLOD[tmu] & LOD_TMIRROR_T) {
                wemit_local_get(e, VL_TXT);
                wemit_i32_const(e, 0x1000);
                wemit_op(e, WOP_I32_AND);
                wemit_if(e);
                wemit_local_get(e, VL_TXT);
                wemit_i32_const(e, -1);
                wemit_op(e, WOP_I32_XOR);
                wemit_local_set(e, VL_TXT);
                wemit_end(e);
        }

        if (bilinear) {
                /* tex_s -= 1 << (3 + tex_lod); s = tex_s >> tex_lod; ... */
                wemit_i32_const(e, 1);
                wemit_i32_const(e, 3);
                wemit_local_get(e, VL_TL);
                wemit_op(e, WOP_I32_ADD);
                wemit_op(e, WOP_I32_SHL);
                wemit_local_set(e, VL_TMP);
                for (c = 0; c < 2; c++) {
                        wemit_local_get(e, c ? VL_TXT : VL_TXS);
                        wemit_local_get(e, VL_TMP);
                        wemit_op(e, WOP_I32_SUB);
                        wemit_local_get(e, VL_TL);
                        wemit_op(e, WOP_I32_SHR_S);
                        wemit_local_set(e, c ? VL_T : VL_S);
                }
                wemit_local_get(e, VL_S);
                wemit_i32_const(e, 0xf);
                wemit_op(e, WOP_I32_AND);
                wemit_local_set(e, VL_DS);
                wemit_local_get(e, VL_T);
                wemit_i32_const(e, 0xf);
                wemit_op(e, WOP_I32_AND);
                wemit_local_set(e, VL_DT);
                wemit_local_get(e, VL_S);
                wemit_i32_const(e, 4);
                wemit_op(e, WOP_I32_SHR_S);
                wemit_local_set(e, VL_S);
                wemit_local_get(e, VL_T);
                wemit_i32_const(e, 4);
                wemit_op(e, WOP_I32_SHR_S);
                wemit_local_set(e, VL_T);
                wemit_i32_const(e, 16);
                wemit_local_get(e, VL_DS);
                wemit_op(e, WOP_I32_SUB);
                wemit_i32_const(e, 16);
                wemit_local_get(e, VL_DT);
                wemit_op(e, WOP_I32_SUB);
                wemit_op(e, WOP_I32_MUL);
                wemit_local_set(e, VL_D0);
                wemit_local_get(e, VL_DS);
                wemit_i32_const(e, 16);
                wemit_local_get(e, VL_DT);
                wemit_op(e, WOP_I32_SUB);
                wemit_op(e, WOP_I32_MUL);
                wemit_local_set(e, VL_D1);
                wemit_i32_const(e, 16);
                wemit_local_get(e, VL_DS);
                wemit_op(e, WOP_I32_SUB);
                wemit_local_get(e, VL_DT);
                wemit_op(e, WOP_I32_MUL);
                wemit_local_set(e, VL_D2);
                wemit_local_get(e, VL_DS);
                wemit_local_get(e, VL_DT);
                wemit_op(e, WOP_I32_MUL);
                wemit_local_set(e, VL_D3);
                for (c = 0; c < 4; c++) {
                        wemit_local_get(e, VL_S);
                        if (c & 1) {
                                wemit_i32_const(e, 1);
                                wemit_op(e, WOP_I32_ADD);
                        }
                        wemit_local_set(e, VL_TMP);
                        wemit_local_get(e, VL_T);
                        if (c & 2) {
                                wemit_i32_const(e, 1);
                                wemit_op(e, WOP_I32_ADD);
                        }
                        wemit_local_set(e, VL_TMP2);
                        ve_tex_coord(e, VL_TMP, VL_SMASK, clamp_s);
                        ve_tex_coord(e, VL_TMP2, VL_TMASK, clamp_t);
                        ve_tex_load(e, VL_TMP, VL_TMP2, VL_T0 + c);
                }
                ve_bilerp_channel(e, 16);
                wemit_local_set(e, ob + 0);
                ve_bilerp_channel(e, 8);
                wemit_local_set(e, ob + 1);
                ve_bilerp_channel(e, 0);
                wemit_local_set(e, ob + 2);
                ve_bilerp_channel(e, 24);
                wemit_local_set(e, ob + 3);
        } else {
                for (c = 0; c < 2; c++) {
                        wemit_local_get(e, c ? VL_TXT : VL_TXS);
                        wemit_i32_const(e, 4);
                        wemit_local_get(e, VL_TL);
                        wemit_op(e, WOP_I32_ADD);
                        wemit_op(e, WOP_I32_SHR_S);
                        wemit_local_set(e, c ? VL_T : VL_S);
                }
                ve_tex_coord(e, VL_S, VL_SMASK, clamp_s);
                ve_tex_coord(e, VL_T, VL_TMASK, clamp_t);
                ve_tex_load(e, VL_S, VL_T, VL_T0);
                wemit_local_get(e, VL_T0);
                wemit_i32_const(e, 16);
                wemit_op(e, WOP_I32_SHR_U);
                wemit_i32_const(e, 0xff);
                wemit_op(e, WOP_I32_AND);
                wemit_local_set(e, ob + 0);
                wemit_local_get(e, VL_T0);
                wemit_i32_const(e, 8);
                wemit_op(e, WOP_I32_SHR_U);
                wemit_i32_const(e, 0xff);
                wemit_op(e, WOP_I32_AND);
                wemit_local_set(e, ob + 1);
                wemit_local_get(e, VL_T0);
                wemit_i32_const(e, 0xff);
                wemit_op(e, WOP_I32_AND);
                wemit_local_set(e, ob + 2);
                wemit_local_get(e, VL_T0);
                wemit_i32_const(e, 24);
                wemit_op(e, WOP_I32_SHR_U);
                wemit_local_set(e, ob + 3);
        }
}

/* mask for the TMU combine multiplier: dst = reverse? 0xff : 0, where
   reverse = (TRILINEAR && (lod & 1)) ? baked_rev : !baked_rev, optionally
   inverted (the TMU1 alpha stage has the opposite polarity in the C code). */
static void ve_tc_revmask(wasm_emit_t *e, int trilinear, int baked_rev, int lodloc, int invert, int dst) {
        if (!trilinear) {
                int rev = !baked_rev;
                if (invert)
                        rev = !rev;
                wemit_i32_const(e, rev ? 0xff : 0);
                wemit_local_set(e, dst);
        } else {
                /* rev = (lod&1) ? baked : !baked  ==  (lod&1) ^ (!baked) */
                wemit_local_get(e, lodloc);
                wemit_i32_const(e, 1);
                wemit_op(e, WOP_I32_AND);
                if ((!baked_rev) ^ (invert ? 1 : 0)) {
                        wemit_i32_const(e, 1);
                        wemit_op(e, WOP_I32_XOR);
                }
                wemit_i32_const(e, 0xff);
                wemit_op(e, WOP_I32_MUL);
                wemit_local_set(e, dst);
        }
}

/* detail factor: dst = min((detail_bias[tmu] - lod) << detail_scale[tmu], detail_max[tmu]) */
static void ve_tc_detail(wasm_emit_t *e, int tmu, int lodloc, int dst) {
        uint32_t po_bias = tmu ? PO(detail_bias[1]) : PO(detail_bias[0]);
        uint32_t po_scale = tmu ? PO(detail_scale[1]) : PO(detail_scale[0]);
        uint32_t po_max = tmu ? PO(detail_max[1]) : PO(detail_max[0]);
        ve_push_params(e, po_bias);
        wemit_local_get(e, lodloc);
        wemit_op(e, WOP_I32_SUB);
        ve_push_params(e, po_scale);
        wemit_op(e, WOP_I32_SHL);
        wemit_local_set(e, dst);
        wemit_local_get(e, dst);
        ve_push_params(e, po_max);
        wemit_op(e, WOP_I32_GT_S);
        wemit_if(e);
        ve_push_params(e, po_max);
        wemit_local_set(e, dst);
        wemit_end(e);
}

/* Dual-TMU fetch-and-blend — voodoo_tmu_fetch_and_blend() emitted. Leaves the
   blended result in VL_TR..VL_TA. Factor locals VL_FR/FG/FB2/FA persist across
   stages exactly like the C function's locals (mselect 6/7 keeps the previous
   stage's factor; they are zeroed here per pixel, like the C locals' init). */
static void voodoo_wasm_emit_tex_dual_blend(wasm_emit_t *e, voodoo_t *voodoo, voodoo_params_t *params) {
        uint32_t tm0 = params->textureMode[0];
        uint32_t tm1 = params->textureMode[1];
        int tril0 = (tm0 & TEXTUREMODE_TRILINEAR) != 0;
        int tril1 = (tm1 & TEXTUREMODE_TRILINEAR) != 0;
        int c;

        for (c = 0; c < 4; c++) {
                wemit_i32_const(e, 0);
                wemit_local_set(e, VL_FR + c); /* FR, FG, FB2, FA = 0 per pixel */
        }

        voodoo_wasm_emit_tex_fetch(e, voodoo, params, 1, VL_TR1, V64_S1, VL_LOD1, VL_LODF1);

        /* ---- TMU1 self-combine (colour), gated on tc_sub_clocal_1 ---------- */
        if (tm1 & (1 << 13)) {
                int msel = (tm1 >> 14) & 7;
                ve_tc_revmask(e, tril1, (tm1 & (1 << 17)) != 0, VL_LOD1, 0, VL_S); /* colour mask */
                switch (msel) {
                case TC_MSELECT_ZERO:
                case TC_MSELECT_AOTHER: /* aother is 0 in the TMU1 stage */
                        for (c = 0; c < 3; c++) {
                                wemit_i32_const(e, 0);
                                wemit_local_set(e, VL_FR + c);
                        }
                        break;
                case TC_MSELECT_CLOCAL:
                        for (c = 0; c < 3; c++) {
                                wemit_local_get(e, VL_TR1 + c);
                                wemit_local_set(e, VL_FR + c);
                        }
                        break;
                case TC_MSELECT_ALOCAL:
                        for (c = 0; c < 3; c++) {
                                wemit_local_get(e, VL_TA1);
                                wemit_local_set(e, VL_FR + c);
                        }
                        break;
                case TC_MSELECT_DETAIL:
                        ve_tc_detail(e, 1, VL_LOD1, VL_FR);
                        wemit_local_get(e, VL_FR);
                        wemit_local_set(e, VL_FG);
                        wemit_local_get(e, VL_FR);
                        wemit_local_set(e, VL_FB2);
                        break;
                case TC_MSELECT_LOD_FRAC:
                        for (c = 0; c < 3; c++) {
                                wemit_local_get(e, VL_LODF1);
                                wemit_local_set(e, VL_FR + c);
                        }
                        break;
                default: /* 6/7: factor keeps its previous value */
                        break;
                }
                for (c = 0; c < 3; c++) {
                        /* r = (-tex1 * ((F^mask)+1)) >> 8 [+ adds]; tex1 = CLAMP(r) */
                        wemit_i32_const(e, 0);
                        wemit_local_get(e, VL_TR1 + c);
                        wemit_op(e, WOP_I32_SUB);
                        wemit_local_get(e, VL_FR + c);
                        wemit_local_get(e, VL_S);
                        wemit_op(e, WOP_I32_XOR);
                        wemit_i32_const(e, 1);
                        wemit_op(e, WOP_I32_ADD);
                        wemit_op(e, WOP_I32_MUL);
                        wemit_i32_const(e, 8);
                        wemit_op(e, WOP_I32_SHR_S);
                        if (tm1 & (1 << 18)) { /* tc_add_clocal_1 */
                                wemit_local_get(e, VL_TR1 + c);
                                wemit_op(e, WOP_I32_ADD);
                        } else if (tm1 & (1 << 19)) { /* tc_add_alocal_1 */
                                wemit_local_get(e, VL_TA1);
                                wemit_op(e, WOP_I32_ADD);
                        }
                        ve_clamp(e, 0xff, VL_TMP);
                        wemit_local_set(e, VL_TR1 + c);
                }
        }
        /* ---- TMU1 self-combine (alpha), gated on tca_sub_clocal_1 ---------- */
        if (tm1 & (1 << 22)) {
                int msel = (tm1 >> 23) & 7;
                /* NOTE the C polarity flip: !a_reverse uses (f^0xff)+1 here */
                ve_tc_revmask(e, tril1, (tm1 & (1 << 26)) != 0, VL_LOD1, 1, VL_T);
                switch (msel) {
                case TCA_MSELECT_ZERO:
                case TCA_MSELECT_AOTHER:
                        wemit_i32_const(e, 0);
                        wemit_local_set(e, VL_FA);
                        break;
                case TCA_MSELECT_CLOCAL:
                case TCA_MSELECT_ALOCAL:
                        wemit_local_get(e, VL_TA1);
                        wemit_local_set(e, VL_FA);
                        break;
                case TCA_MSELECT_DETAIL:
                        ve_tc_detail(e, 1, VL_LOD1, VL_FA);
                        break;
                case TCA_MSELECT_LOD_FRAC:
                        wemit_local_get(e, VL_LODF1);
                        wemit_local_set(e, VL_FA);
                        break;
                default:
                        break;
                }
                wemit_i32_const(e, 0);
                wemit_local_get(e, VL_TA1);
                wemit_op(e, WOP_I32_SUB);
                wemit_local_get(e, VL_FA);
                wemit_local_get(e, VL_T);
                wemit_op(e, WOP_I32_XOR);
                wemit_i32_const(e, 1);
                wemit_op(e, WOP_I32_ADD);
                wemit_op(e, WOP_I32_MUL);
                wemit_i32_const(e, 8);
                wemit_op(e, WOP_I32_SHR_S);
                if (tm1 & ((1 << 27) | (1 << 28))) { /* tca_add_clocal_1 || tca_add_alocal_1 */
                        wemit_local_get(e, VL_TA1);
                        wemit_op(e, WOP_I32_ADD);
                }
                ve_clamp(e, 0xff, VL_TMP);
                wemit_local_set(e, VL_TA1);
        }

        voodoo_wasm_emit_tex_fetch(e, voodoo, params, 0, VL_TR, V64_S, VL_LOD, VL_LODF);

        /* ---- main combine: TMU0 local, TMU1 other --------------------------- */
        ve_tc_revmask(e, tril0, (tm0 & (1 << 17)) != 0, VL_LOD, 0, VL_S); /* colour mask */
        ve_tc_revmask(e, tril0, (tm0 & (1 << 26)) != 0, VL_LOD, 0, VL_T); /* alpha mask (a_reverse -> ^0xff) */
        {
                int msel = (tm0 >> 14) & 7;
                switch (msel) {
                case TC_MSELECT_ZERO:
                        for (c = 0; c < 3; c++) {
                                wemit_i32_const(e, 0);
                                wemit_local_set(e, VL_FR + c);
                        }
                        break;
                case TC_MSELECT_CLOCAL:
                        for (c = 0; c < 3; c++) {
                                wemit_local_get(e, VL_TR + c);
                                wemit_local_set(e, VL_FR + c);
                        }
                        break;
                case TC_MSELECT_AOTHER:
                        for (c = 0; c < 3; c++) {
                                wemit_local_get(e, VL_TA1);
                                wemit_local_set(e, VL_FR + c);
                        }
                        break;
                case TC_MSELECT_ALOCAL:
                        for (c = 0; c < 3; c++) {
                                wemit_local_get(e, VL_TA);
                                wemit_local_set(e, VL_FR + c);
                        }
                        break;
                case TC_MSELECT_DETAIL:
                        ve_tc_detail(e, 0, VL_LOD, VL_FR);
                        wemit_local_get(e, VL_FR);
                        wemit_local_set(e, VL_FG);
                        wemit_local_get(e, VL_FR);
                        wemit_local_set(e, VL_FB2);
                        break;
                case TC_MSELECT_LOD_FRAC:
                        for (c = 0; c < 3; c++) {
                                wemit_local_get(e, VL_LODF);
                                wemit_local_set(e, VL_FR + c);
                        }
                        break;
                default:
                        break;
                }
        }
        /* r/g/b into D0/D1/D2 (the original tex0 stays live as clocal) */
        for (c = 0; c < 3; c++) {
                if (tm0 & (1 << 12)) /* tc_zero_other */
                        wemit_i32_const(e, 0);
                else
                        wemit_local_get(e, VL_TR1 + c);
                if (tm0 & (1 << 13)) { /* tc_sub_clocal */
                        wemit_local_get(e, VL_TR + c);
                        wemit_op(e, WOP_I32_SUB);
                }
                wemit_local_get(e, VL_FR + c);
                wemit_local_get(e, VL_S);
                wemit_op(e, WOP_I32_XOR);
                wemit_i32_const(e, 1);
                wemit_op(e, WOP_I32_ADD);
                wemit_op(e, WOP_I32_MUL);
                wemit_i32_const(e, 8);
                wemit_op(e, WOP_I32_SHR_S);
                if (tm0 & (1 << 18)) { /* tc_add_clocal */
                        wemit_local_get(e, VL_TR + c);
                        wemit_op(e, WOP_I32_ADD);
                } else if (tm0 & (1 << 19)) { /* tc_add_alocal */
                        wemit_local_get(e, VL_TA);
                        wemit_op(e, WOP_I32_ADD);
                }
                wemit_local_set(e, VL_D0 + c);
        }
        /* alpha into D3 */
        {
                int msel = (tm0 >> 23) & 7;
                switch (msel) {
                case TCA_MSELECT_ZERO:
                        wemit_i32_const(e, 0);
                        wemit_local_set(e, VL_FA);
                        break;
                case TCA_MSELECT_CLOCAL:
                case TCA_MSELECT_ALOCAL:
                        wemit_local_get(e, VL_TA);
                        wemit_local_set(e, VL_FA);
                        break;
                case TCA_MSELECT_AOTHER:
                        wemit_local_get(e, VL_TA1);
                        wemit_local_set(e, VL_FA);
                        break;
                case TCA_MSELECT_DETAIL:
                        ve_tc_detail(e, 0, VL_LOD, VL_FA);
                        break;
                case TCA_MSELECT_LOD_FRAC:
                        wemit_local_get(e, VL_LODF);
                        wemit_local_set(e, VL_FA);
                        break;
                default:
                        break;
                }
                if (tm0 & (1 << 21)) /* tca_zero_other */
                        wemit_i32_const(e, 0);
                else
                        wemit_local_get(e, VL_TA1);
                if (tm0 & (1 << 22)) { /* tca_sub_clocal */
                        wemit_local_get(e, VL_TA);
                        wemit_op(e, WOP_I32_SUB);
                }
                wemit_local_get(e, VL_FA);
                wemit_local_get(e, VL_T);
                wemit_op(e, WOP_I32_XOR);
                wemit_i32_const(e, 1);
                wemit_op(e, WOP_I32_ADD);
                wemit_op(e, WOP_I32_MUL);
                wemit_i32_const(e, 8);
                wemit_op(e, WOP_I32_SHR_S);
                if (tm0 & ((1 << 27) | (1 << 28))) { /* tca_add_clocal || tca_add_alocal */
                        wemit_local_get(e, VL_TA);
                        wemit_op(e, WOP_I32_ADD);
                }
                wemit_local_set(e, VL_D3);
        }
        /* clamp into the final tex0 outputs, then baked inverts */
        for (c = 0; c < 3; c++) {
                wemit_local_get(e, VL_D0 + c);
                ve_clamp(e, 0xff, VL_TMP);
                if (tm0 & (1 << 20)) { /* tc_invert_output */
                        wemit_i32_const(e, 0xff);
                        wemit_op(e, WOP_I32_XOR);
                }
                wemit_local_set(e, VL_TR + c);
        }
        wemit_local_get(e, VL_D3);
        ve_clamp(e, 0xff, VL_TMP);
        if (tm0 & (1 << 29)) { /* tca_invert_output */
                wemit_i32_const(e, 0xff);
                wemit_op(e, WOP_I32_XOR);
        }
        wemit_local_set(e, VL_TA);
}

/* push clocal/alocal/... building blocks --------------------------------- */

/* clocal r/g/b into VL_CLR/CLG/CLB per cc_localselect / override */
static void voodoo_wasm_emit_clocal(wasm_emit_t *e, voodoo_params_t *params, int tex_live) {
        uint32_t fbzCP = params->fbzColorPath;
        int have_override = (fbzCP & (1 << 7)) != 0;
        int localselect = (fbzCP & (1 << 4)) != 0;
        int c;
        /* helper to emit one of the two sources */
        /* iterated: CLAMP(ir>>12); color0: params->color0 bytes r/g/b */
        if (have_override) {
                /* sel = tex_a & 0x80 (tex values live in locals if texturing,
                   else loaded from stale state at entry — matching the C) */
                (void)tex_live;
                wemit_local_get(e, VL_TA);
                wemit_i32_const(e, 0x80);
                wemit_op(e, WOP_I32_AND);
                wemit_if(e);
                ve_push_params8(e, PO(color0) + 2);
                wemit_local_set(e, VL_CLR);
                ve_push_params8(e, PO(color0) + 1);
                wemit_local_set(e, VL_CLG);
                ve_push_params8(e, PO(color0) + 0);
                wemit_local_set(e, VL_CLB);
                wemit_else(e);
                ve_push_iter_clamped(e, VL_IR);
                wemit_local_set(e, VL_CLR);
                ve_push_iter_clamped(e, VL_IG);
                wemit_local_set(e, VL_CLG);
                ve_push_iter_clamped(e, VL_IB);
                wemit_local_set(e, VL_CLB);
                wemit_end(e);
        } else if (localselect) {
                ve_push_params8(e, PO(color0) + 2);
                wemit_local_set(e, VL_CLR);
                ve_push_params8(e, PO(color0) + 1);
                wemit_local_set(e, VL_CLG);
                ve_push_params8(e, PO(color0) + 0);
                wemit_local_set(e, VL_CLB);
        } else {
                for (c = 0; c < 3; c++) {
                        ve_push_iter_clamped(e, VL_IR + c);
                        wemit_local_set(e, VL_CLR + c);
                }
        }
}

/* Emit the full span body. Returns module length or -1. */
static int voodoo_wasm_emit(voodoo_t *voodoo, voodoo_params_t *params, voodoo_state_t *state, uint8_t *out, int out_cap) {
        wasm_emit_t E, *e = &E;
        uint32_t fbzMode = params->fbzMode;
        uint32_t fbzCP = params->fbzColorPath;
        uint32_t alphaMode = params->alphaMode;
        uint32_t fogMode = params->fogMode;
        int depth_enable = (fbzMode & FBZ_DEPTH_ENABLE) != 0;
        int depth_wmask = (fbzMode & FBZ_DEPTH_WMASK) != 0;
        int rgb_wmask = (fbzMode & FBZ_RGB_WMASK) != 0;
        int w_buffer = (fbzMode & FBZ_W_BUFFER) != 0;
        int depth_bias = (fbzMode & FBZ_DEPTH_BIAS) != 0;
        int depth_source = (fbzMode & FBZ_DEPTH_SOURCE) != 0;
        int dither_en = (fbzMode & FBZ_DITHER) != 0;
        int dither2x2_en = (fbzMode & FBZ_DITHER_2x2) != 0;
        int zop = (fbzMode >> 5) & 7;
        int texture = (fbzCP & FBZCP_TEXTURE_ENABLED) != 0;
        int chroma = texture && (fbzMode & FBZ_CHROMAKEY);
        int rgb_sel = fbzCP & 3;
        int asel = (fbzCP >> 2) & 3;
        int localselect_override = (fbzCP & (1 << 7)) != 0;
        int ccalocal = (fbzCP >> 5) & 3;
        int zero_other = (fbzCP & (1 << 8)) != 0;
        int sub_clocal = (fbzCP & (1 << 9)) != 0;
        int mselect = (fbzCP >> 10) & 7;
        int reverse_blend = (fbzCP & (1 << 13)) != 0;
        int add_sel = (fbzCP >> 14) & 3;
        int invert_output = (fbzCP & (1 << 16)) != 0;
        int ca_zero_other = (fbzCP & (1 << 17)) != 0;
        int ca_sub_clocal = (fbzCP & (1 << 18)) != 0;
        int camselect = (fbzCP >> 19) & 7;
        int cca_rev = (fbzCP & (1 << 22)) != 0;
        int cca_add_sel = (fbzCP >> 23) & 3;
        int cca_invert = (fbzCP & (1 << 25)) != 0;
        int alpha_test = (alphaMode & 1) != 0;
        int alpha_blend = (alphaMode & (1 << 4)) != 0;
        int afunc = (alphaMode >> 1) & 7;
        int safunc_v = (alphaMode >> 8) & 0xf;
        int dafunc_v = (alphaMode >> 12) & 0xf;
        int aref_v = alphaMode >> 24;
        int fog_on = (fogMode & FOG_ENABLE) != 0;
        int fog_table = fog_on && !(fogMode & FOG_CONSTANT) && ((fogMode & (FOG_Z | FOG_ALPHA)) == 0);
        int need_wdepth = w_buffer || fog_table;
        int need_w64 = need_wdepth || (fog_on && !(fogMode & FOG_CONSTANT) && (fogMode & (FOG_Z | FOG_ALPHA)) == FOG_W);
        /* does the combine stage reference the texture outputs? */
        int tex_ref = (rgb_sel == CC_LOCALSELECT_TEX) || (mselect == CC_MSELECT_TEX) || (mselect == CC_MSELECT_TEXRGB) ||
                      (asel == A_SEL_TEX) || (camselect == CCA_MSELECT_TEX) || localselect_override;
        /* is src_a needed at all? (alpha test/blend, fog-alpha never needs
           src_a — it uses ia directly) */
        int need_srca = alpha_test || alpha_blend;
        int xdir_neg = state->xdir < 0;
        /* texture sampling mode — same precedence as the C rasteriser:
           LOCAL-or-single-TMU samples TMU0 only; PASSTHROUGH samples TMU1
           only; anything else on a dual-TMU board is fetch-and-blend. */
        int tex_single = texture && ((params->textureMode[0] & TEXTUREMODE_LOCAL_MASK) == TEXTUREMODE_LOCAL || !voodoo->dual_tmus);
        int tex_pass = texture && !tex_single && (params->textureMode[0] & TEXTUREMODE_MASK) == TEXTUREMODE_PASSTHROUGH;
        int tex_dual = texture && !tex_single && !tex_pass;
        int texels = tex_dual ? 2 : 1;
        /* tiling: the C span indexes by voodoo->params.{col,aux}_tiled */
        int col_tiled = voodoo->params.col_tiled != 0;
        int aux_tiled = voodoo->params.aux_tiled != 0;
        int any_tiled = col_tiled || aux_tiled;
        int trex_ovr = texture && (voodoo->trexInit1[0] & (1 << 18)) != 0;
        int dsub = alpha_blend && (fbzMode & FBZ_DITHER_SUB) && voodoo->dithersub_enabled;
        int loop_d, skip_d;
        int c;

        wasm_emit_init(e, voodoo_wasm_buf, sizeof(voodoo_wasm_buf));

        /* ---- prologue: x + accumulators + pointers --------------------- */
        wemit_local_get(e, VL_XARG);
        wemit_local_set(e, VL_X);
        ve_ld_state(e, SO(ir), VL_IR);
        ve_ld_state(e, SO(ig), VL_IG);
        ve_ld_state(e, SO(ib), VL_IB);
        ve_ld_state(e, SO(ia), VL_IA);
        ve_ld_state(e, SO(z), VL_Z);
        ve_ld_state(e, SO(fb_mem), VL_FBMEM);
        ve_ld_state(e, SO(aux_mem), VL_AUXMEM);
        ve_ld_state(e, SO(pixel_count), VL_PIXCNT);
        if (texture)
                ve_ld_state(e, SO(texel_count), VL_TEXCNT);
        if (tex_single || tex_dual) {
                ve_ld_state64(e, SO(tmu0_s), V64_S);
                ve_ld_state64(e, SO(tmu0_t), V64_T);
                ve_ld_state64(e, SO(tmu0_w), V64_W);
        }
        if (tex_pass || tex_dual) {
                ve_ld_state64(e, SO(tmu1_s), V64_S1);
                ve_ld_state64(e, SO(tmu1_t), V64_T1);
                ve_ld_state64(e, SO(tmu1_w), V64_W1);
        }
        (void)tex_ref; /* tex-referencing combine without texturing is gated off */
        if (need_w64)
                ve_ld_state64(e, SO(w), V64_WI);

        /* ---- block { loop { block(skip) ... } } ------------------------- */
        wemit_block(e); /* exit */
        wemit_loop(e);
        loop_d = e->depth;

        /* x_tiled = (x & 63) | ((x >> 6) * 2048) — per pixel, before the skip
           block so skipped pixels don't recompute (pure either way) */
        if (any_tiled) {
                wemit_local_get(e, VL_X);
                wemit_i32_const(e, 63);
                wemit_op(e, WOP_I32_AND);
                wemit_local_get(e, VL_X);
                wemit_i32_const(e, 6);
                wemit_op(e, WOP_I32_SHR_S);
                wemit_i32_const(e, 11);
                wemit_op(e, WOP_I32_SHL);
                wemit_op(e, WOP_I32_OR);
                wemit_local_set(e, VL_XT);
        }

        wemit_block(e); /* skip_pixel target */
        skip_d = e->depth;

        /* ---- w_depth ----------------------------------------------------- */
        if (need_wdepth) {
                /* if (w & 0xffff00000000) wd = 0
                   else if (!(w & 0xffff0000)) wd = 0xf001
                   else { exp = fls((uint16)((uint32)w >> 16));
                          mant = ((~(uint32)w) >> (19-exp)) & 0xfff;
                          wd = (exp<<12) + mant + 1; wd = min(wd, 0xffff) } */
                wemit_local_get(e, V64_WI);
                wemit_i64_const(e, 0xffff00000000LL);
                wemit_op(e, WOP_I64_AND);
                wemit_op(e, WOP_I64_EQZ);
                wemit_op(e, WOP_I32_EQZ);
                wemit_if(e);
                wemit_i32_const(e, 0);
                wemit_local_set(e, VL_WDEPTH);
                wemit_else(e);
                wemit_local_get(e, V64_WI);
                wemit_i64_const(e, 0xffff0000LL);
                wemit_op(e, WOP_I64_AND);
                wemit_op(e, WOP_I64_EQZ);
                wemit_if(e);
                wemit_i32_const(e, 0xf001);
                wemit_local_set(e, VL_WDEPTH);
                wemit_else(e);
                /* TMP = (uint32)w */
                wemit_local_get(e, V64_WI);
                wemit_op(e, WOP_I32_WRAP_I64);
                wemit_local_set(e, VL_TMP);
                /* TMP2 = exp = fls16(TMP >> 16) = clz32(TMP>>16) - 16 (arg != 0 here) */
                wemit_local_get(e, VL_TMP);
                wemit_i32_const(e, 16);
                wemit_op(e, WOP_I32_SHR_U);
                wemit_op(e, WOP_I32_CLZ);
                wemit_i32_const(e, 16);
                wemit_op(e, WOP_I32_SUB);
                wemit_local_set(e, VL_TMP2);
                /* WDEPTH = (exp<<12) + ((~TMP >> (19-exp)) & 0xfff) + 1 */
                wemit_local_get(e, VL_TMP2);
                wemit_i32_const(e, 12);
                wemit_op(e, WOP_I32_SHL);
                wemit_local_get(e, VL_TMP);
                wemit_i32_const(e, -1);
                wemit_op(e, WOP_I32_XOR);
                wemit_i32_const(e, 19);
                wemit_local_get(e, VL_TMP2);
                wemit_op(e, WOP_I32_SUB);
                wemit_op(e, WOP_I32_SHR_U);
                wemit_i32_const(e, 0xfff);
                wemit_op(e, WOP_I32_AND);
                wemit_op(e, WOP_I32_ADD);
                wemit_i32_const(e, 1);
                wemit_op(e, WOP_I32_ADD);
                wemit_local_set(e, VL_WDEPTH);
                wemit_local_get(e, VL_WDEPTH);
                wemit_i32_const(e, 0xffff);
                wemit_op(e, WOP_I32_GT_S);
                wemit_if(e);
                wemit_i32_const(e, 0xffff);
                wemit_local_set(e, VL_WDEPTH);
                wemit_end(e);
                wemit_end(e);
                wemit_end(e);
        }

        /* ---- new_depth ---------------------------------------------------- */
        if (w_buffer) {
                wemit_local_get(e, VL_WDEPTH);
                wemit_local_set(e, VL_NDEPTH);
        } else {
                wemit_local_get(e, VL_Z);
                wemit_i32_const(e, 12);
                wemit_op(e, WOP_I32_SHR_S);
                ve_clamp(e, 0xffff, VL_TMP);
                wemit_local_set(e, VL_NDEPTH);
        }
        if (depth_bias) {
                /* new_depth = CLAMP16(new_depth + (int16)zaColor) */
                wemit_local_get(e, VL_NDEPTH);
                ve_push_params(e, PO(zaColor));
                wemit_op(e, WOP_I32_EXTEND16_S);
                wemit_op(e, WOP_I32_ADD);
                ve_clamp(e, 0xffff, VL_TMP);
                wemit_local_set(e, VL_NDEPTH);
        }

        /* ---- depth test ---------------------------------------------------- */
        if (depth_enable) {
                if (zop == DEPTHOP_NEVER) {
                        wemit_br(e, e->depth - skip_d);
                } else if (zop == DEPTHOP_ALWAYS) {
                        /* pass */
                } else if (depth_source) {
                        /* Upstream quirk, faithfully reproduced: DEPTH_TEST()'s
                           argument is substituted unparenthesised into
                           `!(comp_depth OP old_depth)`, so with FBZ_DEPTH_SOURCE
                           the ternary swallows the comparison and every
                           comparison op passes iff (zaColor & 0xffff) != 0.
                           (The A/B oracle caught this — byte-parity with the C
                           rasteriser is the contract, quirks included.) */
                        ve_push_params(e, PO(zaColor));
                        wemit_i32_const(e, 0xffff);
                        wemit_op(e, WOP_I32_AND);
                        wemit_op(e, WOP_I32_EQZ);
                        wemit_br_if(e, e->depth - skip_d);
                } else {
                        wemit_local_get(e, VL_NDEPTH);
                        /* old = aux_mem[x or x_tiled] */
                        wemit_local_get(e, VL_AUXMEM);
                        wemit_local_get(e, aux_tiled ? VL_XT : VL_X);
                        wemit_i32_const(e, 1);
                        wemit_op(e, WOP_I32_SHL);
                        wemit_op(e, WOP_I32_ADD);
                        wemit_load(e, WOP_I32_LOAD16_U, 0, 0);
                        switch (zop) {
                        case DEPTHOP_LESSTHAN:
                                wemit_op(e, WOP_I32_LT_S);
                                break;
                        case DEPTHOP_EQUAL:
                                wemit_op(e, WOP_I32_EQ);
                                break;
                        case DEPTHOP_LESSTHANEQUAL:
                                wemit_op(e, WOP_I32_LE_S);
                                break;
                        case DEPTHOP_GREATERTHAN:
                                wemit_op(e, WOP_I32_GT_S);
                                break;
                        case DEPTHOP_NOTEQUAL:
                                wemit_op(e, WOP_I32_NE);
                                break;
                        case DEPTHOP_GREATERTHANEQUAL:
                                wemit_op(e, WOP_I32_GE_S);
                                break;
                        }
                        wemit_op(e, WOP_I32_EQZ);
                        wemit_br_if(e, e->depth - skip_d);
                }
        }

        /* ---- dest read (blend only; C's unconditional read is pure) ------- */
        if (alpha_blend) {
                wemit_local_get(e, VL_FBMEM);
                wemit_local_get(e, col_tiled ? VL_XT : VL_X);
                wemit_i32_const(e, 1);
                wemit_op(e, WOP_I32_SHL);
                wemit_op(e, WOP_I32_ADD);
                wemit_load(e, WOP_I32_LOAD16_U, 0, 0);
                wemit_local_set(e, VL_TMP3); /* dat */
                /* dest_r = (dat>>8)&0xf8; dest_r |= dest_r>>5 */
                wemit_local_get(e, VL_TMP3);
                wemit_i32_const(e, 8);
                wemit_op(e, WOP_I32_SHR_U);
                wemit_i32_const(e, 0xf8);
                wemit_op(e, WOP_I32_AND);
                wemit_local_tee(e, VL_DR);
                wemit_i32_const(e, 5);
                wemit_op(e, WOP_I32_SHR_U);
                wemit_local_get(e, VL_DR);
                wemit_op(e, WOP_I32_OR);
                wemit_local_set(e, VL_DR);
                wemit_local_get(e, VL_TMP3);
                wemit_i32_const(e, 3);
                wemit_op(e, WOP_I32_SHR_U);
                wemit_i32_const(e, 0xfc);
                wemit_op(e, WOP_I32_AND);
                wemit_local_tee(e, VL_DG);
                wemit_i32_const(e, 6);
                wemit_op(e, WOP_I32_SHR_U);
                wemit_local_get(e, VL_DG);
                wemit_op(e, WOP_I32_OR);
                wemit_local_set(e, VL_DG);
                wemit_local_get(e, VL_TMP3);
                wemit_i32_const(e, 3);
                wemit_op(e, WOP_I32_SHL);
                wemit_i32_const(e, 0xf8);
                wemit_op(e, WOP_I32_AND);
                wemit_local_tee(e, VL_DB);
                wemit_i32_const(e, 5);
                wemit_op(e, WOP_I32_SHR_U);
                wemit_local_get(e, VL_DB);
                wemit_op(e, WOP_I32_OR);
                wemit_local_set(e, VL_DB);
                /* dither-subtraction of the blend destination (dest locals
                   only — equivalent to the C's placement just before
                   ALPHA_BLEND, since nothing reads dest in between) */
                if (dsub) {
                        const uint8_t *rbtbl = dither2x2_en ? &dithersub_rb2x2[0][0][0] : &dithersub_rb[0][0][0];
                        const uint8_t *gtbl = dither2x2_en ? &dithersub_g2x2[0][0][0] : &dithersub_g[0][0][0];
                        int dymask = dither2x2_en ? 1 : 3;
                        int dcell = dither2x2_en ? 2 : 4;
                        int drow = dither2x2_en ? 1 : 2;
                        for (c = 0; c < 3; c++) {
                                const uint8_t *tbl = (c == 1) ? gtbl : rbtbl;
                                wemit_i32_const(e, (int32_t)(uintptr_t)tbl);
                                wemit_local_get(e, VL_DR + c);
                                wemit_i32_const(e, dcell);
                                wemit_op(e, WOP_I32_SHL);
                                wemit_op(e, WOP_I32_ADD);
                                wemit_local_get(e, VL_REALY);
                                wemit_i32_const(e, dymask);
                                wemit_op(e, WOP_I32_AND);
                                wemit_i32_const(e, drow);
                                wemit_op(e, WOP_I32_SHL);
                                wemit_op(e, WOP_I32_ADD);
                                wemit_local_get(e, VL_X);
                                wemit_i32_const(e, dymask);
                                wemit_op(e, WOP_I32_AND);
                                wemit_op(e, WOP_I32_ADD);
                                wemit_load(e, WOP_I32_LOAD8_U, 0, 0);
                                wemit_local_set(e, VL_DR + c);
                        }
                }
        }

        /* ---- texture fetch + chroma key ----------------------------------- */
        if (texture) {
                if (tex_single)
                        voodoo_wasm_emit_tex_fetch(e, voodoo, params, 0, VL_TR, V64_S, VL_LOD, VL_LODF);
                else if (tex_pass)
                        /* TMU0 pass-through: sample TMU1 straight into the
                           final tex outputs (the C copies tex[1] -> tex[0]) */
                        voodoo_wasm_emit_tex_fetch(e, voodoo, params, 1, VL_TR, V64_S1, VL_LOD, VL_LODF);
                else
                        voodoo_wasm_emit_tex_dual_blend(e, voodoo, params);
                /* trexInit1 TMU-config output override (tmuConfig is a live
                   register: load it at run time from its baked address) */
                if (trex_ovr) {
                        wemit_i32_const(e, 0);
                        wemit_local_set(e, VL_TR);
                        wemit_i32_const(e, 0);
                        wemit_local_set(e, VL_TG);
                        wemit_i32_const(e, (int32_t)(uintptr_t)&voodoo->tmuConfig);
                        wemit_load(e, WOP_I32_LOAD, 0, 0);
                        wemit_local_set(e, VL_TB);
                }
                if (chroma) {
                        wemit_local_get(e, VL_TR);
                        ve_push_params(e, PO(chromaKey_r));
                        wemit_op(e, WOP_I32_EQ);
                        wemit_local_get(e, VL_TG);
                        ve_push_params(e, PO(chromaKey_g));
                        wemit_op(e, WOP_I32_EQ);
                        wemit_op(e, WOP_I32_AND);
                        wemit_local_get(e, VL_TB);
                        ve_push_params(e, PO(chromaKey_b));
                        wemit_op(e, WOP_I32_EQ);
                        wemit_op(e, WOP_I32_AND);
                        wemit_br_if(e, e->depth - skip_d);
                }
        }

        /* ---- colour combine ------------------------------------------------ */
        /* clocal (needed for sub_clocal / mselect==CLOCAL / add==CLOCAL) */
        if (sub_clocal || mselect == CC_MSELECT_CLOCAL || add_sel == CC_ADD_CLOCAL)
                voodoo_wasm_emit_clocal(e, params, texture);
        /* alocal (cca side + cc mselect/add uses) */
        if (ca_sub_clocal || camselect == CCA_MSELECT_ALOCAL || camselect == CCA_MSELECT_ALOCAL2 || mselect == CC_MSELECT_ALOCAL ||
            add_sel == CC_ADD_ALOCAL || (cca_add_sel != 0) || need_srca || fog_on) {
                switch (ccalocal) {
                case CCA_LOCALSELECT_ITER_A:
                        ve_push_iter_clamped(e, VL_IA);
                        wemit_local_set(e, VL_ALOC);
                        break;
                case CCA_LOCALSELECT_COLOR0:
                        ve_push_params8(e, PO(color0) + 3);
                        wemit_local_set(e, VL_ALOC);
                        break;
                case CCA_LOCALSELECT_ITER_Z:
                        wemit_local_get(e, VL_Z);
                        wemit_i32_const(e, 20);
                        wemit_op(e, WOP_I32_SHR_S);
                        ve_clamp(e, 0xff, VL_TMP);
                        wemit_local_set(e, VL_ALOC);
                        break;
                }
        }
        /* aother */
        if (need_srca || camselect == CCA_MSELECT_AOTHER || mselect == CC_MSELECT_AOTHER) {
                switch (asel) {
                case A_SEL_ITER_A:
                        ve_push_iter_clamped(e, VL_IA);
                        wemit_local_set(e, VL_AOTH);
                        break;
                case A_SEL_TEX:
                        wemit_local_get(e, VL_TA);
                        wemit_local_set(e, VL_AOTH);
                        break;
                case A_SEL_COLOR1:
                        ve_push_params8(e, PO(color1) + 3);
                        wemit_local_set(e, VL_AOTH);
                        break;
                }
        }

        /* src_r/g/b = zero_other ? 0 : cother  (cother per rgb_sel) */
        for (c = 0; c < 3; c++) {
                if (zero_other) {
                        wemit_i32_const(e, 0);
                } else {
                        switch (rgb_sel) {
                        case CC_LOCALSELECT_ITER_RGB:
                                ve_push_iter_clamped(e, VL_IR + c);
                                break;
                        case CC_LOCALSELECT_TEX:
                                /* uint8_t cother truncation — visible when the
                                   trex override injects tmuConfig (> 255) */
                                wemit_local_get(e, VL_TR + c);
                                wemit_i32_const(e, 0xff);
                                wemit_op(e, WOP_I32_AND);
                                break;
                        case CC_LOCALSELECT_COLOR1:
                                ve_push_params8(e, PO(color1) + (2 - c));
                                break;
                        case CC_LOCALSELECT_LFB:
                                /* cother = src_r/g/b, initialised to 0 each
                                   pixel by the C rasteriser at this point */
                                wemit_i32_const(e, 0);
                                break;
                        }
                }
                wemit_local_set(e, VL_SR + c);
        }
        /* src_a = ca_zero_other ? 0 : aother */
        if (need_srca) {
                if (ca_zero_other)
                        wemit_i32_const(e, 0);
                else
                        wemit_local_get(e, VL_AOTH);
                wemit_local_set(e, VL_SA);
        }
        if (sub_clocal) {
                for (c = 0; c < 3; c++) {
                        wemit_local_get(e, VL_SR + c);
                        wemit_local_get(e, VL_CLR + c);
                        wemit_op(e, WOP_I32_SUB);
                        wemit_local_set(e, VL_SR + c);
                }
        }
        if (need_srca && ca_sub_clocal) {
                wemit_local_get(e, VL_SA);
                wemit_local_get(e, VL_ALOC);
                wemit_op(e, WOP_I32_SUB);
                wemit_local_set(e, VL_SA);
        }
        /* msel per channel: src = (src * ((msel maybe ^0xff) + 1)) >> 8 */
        for (c = 0; c < 3; c++) {
                wemit_local_get(e, VL_SR + c);
                switch (mselect) {
                case CC_MSELECT_ZERO:
                        wemit_i32_const(e, 0);
                        break;
                case CC_MSELECT_CLOCAL:
                        wemit_local_get(e, VL_CLR + c);
                        break;
                case CC_MSELECT_AOTHER:
                        wemit_local_get(e, VL_AOTH);
                        break;
                case CC_MSELECT_ALOCAL:
                        wemit_local_get(e, VL_ALOC);
                        break;
                case CC_MSELECT_TEX:
                        wemit_local_get(e, VL_TA);
                        break;
                case CC_MSELECT_TEXRGB:
                        wemit_local_get(e, VL_TR + c);
                        break;
                }
                if (!reverse_blend) {
                        wemit_i32_const(e, 0xff);
                        wemit_op(e, WOP_I32_XOR);
                }
                wemit_i32_const(e, 1);
                wemit_op(e, WOP_I32_ADD);
                wemit_op(e, WOP_I32_MUL);
                wemit_i32_const(e, 8);
                wemit_op(e, WOP_I32_SHR_S);
                wemit_local_set(e, VL_SR + c);
        }
        if (need_srca) {
                wemit_local_get(e, VL_SA);
                switch (camselect) {
                case CCA_MSELECT_ZERO:
                        wemit_i32_const(e, 0);
                        break;
                case CCA_MSELECT_ALOCAL:
                case CCA_MSELECT_ALOCAL2:
                        wemit_local_get(e, VL_ALOC);
                        break;
                case CCA_MSELECT_AOTHER:
                        wemit_local_get(e, VL_AOTH);
                        break;
                case CCA_MSELECT_TEX:
                        wemit_local_get(e, VL_TA);
                        break;
                }
                if (!cca_rev) {
                        wemit_i32_const(e, 0xff);
                        wemit_op(e, WOP_I32_XOR);
                }
                wemit_i32_const(e, 1);
                wemit_op(e, WOP_I32_ADD);
                wemit_op(e, WOP_I32_MUL);
                wemit_i32_const(e, 8);
                wemit_op(e, WOP_I32_SHR_S);
                wemit_local_set(e, VL_SA);
        }
        /* cc_add / cca_add */
        if (add_sel == CC_ADD_CLOCAL) {
                for (c = 0; c < 3; c++) {
                        wemit_local_get(e, VL_SR + c);
                        wemit_local_get(e, VL_CLR + c);
                        wemit_op(e, WOP_I32_ADD);
                        wemit_local_set(e, VL_SR + c);
                }
        } else if (add_sel == CC_ADD_ALOCAL) {
                for (c = 0; c < 3; c++) {
                        wemit_local_get(e, VL_SR + c);
                        wemit_local_get(e, VL_ALOC);
                        wemit_op(e, WOP_I32_ADD);
                        wemit_local_set(e, VL_SR + c);
                }
        }
        if (need_srca && cca_add_sel) {
                wemit_local_get(e, VL_SA);
                wemit_local_get(e, VL_ALOC);
                wemit_op(e, WOP_I32_ADD);
                wemit_local_set(e, VL_SA);
        }
        /* clamp + invert */
        for (c = 0; c < 3; c++) {
                wemit_local_get(e, VL_SR + c);
                ve_clamp(e, 0xff, VL_TMP);
                if (invert_output) {
                        wemit_i32_const(e, 0xff);
                        wemit_op(e, WOP_I32_XOR);
                }
                wemit_local_set(e, VL_SR + c);
        }
        if (need_srca) {
                wemit_local_get(e, VL_SA);
                ve_clamp(e, 0xff, VL_TMP);
                if (cca_invert) {
                        wemit_i32_const(e, 0xff);
                        wemit_op(e, WOP_I32_XOR);
                }
                wemit_local_set(e, VL_SA);
        }

        /* ---- fog ------------------------------------------------------------ */
        if (fog_on) {
                if (fogMode & FOG_CONSTANT) {
                        static const uint32_t fog_c_off[3] = {2, 1, 0}; /* r,g,b of rgb_t{b,g,r} */
                        for (c = 0; c < 3; c++) {
                                wemit_local_get(e, VL_SR + c);
                                ve_push_params8(e, PO(fogColor) + fog_c_off[c]);
                                wemit_op(e, WOP_I32_ADD);
                                wemit_local_set(e, VL_SR + c);
                        }
                } else {
                        /* fog_a into VL_TMP2 */
                        switch (fogMode & (FOG_Z | FOG_ALPHA)) {
                        case 0:
                                /* fog table: idx = (w_depth>>10)&0x3f;
                                   fog_a = tbl[idx].fog + ((tbl[idx].dfog * ((w_depth>>2)&0xff)) >> 10) */
                                wemit_local_get(e, VL_PARAMS);
                                wemit_local_get(e, VL_WDEPTH);
                                wemit_i32_const(e, 10);
                                wemit_op(e, WOP_I32_SHR_U);
                                wemit_i32_const(e, 0x3f);
                                wemit_op(e, WOP_I32_AND);
                                wemit_i32_const(e, 1);
                                wemit_op(e, WOP_I32_SHL);
                                wemit_op(e, WOP_I32_ADD);
                                wemit_local_tee(e, VL_TMP); /* &fogTable[idx] - PO */
                                wemit_load(e, WOP_I32_LOAD8_U, PO(fogTable), 0);
                                wemit_local_get(e, VL_TMP);
                                wemit_load(e, WOP_I32_LOAD8_U, PO(fogTable) + 1, 0);
                                wemit_local_get(e, VL_WDEPTH);
                                wemit_i32_const(e, 2);
                                wemit_op(e, WOP_I32_SHR_U);
                                wemit_i32_const(e, 0xff);
                                wemit_op(e, WOP_I32_AND);
                                wemit_op(e, WOP_I32_MUL);
                                wemit_i32_const(e, 10);
                                wemit_op(e, WOP_I32_SHR_S);
                                wemit_op(e, WOP_I32_ADD);
                                wemit_local_set(e, VL_TMP2);
                                break;
                        case FOG_Z:
                                wemit_local_get(e, VL_Z);
                                wemit_i32_const(e, 20);
                                wemit_op(e, WOP_I32_SHR_S);
                                wemit_i32_const(e, 0xff);
                                wemit_op(e, WOP_I32_AND);
                                wemit_local_set(e, VL_TMP2);
                                break;
                        case FOG_ALPHA:
                                ve_push_iter_clamped(e, VL_IA);
                                wemit_local_set(e, VL_TMP2);
                                break;
                        case FOG_W:
                                wemit_local_get(e, V64_WI);
                                wemit_i64_const(e, 32);
                                wemit_op(e, WOP_I64_SHR_S);
                                wemit_op(e, WOP_I32_WRAP_I64);
                                wemit_i32_const(e, 0xff);
                                wemit_op(e, WOP_I32_AND);
                                ve_clamp(e, 0xff, VL_TMP);
                                wemit_local_set(e, VL_TMP2);
                                break;
                        }
                        wemit_local_get(e, VL_TMP2);
                        wemit_i32_const(e, 1);
                        wemit_op(e, WOP_I32_ADD);
                        wemit_local_set(e, VL_TMP2);
                        /* per channel: fog = FOG_ADD?0:fogColor; if !FOG_MULT fog -= src;
                           fog = (fog*fog_a)>>8; src = FOG_MULT ? fog : src+fog */
                        {
                                static const uint32_t fog_c_off2[3] = {2, 1, 0};
                                for (c = 0; c < 3; c++) {
                                        if (fogMode & FOG_ADD)
                                                wemit_i32_const(e, 0);
                                        else
                                                ve_push_params8(e, PO(fogColor) + fog_c_off2[c]);
                                        if (!(fogMode & FOG_MULT)) {
                                                wemit_local_get(e, VL_SR + c);
                                                wemit_op(e, WOP_I32_SUB);
                                        }
                                        wemit_local_get(e, VL_TMP2);
                                        wemit_op(e, WOP_I32_MUL);
                                        wemit_i32_const(e, 8);
                                        wemit_op(e, WOP_I32_SHR_S);
                                        if (!(fogMode & FOG_MULT)) {
                                                wemit_local_get(e, VL_SR + c);
                                                wemit_op(e, WOP_I32_ADD);
                                        }
                                        wemit_local_set(e, VL_SR + c);
                                }
                        }
                }
                for (c = 0; c < 3; c++) {
                        wemit_local_get(e, VL_SR + c);
                        ve_clamp(e, 0xff, VL_TMP);
                        wemit_local_set(e, VL_SR + c);
                }
        }

        /* ---- alpha test ------------------------------------------------------ */
        if (alpha_test) {
                if (afunc == AFUNC_NEVER) {
                        wemit_br(e, e->depth - skip_d);
                } else if (afunc != AFUNC_ALWAYS) {
                        wemit_local_get(e, VL_SA);
                        wemit_i32_const(e, aref_v);
                        switch (afunc) {
                        case AFUNC_LESSTHAN:
                                wemit_op(e, WOP_I32_LT_S);
                                break;
                        case AFUNC_EQUAL:
                                wemit_op(e, WOP_I32_EQ);
                                break;
                        case AFUNC_LESSTHANEQUAL:
                                wemit_op(e, WOP_I32_LE_S);
                                break;
                        case AFUNC_GREATERTHAN:
                                wemit_op(e, WOP_I32_GT_S);
                                break;
                        case AFUNC_NOTEQUAL:
                                wemit_op(e, WOP_I32_NE);
                                break;
                        case AFUNC_GREATERTHANEQUAL:
                                wemit_op(e, WOP_I32_GE_S);
                                break;
                        }
                        wemit_op(e, WOP_I32_EQZ);
                        wemit_br_if(e, e->depth - skip_d);
                }
        }

        /* ---- alpha blend ------------------------------------------------------ */
        if (alpha_blend) {
                /* newdest r/g/b into VL_D0/D1/D2 per dafunc_v (dest_a == 0xff) */
                for (c = 0; c < 3; c++) {
                        switch (dafunc_v) {
                        case AFUNC_AZERO:
                                wemit_i32_const(e, 0);
                                break;
                        case AFUNC_ASRC_ALPHA:
                                wemit_local_get(e, VL_DR + c);
                                wemit_local_get(e, VL_SA);
                                ve_mul_div255(e);
                                break;
                        case AFUNC_A_COLOR:
                                wemit_local_get(e, VL_DR + c);
                                wemit_local_get(e, VL_SR + c);
                                ve_mul_div255(e);
                                break;
                        case AFUNC_ADST_ALPHA: /* dest_a = 0xff */
                                wemit_local_get(e, VL_DR + c);
                                wemit_i32_const(e, 0xff);
                                ve_mul_div255(e);
                                break;
                        case AFUNC_AONE:
                                wemit_local_get(e, VL_DR + c);
                                break;
                        case AFUNC_AOMSRC_ALPHA:
                                wemit_local_get(e, VL_DR + c);
                                wemit_i32_const(e, 255);
                                wemit_local_get(e, VL_SA);
                                wemit_op(e, WOP_I32_SUB);
                                ve_mul_div255(e);
                                break;
                        case AFUNC_AOM_COLOR:
                                wemit_local_get(e, VL_DR + c);
                                wemit_i32_const(e, 255);
                                wemit_local_get(e, VL_SR + c);
                                wemit_op(e, WOP_I32_SUB);
                                ve_mul_div255(e);
                                break;
                        case AFUNC_AOMDST_ALPHA: /* 255 - 0xff = 0 */
                                wemit_local_get(e, VL_DR + c);
                                wemit_i32_const(e, 0);
                                ve_mul_div255(e);
                                break;
                        case AFUNC_ASATURATE: /* _a = MIN(src_a, 1-0xff) = -254 */
                                wemit_local_get(e, VL_DR + c);
                                wemit_i32_const(e, -254);
                                ve_mul_div255(e);
                                break;
                        default: /* 9..14: no case in C -> newdest = 0 */
                                wemit_i32_const(e, 0);
                                break;
                        }
                        wemit_local_set(e, VL_D0 + c);
                }
                /* src factor in place */
                for (c = 0; c < 3; c++) {
                        switch (safunc_v) {
                        case AFUNC_AZERO:
                                wemit_i32_const(e, 0);
                                wemit_local_set(e, VL_SR + c);
                                break;
                        case AFUNC_ASRC_ALPHA:
                                wemit_local_get(e, VL_SR + c);
                                wemit_local_get(e, VL_SA);
                                ve_mul_div255(e);
                                wemit_local_set(e, VL_SR + c);
                                break;
                        case AFUNC_A_COLOR:
                                wemit_local_get(e, VL_SR + c);
                                wemit_local_get(e, VL_DR + c);
                                ve_mul_div255(e);
                                wemit_local_set(e, VL_SR + c);
                                break;
                        case AFUNC_ADST_ALPHA:
                                wemit_local_get(e, VL_SR + c);
                                wemit_i32_const(e, 0xff);
                                ve_mul_div255(e);
                                wemit_local_set(e, VL_SR + c);
                                break;
                        case AFUNC_AONE:
                                break;
                        case AFUNC_AOMSRC_ALPHA:
                                wemit_local_get(e, VL_SR + c);
                                wemit_i32_const(e, 255);
                                wemit_local_get(e, VL_SA);
                                wemit_op(e, WOP_I32_SUB);
                                ve_mul_div255(e);
                                wemit_local_set(e, VL_SR + c);
                                break;
                        case AFUNC_AOM_COLOR:
                                wemit_local_get(e, VL_SR + c);
                                wemit_i32_const(e, 255);
                                wemit_local_get(e, VL_DR + c);
                                wemit_op(e, WOP_I32_SUB);
                                ve_mul_div255(e);
                                wemit_local_set(e, VL_SR + c);
                                break;
                        case AFUNC_AOMDST_ALPHA:
                                wemit_local_get(e, VL_SR + c);
                                wemit_i32_const(e, 0);
                                ve_mul_div255(e);
                                wemit_local_set(e, VL_SR + c);
                                break;
                        default: /* 8..14 unhandled in C src switch: unchanged */
                                break;
                        }
                }
                for (c = 0; c < 3; c++) {
                        wemit_local_get(e, VL_SR + c);
                        wemit_local_get(e, VL_D0 + c);
                        wemit_op(e, WOP_I32_ADD);
                        ve_clamp(e, 0xff, VL_TMP);
                        wemit_local_set(e, VL_SR + c);
                }
        }

        /* ---- dither / shift + writes ----------------------------------------- */
        if (rgb_wmask || depth_wmask) { /* the C 'update' block */
                if (dither_en) {
                        /* src = table[src][real_y & ym][x & xm] */
                        const uint8_t *rb = dither2x2_en ? &dither_rb2x2[0][0][0] : &dither_rb[0][0][0];
                        const uint8_t *g = dither2x2_en ? &dither_g2x2[0][0][0] : &dither_g[0][0][0];
                        int ymask = dither2x2_en ? 1 : 3;
                        int cellshift = dither2x2_en ? 2 : 4; /* entries per src value: 2*2 / 4*4 */
                        int rowshift = dither2x2_en ? 1 : 2;  /* entries per row: 2 / 4 */
                        for (c = 0; c < 3; c++) {
                                const uint8_t *tbl = (c == 1) ? g : rb;
                                wemit_i32_const(e, (int32_t)(uintptr_t)tbl);
                                wemit_local_get(e, VL_SR + c);
                                wemit_i32_const(e, cellshift);
                                wemit_op(e, WOP_I32_SHL);
                                wemit_op(e, WOP_I32_ADD);
                                wemit_local_get(e, VL_REALY);
                                wemit_i32_const(e, ymask);
                                wemit_op(e, WOP_I32_AND);
                                wemit_i32_const(e, rowshift);
                                wemit_op(e, WOP_I32_SHL);
                                wemit_op(e, WOP_I32_ADD);
                                wemit_local_get(e, VL_X);
                                wemit_i32_const(e, ymask);
                                wemit_op(e, WOP_I32_AND);
                                wemit_op(e, WOP_I32_ADD);
                                wemit_load(e, WOP_I32_LOAD8_U, 0, 0);
                                wemit_local_set(e, VL_SR + c);
                        }
                } else {
                        wemit_local_get(e, VL_SR);
                        wemit_i32_const(e, 3);
                        wemit_op(e, WOP_I32_SHR_S);
                        wemit_local_set(e, VL_SR);
                        wemit_local_get(e, VL_SG);
                        wemit_i32_const(e, 2);
                        wemit_op(e, WOP_I32_SHR_S);
                        wemit_local_set(e, VL_SG);
                        wemit_local_get(e, VL_SB);
                        wemit_i32_const(e, 3);
                        wemit_op(e, WOP_I32_SHR_S);
                        wemit_local_set(e, VL_SB);
                }
                if (rgb_wmask) {
                        wemit_local_get(e, VL_FBMEM);
                        wemit_local_get(e, col_tiled ? VL_XT : VL_X);
                        wemit_i32_const(e, 1);
                        wemit_op(e, WOP_I32_SHL);
                        wemit_op(e, WOP_I32_ADD);
                        wemit_local_get(e, VL_SB);
                        wemit_local_get(e, VL_SG);
                        wemit_i32_const(e, 5);
                        wemit_op(e, WOP_I32_SHL);
                        wemit_op(e, WOP_I32_OR);
                        wemit_local_get(e, VL_SR);
                        wemit_i32_const(e, 11);
                        wemit_op(e, WOP_I32_SHL);
                        wemit_op(e, WOP_I32_OR);
                        wemit_store(e, WOP_I32_STORE16, 0, 0);
                }
                if (depth_wmask && depth_enable) {
                        wemit_local_get(e, VL_AUXMEM);
                        wemit_local_get(e, aux_tiled ? VL_XT : VL_X);
                        wemit_i32_const(e, 1);
                        wemit_op(e, WOP_I32_SHL);
                        wemit_op(e, WOP_I32_ADD);
                        wemit_local_get(e, VL_NDEPTH);
                        wemit_store(e, WOP_I32_STORE16, 0, 0);
                }
        }

        wemit_end(e); /* end skip_pixel block */

        /* ---- per-pixel counters (native-recompiler parity) ------------------- */
        wemit_local_get(e, VL_PIXCNT);
        wemit_i32_const(e, 1);
        wemit_op(e, WOP_I32_ADD);
        wemit_local_set(e, VL_PIXCNT);
        if (texture) {
                wemit_local_get(e, VL_TEXCNT);
                wemit_i32_const(e, texels); /* 1 texel (single/passthrough), 2 (dual blend) */
                wemit_op(e, WOP_I32_ADD);
                wemit_local_set(e, VL_TEXCNT);
        }

        /* ---- iterate --------------------------------------------------------- */
        ve_iter(e, VL_IR, PO(dRdX), xdir_neg);
        ve_iter(e, VL_IG, PO(dGdX), xdir_neg);
        ve_iter(e, VL_IB, PO(dBdX), xdir_neg);
        ve_iter(e, VL_IA, PO(dAdX), xdir_neg);
        ve_iter(e, VL_Z, PO(dZdX), xdir_neg);
        if (tex_single || tex_dual) {
                ve_iter64(e, V64_S, PO(tmu[0].dSdX), xdir_neg);
                ve_iter64(e, V64_T, PO(tmu[0].dTdX), xdir_neg);
                ve_iter64(e, V64_W, PO(tmu[0].dWdX), xdir_neg);
        }
        if (tex_pass || tex_dual) {
                ve_iter64(e, V64_S1, PO(tmu[1].dSdX), xdir_neg);
                ve_iter64(e, V64_T1, PO(tmu[1].dTdX), xdir_neg);
                ve_iter64(e, V64_W1, PO(tmu[1].dWdX), xdir_neg);
        }
        if (need_w64)
                ve_iter64(e, V64_WI, PO(dWdX), xdir_neg);

        /* if (x == x2) exit; x += xdir; loop */
        wemit_local_get(e, VL_X);
        ve_push_state(e, SO(x2));
        wemit_op(e, WOP_I32_EQ);
        wemit_br_if(e, e->depth - loop_d + 1); /* br to end of outer block */
        wemit_local_get(e, VL_X);
        wemit_i32_const(e, xdir_neg ? -1 : 1);
        wemit_op(e, WOP_I32_ADD);
        wemit_local_set(e, VL_X);
        wemit_br(e, e->depth - loop_d); /* continue loop */

        wemit_end(e); /* end loop */
        wemit_end(e); /* end block */

        /* ---- write-back ------------------------------------------------------- */
        ve_st_state(e, SO(ir), VL_IR);
        ve_st_state(e, SO(ig), VL_IG);
        ve_st_state(e, SO(ib), VL_IB);
        ve_st_state(e, SO(ia), VL_IA);
        ve_st_state(e, SO(z), VL_Z);
        ve_st_state(e, SO(pixel_count), VL_PIXCNT);
        if (texture)
                ve_st_state(e, SO(texel_count), VL_TEXCNT);

        /* return 0 */
        wemit_i32_const(e, 0);

        if (e->failed || e->depth != 0)
                return -1;
        return wasm_emit_wrap_module(voodoo_wasm_buf, e->pos, WTYPE_IIII_I, VOODOO_N_I64, 0, VOODOO_N_I32, out, out_cap);
}

typedef uint8_t (*voodoo_draw_fn)(voodoo_state_t *state, voodoo_params_t *params, int x, int real_y);

static inline void *voodoo_get_block(voodoo_t *voodoo, voodoo_params_t *params, voodoo_state_t *state, int odd_even) {
        voodoo_wasm_block_t *blk;
        uint32_t h;
        int modlen;
        uint32_t idx;
        uint32_t trex18 = voodoo->trexInit1[0] & (1 << 18);
        uint32_t tl0 = params->tLOD[0] & LOD_MASK;
        uint32_t tl1 = params->tLOD[1] & LOD_MASK;
        int ct = voodoo->params.col_tiled ? 1 : 0;
        int at = voodoo->params.aux_tiled ? 1 : 0;

        if (!voodoo_wasm_supported(voodoo, params, state, odd_even))
                return NULL; /* caller falls back to the C rasteriser */

        h = params->fbzMode ^ (params->fbzColorPath * 31) ^ (params->alphaMode * 131) ^ (params->fogMode * 17) ^
            (params->textureMode[0] * 7) ^ (params->textureMode[1] * 3) ^ (tl0 >> 20) ^ (uint32_t)(state->xdir & 3) ^
            (uint32_t)(ct * 5 + at * 9);
        blk = &voodoo_wasm_cache[h % VOODOO_WASM_CACHE];

        if (blk->valid && blk->xdir == state->xdir && blk->alphaMode == params->alphaMode && blk->fbzMode == params->fbzMode &&
            blk->fogMode == params->fogMode && blk->fbzColorPath == params->fbzColorPath && blk->trex18 == trex18 &&
            blk->textureMode0 == params->textureMode[0] && blk->textureMode1 == params->textureMode[1] && blk->tLOD0 == tl0 &&
            blk->tLOD1 == tl1 && blk->col_tiled == ct && blk->aux_tiled == at)
                return (void *)(uintptr_t)blk->fn_idx;

        voodoo_recomp++;
        modlen = voodoo_wasm_emit(voodoo, params, state, voodoo_wasm_mod, sizeof(voodoo_wasm_mod));
        if (modlen < 0)
                return NULL;
        idx = wasm_jit_install(voodoo_wasm_mod, modlen);
        if (!idx)
                return NULL;

        if (blk->valid && blk->fn_idx)
                wasm_jit_free(blk->fn_idx);
        blk->valid = 1;
        blk->xdir = state->xdir;
        blk->alphaMode = params->alphaMode;
        blk->fbzMode = params->fbzMode;
        blk->fogMode = params->fogMode;
        blk->fbzColorPath = params->fbzColorPath;
        blk->trex18 = trex18;
        blk->textureMode0 = params->textureMode[0];
        blk->textureMode1 = params->textureMode[1];
        blk->tLOD0 = tl0;
        blk->tLOD1 = tl1;
        blk->col_tiled = ct;
        blk->aux_tiled = at;
        blk->fn_idx = idx;
        return (void *)(uintptr_t)idx;
}

/* Lifecycle: the wasm span cache is per-thread and lazily populated, so init
   only clears this thread's cache; blocks are freed as they are evicted. */
void voodoo_codegen_init(voodoo_t *voodoo) {
        int i;
        (void)voodoo;
        for (i = 0; i < VOODOO_WASM_CACHE; i++)
                voodoo_wasm_cache[i].valid = 0;
}
void voodoo_codegen_close(voodoo_t *voodoo) {
        int i;
        (void)voodoo;
        for (i = 0; i < VOODOO_WASM_CACHE; i++) {
                if (voodoo_wasm_cache[i].valid && voodoo_wasm_cache[i].fn_idx)
                        wasm_jit_free(voodoo_wasm_cache[i].fn_idx);
                voodoo_wasm_cache[i].valid = 0;
        }
}

/* A/B oracle entry point (wasm/jit/voodoo_ab.c drives this): compile a span for
   `params`/`state` and return its callable function pointer, or NULL if the
   state isn't covered. Exposed so the harness can invoke exactly the path the
   renderer would. */
void *voodoo_wasm_get_block_for_test(voodoo_t *voodoo, voodoo_params_t *params, voodoo_state_t *state, int odd_even) {
        return voodoo_get_block(voodoo, params, state, odd_even);
}

/* Gate probe for the oracle: answers "would this state compile?" without
   rendering. Needed for the register configs the C rasteriser fatal()s on —
   those can't go through an A/B render, but the gate rejecting them still
   needs proving. */
int voodoo_wasm_gate_probe(voodoo_t *voodoo, voodoo_params_t *params, int xdir) {
        voodoo_state_t st;
        memset(&st, 0, sizeof(st));
        st.xdir = xdir;
        return voodoo_wasm_supported(voodoo, params, &st, 0);
}

#undef SO
#undef PO
#endif /* _VID_VOODOO_CODEGEN_WASM_H_ */
