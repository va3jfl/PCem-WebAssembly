#!/usr/bin/env bash
# ============================================================================
#  PCem-web build pipeline
#
#  Compiles the PCem core + wasm platform layer with Emscripten and stages
#  pcem.{js,wasm,worker.js} into web/pcem/, then runs a wasm-opt -O3 pass
#  over the module (the CMake link step intentionally links at -O1; heavy
#  optimisation happens here where it is toolchain-independent).
#
#  Requirements: an activated Emscripten environment (emcc on PATH), or set
#  EMSDK/EMSCRIPTEN_ROOT so this script can find it.
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")"

JOBS=${JOBS:-$(nproc)}
BUILD_DIR=${BUILD_DIR:-build}
BUILD_TYPE=${BUILD_TYPE:-Release}

# ---- locate emscripten ------------------------------------------------------
if ! command -v emcc >/dev/null 2>&1; then
    for candidate in "${EMSCRIPTEN_ROOT:-}" "${EMSDK:-}/upstream/emscripten" "$HOME/emscripten" "$HOME/emsdk/upstream/emscripten"; do
        if [ -n "$candidate" ] && [ -x "$candidate/emcc" ]; then
            export PATH="$candidate:$PATH"
            break
        fi
    done
fi
command -v emcc >/dev/null 2>&1 || { echo "error: emcc not found — activate emsdk or set EMSCRIPTEN_ROOT" >&2; exit 1; }
echo "==> emcc: $(command -v emcc) ($(emcc --version | head -1))"

# ---- configure + build ------------------------------------------------------
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    emcmake cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi
cmake --build "$BUILD_DIR" -j "$JOBS"

# ---- post-link optimisation ------------------------------------------------
WASM_OPT=${WASM_OPT:-$(command -v wasm-opt || true)}
if [ -z "$WASM_OPT" ]; then
    # fall back to the binaryen emscripten itself is configured with
    BINROOT=$(em-config BINARYEN_ROOT 2>/dev/null || true)
    [ -n "$BINROOT" ] && [ -x "$BINROOT/bin/wasm-opt" ] && WASM_OPT="$BINROOT/bin/wasm-opt"
fi
if [ -n "$WASM_OPT" ] && [ "${SKIP_WASM_OPT:-0}" != "1" ]; then
    echo "==> wasm-opt -O3 pass"
    "$WASM_OPT" -O3 --enable-threads --enable-bulk-memory --enable-sign-ext \
        --enable-mutable-globals --enable-nontrapping-float-to-int --enable-simd \
        web/pcem/pcem.wasm -o web/pcem/pcem.wasm.opt
    mv web/pcem/pcem.wasm.opt web/pcem/pcem.wasm
else
    echo "==> skipping wasm-opt pass (not found or SKIP_WASM_OPT=1)"
fi

echo
echo "==> build complete:"
ls -la web/pcem/
echo
echo "Serve it with:   python3 serve.py    (then open http://localhost:8000)"
