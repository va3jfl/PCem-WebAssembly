/*
 * Minimal SDL shim for the PCem-web build.
 *
 * The only core (non-UI) file that touches SDL is plugin-api/paths.c, which
 * calls SDL_GetBasePath() to locate the executable directory.  In the
 * browser everything lives under the virtual /pcem tree, so we satisfy the
 * include with this header instead of pulling in (and porting against) a
 * real SDL, keeping the upstream source untouched.
 */
#ifndef _PCEM_WASM_SDL_SHIM_H_
#define _PCEM_WASM_SDL_SHIM_H_

static inline char *SDL_GetBasePath(void) { return "/pcem/"; }
static inline void SDL_free(void *p) { (void)p; }

#endif
