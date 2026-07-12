#!/usr/bin/env bash
# ============================================================================
#  setup-toolchain.sh — reproduce the pcem-web build toolchain from scratch.
#
#  Written for environments where the usual binary channels are blocked
#  (emsdk's storage.googleapis.com, npm registry, apt archive fetches,
#  GitHub release assets) but plain `git clone` of github.com works —
#  e.g. the sandbox this port was built in. Assembles:
#
#     * emscripten 3.1.52 frontend        (git, pure python/js)
#     * system LLVM 18 (clang-18/wasm-ld) (must already be installed)
#     * binaryen version_115              (git + local cmake/ninja build)
#     * acorn for emscripten's JS passes  (symlinked from a local install
#       if the npm registry is blocked)
#     * playwright link for tools/*.mjs   (from a global install)
#
#  Usage:  ./tools/setup-toolchain.sh [prefix]     (default prefix: $HOME)
#  Then:   export PATH="$PREFIX/emscripten:$PATH" && ./build.sh
# ============================================================================
set -euo pipefail

PREFIX="${1:-$HOME}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

echo "==> prefix: $PREFIX"

# ---- 0. system LLVM 18 -------------------------------------------------------
if ! command -v clang-18 >/dev/null 2>&1 || ! command -v wasm-ld-18 >/dev/null 2>&1; then
    echo "error: clang-18 / wasm-ld-18 not found."
    echo "       Install LLVM 18 (apt: clang-18 lld-18 llvm-18) and re-run."
    exit 1
fi
LLVM_BIN="$(dirname "$(readlink -f "$(command -v clang-18)")")"
echo "==> LLVM 18: $LLVM_BIN"

# ---- 1. emscripten 3.1.52 frontend ------------------------------------------
if [ ! -d "$PREFIX/emscripten" ]; then
    echo "==> cloning emscripten 3.1.52…"
    git clone --depth 1 --branch 3.1.52 https://github.com/emscripten-core/emscripten.git "$PREFIX/emscripten"
fi
cd "$PREFIX/emscripten"
python3 tools/maint/create_entry_points.py
mkdir -p out node_modules
touch out/npm_packages.stamp out/create_entry_points.stamp out/git_submodules.stamp

# acorn: emscripten's JS transform passes need it. npm if it works, else
# symlink any locally installed copy (node-tools ships one in the sandbox).
if [ ! -e node_modules/acorn ]; then
    if npm install --no-audit --no-fund acorn >/dev/null 2>&1; then
        echo "==> acorn via npm"
    else
        ACORN="$(find /opt /usr/lib/node_modules "$HOME/.npm-global" -maxdepth 4 -type d -name acorn 2>/dev/null | head -1 || true)"
        if [ -n "$ACORN" ]; then
            ln -sfn "$ACORN" node_modules/acorn
            [ -d "$(dirname "$ACORN")/acorn-walk" ] && ln -sfn "$(dirname "$ACORN")/acorn-walk" node_modules/acorn-walk
            echo "==> acorn symlinked from $ACORN"
        else
            echo "error: npm blocked and no local acorn found" ; exit 1
        fi
    fi
fi

# ---- 2. binaryen version_115 (built from source) ------------------------------
if [ ! -x "$PREFIX/binaryen/build/bin/wasm-opt" ]; then
    echo "==> cloning + building binaryen version_115 (this is the slow part)…"
    [ -d "$PREFIX/binaryen" ] || git clone --depth 1 --branch version_115 https://github.com/WebAssembly/binaryen.git "$PREFIX/binaryen"
    cmake -S "$PREFIX/binaryen" -B "$PREFIX/binaryen/build" -G Ninja \
          -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DINSTALL_LIBS=OFF -DENABLE_WERROR=OFF
    ninja -C "$PREFIX/binaryen/build" wasm-opt wasm-metadce wasm-emscripten-finalize
fi
"$PREFIX/binaryen/build/bin/wasm-opt" --version

# ---- 3. emscripten config ------------------------------------------------------
cat > "$PREFIX/emscripten/.emscripten" <<EOF
LLVM_ROOT = '$LLVM_BIN'
BINARYEN_ROOT = '$PREFIX/binaryen/build'
NODE_JS = '$(command -v node)'
CACHE = '$PREFIX/emcache'
FROZEN_CACHE = False
EOF
echo "==> emscripten config written"

# ---- 4. playwright for tools/*.mjs -------------------------------------------
cd "$HERE"
mkdir -p node_modules
if [ ! -e node_modules/playwright ]; then
    PW="$(find "$HOME/.npm-global/lib/node_modules" /usr/lib/node_modules /opt -maxdepth 4 -type d -name playwright 2>/dev/null | head -1 || true)"
    if [ -n "$PW" ]; then
        ln -sfn "$PW" node_modules/playwright
        echo "==> playwright symlinked from $PW"
    else
        npm install --no-audit --no-fund playwright || echo "warn: playwright unavailable — tools/*.mjs tests won't run"
    fi
fi

# ---- 5. smoke ------------------------------------------------------------------
export PATH="$PREFIX/emscripten:$PATH"
emcc --version | head -1
echo
echo "Toolchain ready. Build with:"
echo "    export PATH=\"$PREFIX/emscripten:\$PATH\" && ./build.sh"
echo "Regenerate test ROMs if web/roms is empty:  python3 tools/make_test_rom.py"
