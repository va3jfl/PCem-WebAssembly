#!/usr/bin/env python3
"""
make_test_rom.py — generate open test BIOSes for PCem-web.

No proprietary code: each ROM is ~100-130 bytes of hand-assembled machine
code (annotated below) that programs the CGA 6845 for 320x200 4-colour
graphics mode and draws a test card of solid vertical colour bars
(white / cyan / magenta / black), then halts. They exist so the whole stack —
/roms fetch over HTTP -> Emscripten FS -> loadbios() -> CPU interpreter ->
CGA device -> framebuffer -> canvas — can be exercised and CI-tested without
any copyrighted BIOS image. They are NOT BIOSes: they can't boot an OS. Use
your own ROM sets for that.

Two ROMs are produced:

  web/roms/genxt/pcxt.rom          "[8088] Generic XT clone" (8 KB at F000:E000)
        8088 test card — the original smoke-test ROM.

  web/roms/p55tvp4/tv5i0204.awd    "[Socket 7] ASUS P/I-P55TVP4" (128 KB at E0000)
        Same test card, prefixed with a CPUID + RDTSC exercise: it verifies
        the interpreter executes Pentium-class instructions (this machine's
        CPUs are all "requires dynarec" parts in native PCem) and dead-stops
        without drawing if the TSC doesn't advance. Bars on screen ==
        Pentium/K6/WinChip/6x86 interpreter execution works.

Usage: python3 tools/make_test_rom.py [webroot-roms-dir]   (default web/roms)
"""
import os
import sys


def cga_testcard(code, code_seg_off, pentium_prologue=False):
    """Append the test-card program to `code`. `code_seg_off` is the offset of
    code[0] within its 64 KB segment (tables are addressed DS:SI relative to
    the same segment the code runs in)."""

    def emit(*b):
        code.extend(b)

    # --- entry ---------------------------------------------------------------
    emit(0xFA)                    # cli
    emit(0x31, 0xC0)              # xor ax, ax
    emit(0x8E, 0xD0)              # mov ss, ax
    emit(0xBC, 0x00, 0x70)        # mov sp, 0x7000
    emit(0x8C, 0xC8)              # mov ax, cs
    emit(0x8E, 0xD8)              # mov ds, ax

    if pentium_prologue:
        # Prove Pentium-class decode/execute on the interpreter before any
        # video output: CPUID leaf 0, then RDTSC in a retry loop until the
        # TSC visibly advances. (A single back-to-back delta can be 0 —
        # PCem models Pentium U/V-pipe pairing, so cheap instructions may
        # cost 0 cycles.) A genuinely frozen TSC hangs here and the test
        # card never appears -> test fails.
        emit(0x66, 0x31, 0xC0)    # xor eax, eax
        emit(0x0F, 0xA2)          # cpuid          (vendor string in ebx:edx:ecx)
        emit(0x0F, 0x31)          # rdtsc          (edx:eax = TSC)
        emit(0x66, 0x89, 0xC3)    # mov ebx, eax
        # tsc_retry:
        emit(0x0F, 0x31)          # rdtsc
        emit(0x66, 0x29, 0xD8)    # sub eax, ebx   (delta since first read)
        emit(0x74, 0xF9)          # jz tsc_retry   (-7: loop until TSC advances)

    # program the 6845 CRTC with standard CGA 320x200 graphics values
    crtc_table_placeholder = len(code) + 1   # patched below
    emit(0xBE, 0x00, 0x00)        # mov si, imm16 (crtc table, patched)
    emit(0xBA, 0xD4, 0x03)        # mov dx, 0x3D4
    emit(0xB9, 0x0C, 0x00)        # mov cx, 12
    emit(0xB3, 0x00)              # mov bl, 0
    # crtc_loop:
    emit(0x88, 0xD8)              # mov al, bl
    emit(0xEE)                    # out dx, al        (3D4: reg index)
    emit(0x42)                    # inc dx
    emit(0xAC)                    # lodsb
    emit(0xEE)                    # out dx, al        (3D5: reg value)
    emit(0x4A)                    # dec dx
    emit(0xFE, 0xC3)              # inc bl
    emit(0xE2, 0xF5)              # loop crtc_loop    (-11)

    # mode control: 320x200 graphics | video enable
    emit(0xBA, 0xD8, 0x03)        # mov dx, 0x3D8
    emit(0xB0, 0x0A)              # mov al, 0x0A
    emit(0xEE)                    # out dx, al
    # colour select: intensity + palette 1 (cyan/magenta/white on black)
    emit(0xBA, 0xD9, 0x03)        # mov dx, 0x3D9
    emit(0xB0, 0x30)              # mov al, 0x30
    emit(0xEE)                    # out dx, al

    # draw solid vertical colour bars: white / cyan / magenta / black.
    # CGA graphics VRAM is two 8 KB banks (even/odd scanlines) of 100 rows x
    # 80 bytes; each byte is 4 pixels (2bpp), so a solid-colour byte is
    # colour*0x55. Fill each bank row-wise: 4 bars x 20 bytes per row keeps
    # the bars perfectly vertical in both banks.
    emit(0xB8, 0x00, 0xB8)        # mov ax, 0xB800
    emit(0x8E, 0xC0)              # mov es, ax
    emit(0x31, 0xFF)              # xor di, di
    emit(0xBD, 0x02, 0x00)        # mov bp, 2         (two banks)
    # bank_loop:
    emit(0xB6, 0x64)              # mov dh, 100       (rows per bank)
    # row_loop:
    bar_table_placeholder = len(code) + 1    # patched below
    emit(0xBE, 0x00, 0x00)        # mov si, imm16 (bar colour table, patched)
    emit(0xB2, 0x04)              # mov dl, 4         (bars per row)
    # bar_loop:
    emit(0xAC)                    # lodsb             (al = bar colour byte)
    emit(0xB9, 0x14, 0x00)        # mov cx, 20        (bytes per bar)
    emit(0xF3, 0xAA)              # rep stosb
    emit(0xFE, 0xCA)              # dec dl
    emit(0x75, 0xF6)              # jnz bar_loop      (-10)
    emit(0xFE, 0xCE)              # dec dh
    emit(0x75, 0xED)              # jnz row_loop      (-19)
    emit(0x81, 0xC7, 0xC0, 0x00)  # add di, 192       (skip bank padding: 8000 -> 8192)
    emit(0x4D)                    # dec bp
    emit(0x75, 0xE4)              # jnz bank_loop     (-28)

    # --- MPU-401 UART exercise (tools/midi_test.mjs) ---------------------------
    # Switch the MPU-401 (0x330/0x331 — SB16's UART) into UART mode, then send
    # note-on C4 and, via MIDI running status, its note-off. Bare OUTs to an
    # unclaimed port are no-ops on machines without an MPU-401, so this rides
    # along in every test ROM after the bars are on screen.
    emit(0xBA, 0x31, 0x03)        # mov dx, 0x331     (MPU-401 command port)
    emit(0xB0, 0x3F)              # mov al, 0x3F      (enter UART mode)
    emit(0xEE)                    # out dx, al
    emit(0xBA, 0x30, 0x03)        # mov dx, 0x330     (MPU-401 data port)
    for midi_byte in (0x90, 0x3C, 0x7F,   # note on  ch1 C4 vel 127
                      0x3C, 0x00):        # running status -> note off C4
        emit(0xB0, midi_byte)     # mov al, byte
        emit(0xEE)                # out dx, al

    # halt forever
    emit(0xF4)                    # hlt
    emit(0xEB, 0xFD)              # jmp $-1 (back to hlt)

    # --- CRTC table ----------------------------------------------------------
    crtc_off = len(code)
    code.extend([0x38, 0x28, 0x2D, 0x0A, 0x7F, 0x06, 0x64, 0x70,
                 0x02, 0x01, 0x06, 0x07])
    si = code_seg_off + crtc_off
    code[crtc_table_placeholder] = si & 0xFF
    code[crtc_table_placeholder + 1] = (si >> 8) & 0xFF

    # --- bar colour table (2bpp solid-fill bytes: white, cyan, magenta, black)
    bar_off = len(code)
    code.extend([0xFF, 0x55, 0xAA, 0x00])
    bar_addr = code_seg_off + bar_off
    code[bar_table_placeholder] = bar_addr & 0xFF
    code[bar_table_placeholder + 1] = (bar_addr >> 8) & 0xFF


def build_genxt():
    """8 KB ROM at F000:E000 for '[8088] Generic XT clone'."""
    ROM_SIZE = 8192
    CODE_SEG, CODE_OFF = 0xF000, 0xE000  # file offset 0 maps to F000:E000
    RESET_FILE_OFF = 0x1FF0              # FFFF:0000 == file offset 0x1FF0

    rom = bytearray([0xFF] * ROM_SIZE)
    code = bytearray()
    cga_testcard(code, CODE_OFF, pentium_prologue=False)

    assert len(code) < RESET_FILE_OFF, "code overlaps reset vector"
    rom[0:len(code)] = code

    rom[RESET_FILE_OFF:RESET_FILE_OFF + 5] = bytes([
        0xEA, CODE_OFF & 0xFF, CODE_OFF >> 8, CODE_SEG & 0xFF, CODE_SEG >> 8])
    rom[RESET_FILE_OFF + 5:RESET_FILE_OFF + 13] = b"PCEMWEB\0"
    return bytes(rom)


def build_p55tvp4():
    """128 KB flash image for '[Socket 7] ASUS P/I-P55TVP4'.
    loadbios() reads the whole file to rom[0..0x1ffff], mapped at E0000
    (biosmask 0x1ffff): file offset 0x10000 == F000:0000, reset vector
    F000:FFF0 == file offset 0x1FFF0."""
    ROM_SIZE = 0x20000
    CODE_SEG, CODE_OFF = 0xF000, 0x0000
    CODE_FILE_OFF = 0x10000
    RESET_FILE_OFF = 0x1FFF0

    rom = bytearray([0xFF] * ROM_SIZE)
    code = bytearray()
    cga_testcard(code, CODE_OFF, pentium_prologue=True)

    assert CODE_FILE_OFF + len(code) < RESET_FILE_OFF, "code overlaps reset vector"
    rom[CODE_FILE_OFF:CODE_FILE_OFF + len(code)] = code

    rom[RESET_FILE_OFF:RESET_FILE_OFF + 5] = bytes([
        0xEA, CODE_OFF & 0xFF, CODE_OFF >> 8, CODE_SEG & 0xFF, CODE_SEG >> 8])
    rom[RESET_FILE_OFF + 5:RESET_FILE_OFF + 13] = b"PCEMWEB\0"
    return bytes(rom)


def main():
    romsdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "web", "roms")

    targets = [
        ("genxt", "pcxt.rom", build_genxt(),
         "'[8088] Generic XT clone' test ROM"),
        ("p55tvp4", "tv5i0204.awd", build_p55tvp4(),
         "'[Socket 7] ASUS P/I-P55TVP4' Pentium-interpreter test ROM"),
    ]
    for subdir, fn, data, desc in targets:
        outdir = os.path.join(romsdir, subdir)
        os.makedirs(outdir, exist_ok=True)
        out = os.path.join(outdir, fn)
        with open(out, "wb") as f:
            f.write(data)
        print(f"wrote {out} ({len(data)} bytes) — {desc}")


if __name__ == "__main__":
    main()
