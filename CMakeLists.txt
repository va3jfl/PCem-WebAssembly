# ============================================================================
#  PCem-web — Emscripten build of the PCem emulator core
#
#  Builds the headless PCem core (interpreter CPU paths, no native UI, no
#  host-device passthrough) plus the wasm platform layer and embind bridge
#  into web/pcem/pcem.{js,wasm}.
#
#  Usage:   emcmake cmake -B build -DCMAKE_BUILD_TYPE=Release
#           cmake --build build -j$(nproc)
#  (or just ./build.sh)
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(pcem-web C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "This build must be configured through emcmake (Emscripten toolchain).")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PCEM_CORE ${CMAKE_SOURCE_DIR}/core)
set(PCEM_SRC_DIR ${PCEM_CORE}/src)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ----------------------------------------------------------------------------
# Compile flags
# ----------------------------------------------------------------------------
# -O3            : hot interpreter loops want it
# -msimd128/-msse: lets the SSE-flavoured resid-fp filter code map onto wasm SIMD
# -fno-strict-aliasing / -fwrapv : emulator code plays fast and loose; match
#                  the assumptions upstream builds with (-fno-strict-aliasing
#                  is in PCem's own release flags)
# -pthread       : PCem's video blit / CD audio / Voodoo threads become Web
#                  Workers (requires cross-origin isolation at serve time)
set(PCEM_C_FLAGS "-O3 -pthread -msimd128 -msse -fno-strict-aliasing -fwrapv -fomit-frame-pointer -fno-common")
# NOTE: implicit function declarations stay ERRORS on purpose — in wasm a
# signature mismatch is a runtime trap, not a warning-grade nuisance.
set(PCEM_WARN "-Wno-incompatible-pointer-types -Wno-parentheses -Wno-unused-value -Wno-format")

set(CMAKE_C_FLAGS_RELEASE "${PCEM_C_FLAGS} ${PCEM_WARN}")
set(CMAKE_CXX_FLAGS_RELEASE "${PCEM_C_FLAGS} -Wno-parentheses -Wno-format")

add_compile_definitions(
    PCEM_VERSION_STRING="web"
    NO_CODEGEN            # Voodoo rasteriser: force the portable C paths
    PCEM_WASM
    # Networking: compile core/src/networking (NE2000 + RTL8029AS) unmodified.
    # ne2000.c's <slirp/libslirp.h> resolves to wasm/shim/slirp/libslirp.h and
    # the "slirp" it talks to is wasm-net.c — a WebSocket ethernet relay
    # backend (v86/websockproxy protocol). USE_PCAP_NETWORKING stays undefined
    # (no pcap in a browser), which also compiles out every pcap reference.
    USE_NETWORKING
)

# ----------------------------------------------------------------------------
# Include paths (mirror includes/includes.cmake, minus the native UI dirs,
# plus our shims which must come first)
# ----------------------------------------------------------------------------
include_directories(
    ${CMAKE_SOURCE_DIR}/wasm/shim
    ${PCEM_CORE}/includes/private
    ${PCEM_CORE}/includes/private/bus
    ${PCEM_CORE}/includes/private/cdrom
    ${PCEM_CORE}/includes/private/codegen
    ${PCEM_CORE}/includes/private/cpu
    ${PCEM_CORE}/includes/private/devices
    ${PCEM_CORE}/includes/private/disc
    ${PCEM_CORE}/includes/private/dosbox
    ${PCEM_CORE}/includes/private/flash
    ${PCEM_CORE}/includes/private/floppy
    ${PCEM_CORE}/includes/private/hdd
    ${PCEM_CORE}/includes/private/ide
    ${PCEM_CORE}/includes/private/joystick
    ${PCEM_CORE}/includes/private/keyboard
    ${PCEM_CORE}/includes/private/lpt
    ${PCEM_CORE}/includes/private/memory
    ${PCEM_CORE}/includes/private/mfm
    ${PCEM_CORE}/includes/private/models
    ${PCEM_CORE}/includes/private/mouse
    ${PCEM_CORE}/includes/private/networking
    ${PCEM_CORE}/includes/private/plugin-api
    ${PCEM_CORE}/includes/private/scsi
    ${PCEM_CORE}/includes/private/sound
    ${PCEM_CORE}/includes/private/video
    ${PCEM_CORE}/includes/private/wx-ui     # viewer.h / ui-utils.h referenced from core
    ${PCEM_CORE}/includes/public
)

# ----------------------------------------------------------------------------
# Core sources
# ----------------------------------------------------------------------------
file(GLOB SRC_ROOT      ${PCEM_SRC_DIR}/*.c)                 # pc.c io.c timer.c ... + thread-pthread.c
file(GLOB SRC_BUS       ${PCEM_SRC_DIR}/bus/*.c)
file(GLOB SRC_CPU       ${PCEM_SRC_DIR}/cpu/*.c)
# CPU recompiler: the shared IR/uop layer (top-level codegen/*.c only — the
# native x86/x86-64/arm/arm64 backends live in subdirectories and stay
# excluded) plus the wasm backend in wasm/jit/.
file(GLOB SRC_CODEGEN   ${PCEM_SRC_DIR}/codegen/*.c)
list(FILTER SRC_CODEGEN EXCLUDE REGEX ".*/codegen_x86-64\\.c$")   # x86-64-only helper
file(GLOB SRC_DEVICES   ${PCEM_SRC_DIR}/devices/*.c)
file(GLOB SRC_DISC      ${PCEM_SRC_DIR}/disc/*.c)
file(GLOB SRC_DOSBOX    ${PCEM_SRC_DIR}/dosbox/*.c ${PCEM_SRC_DIR}/dosbox/*.cc ${PCEM_SRC_DIR}/dosbox/*.cpp)
file(GLOB SRC_FLASH     ${PCEM_SRC_DIR}/flash/*.c)
file(GLOB SRC_FLOPPY    ${PCEM_SRC_DIR}/floppy/*.c)
file(GLOB SRC_HDD       ${PCEM_SRC_DIR}/hdd/*.c
                        ${PCEM_SRC_DIR}/hdd/ramdisk/*.c
                        ${PCEM_SRC_DIR}/hdd/minivhd/*.c)
file(GLOB SRC_IDE       ${PCEM_SRC_DIR}/ide/*.c)
file(GLOB SRC_JOYSTICK  ${PCEM_SRC_DIR}/joystick/*.c)
file(GLOB SRC_KEYBOARD  ${PCEM_SRC_DIR}/keyboard/*.c)
file(GLOB SRC_LPT       ${PCEM_SRC_DIR}/lpt/*.c)
file(GLOB SRC_MEMORY    ${PCEM_SRC_DIR}/memory/*.c)
file(GLOB SRC_MFM       ${PCEM_SRC_DIR}/mfm/*.c)
file(GLOB SRC_MODELS    ${PCEM_SRC_DIR}/models/*.c)
file(GLOB SRC_MOUSE     ${PCEM_SRC_DIR}/mouse/*.c)
# networking: exactly the three sources core's networking.cmake lists (the
# src/networking/slirp/ subdir is legacy bundled-slirp, not part of the build)
set(SRC_NETWORKING
    ${PCEM_SRC_DIR}/networking/ne2000.c
    ${PCEM_SRC_DIR}/networking/nethandler.c
    ${PCEM_SRC_DIR}/networking/queue.c
)
file(GLOB SRC_SCSI      ${PCEM_SRC_DIR}/scsi/*.c)
file(GLOB SRC_SOUND     ${PCEM_SRC_DIR}/sound/*.c ${PCEM_SRC_DIR}/sound/*.cc
                        ${PCEM_SRC_DIR}/sound/ayumi/*.c
                        ${PCEM_SRC_DIR}/sound/resid-fp/*.cc)
file(GLOB SRC_VIDEO     ${PCEM_SRC_DIR}/video/*.c)
file(GLOB SRC_PLUGINAPI ${PCEM_SRC_DIR}/plugin-api/*.c)
set(SRC_CDROM
    ${PCEM_SRC_DIR}/cdrom/cdrom-image.cc
    ${PCEM_SRC_DIR}/cdrom/cdrom-null.c
    ${PCEM_SRC_DIR}/cdrom/cdrom-ioctl-dummy.c
)

set(PCEM_ALL_SRC
    ${SRC_ROOT} ${SRC_BUS} ${SRC_CDROM} ${SRC_CPU} ${SRC_CODEGEN} ${SRC_DEVICES}
    ${SRC_DISC} ${SRC_DOSBOX} ${SRC_FLASH} ${SRC_FLOPPY} ${SRC_HDD} ${SRC_IDE}
    ${SRC_JOYSTICK} ${SRC_KEYBOARD} ${SRC_LPT} ${SRC_MEMORY} ${SRC_MFM}
    ${SRC_MODELS} ${SRC_MOUSE} ${SRC_NETWORKING} ${SRC_SCSI} ${SRC_SOUND}
    ${SRC_VIDEO} ${SRC_PLUGINAPI}
)

# Host-specific / replaced-by-wasm-layer sources to drop from the globs.
set(PCEM_EXCLUDE
    sound/midi_alsa.c        # host MIDI backends
    sound/sdl2-midi.c
    sound/win-midi.c
    sound/soundopenal.c      # replaced by wasm-audio.c ring buffers
)

foreach(pat ${PCEM_EXCLUDE})
    list(FILTER PCEM_ALL_SRC EXCLUDE REGEX ".*/${pat}$")
endforeach()

# ----------------------------------------------------------------------------
# WASM platform layer + bridge
# ----------------------------------------------------------------------------
set(WASM_SRC
    wasm/platform/wasm-main.c
    wasm/platform/wasm-video.c
    wasm/platform/wasm-audio.c
    wasm/platform/wasm-input.c
    wasm/platform/wasm-utils.c
    wasm/platform/wasm-stubs.c
    wasm/platform/wasm-diskio.c
    wasm/platform/wasm-net.c
    wasm/jit/wasm-emit.c
    wasm/jit/wasm-jit-selftest.c
    wasm/jit/codegen_backend_wasm.c
    wasm/jit/codegen_backend_wasm_uops.c
    wasm/jit/wasm-jit-oracle.c
    wasm/jit/wasm-jit-bench.c
    wasm/jit/voodoo_ab.c
    wasm/wasm-bridge.cpp
)
include_directories(${CMAKE_SOURCE_DIR}/wasm/jit)

# resid-fp's version.cc expects an autotools-style VERSION define.
set_source_files_properties(${PCEM_SRC_DIR}/sound/resid-fp/version.cc
    PROPERTIES COMPILE_DEFINITIONS VERSION="pcem-web")

add_executable(pcem ${PCEM_ALL_SRC} ${WASM_SRC})
set_target_properties(pcem PROPERTIES OUTPUT_NAME "pcem" SUFFIX ".js")

# ----------------------------------------------------------------------------
# Link flags
# ----------------------------------------------------------------------------
# Notes on the choices:
#  * MODULARIZE + EXPORT_NAME    : the page instantiates via createPCem({...})
#  * -pthread + POOL             : workers are pre-spawned so thread_create()
#                                  from initvideo()/sound_init() starts
#                                  immediately without yielding to the loop
#  * ALLOW_MEMORY_GROWTH         : machine RAM + media images are dynamic
#  * -O1 at link                 : wasm-opt -O3 runs as a separate post-link
#                                  step in build.sh (keeps link JS-processing
#                                  independent of npm-provided optimizers)
option(PCEM_DEBUG "Link with assertions + DWARF names for debugging" OFF)
if(PCEM_DEBUG)
    set(PCEM_LINK_OPT "-O0" "-g2" "SHELL:-s ASSERTIONS=2" "SHELL:-s STACK_OVERFLOW_CHECK=2" "SHELL:-s DEMANGLE_SUPPORT=1")
else()
    set(PCEM_LINK_OPT "-O1" "SHELL:-s ASSERTIONS=0")
endif()

target_link_options(pcem PRIVATE
    ${PCEM_LINK_OPT}
    "-pthread"
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_NAME=createPCem"
    "SHELL:-s ENVIRONMENT=web,worker"
    "SHELL:-s PTHREAD_POOL_SIZE=12"
    "SHELL:-s INITIAL_MEMORY=402653184"
    "SHELL:-s MAXIMUM_MEMORY=2147483648"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s STACK_SIZE=4194304"
    "SHELL:-s DEFAULT_PTHREAD_STACK_SIZE=2097152"
    "SHELL:-s WASM_BIGINT=1"
    # runtime JITs (CPU + Voodoo) build wasm modules that import this module's
    # shared memory and function table; the table must be growable so newly
    # compiled span/block functions can be installed as callable pointers.
    "SHELL:-s ALLOW_TABLE_GROWTH=1"
    "SHELL:-s EXIT_RUNTIME=0"
    "SHELL:-s INVOKE_RUN=1"
    "SHELL:-s FORCE_FILESYSTEM=1"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=FS,ccall,cwrap,HEAPU8,UTF8ToString,stringToNewUTF8"
    "-lembind"
)

# Ship straight into the web root.
add_custom_command(TARGET pcem POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/web/pcem
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE_DIR:pcem>/pcem.js
            $<TARGET_FILE_DIR:pcem>/pcem.wasm
            $<TARGET_FILE_DIR:pcem>/pcem.worker.js
            ${CMAKE_SOURCE_DIR}/web/pcem/
    COMMENT "Staging pcem.js / pcem.wasm into web/pcem/"
)
