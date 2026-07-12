#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#include <errno.h>

#include "ibm.h"
#include "hdd_file.h"
#include "ramdisk/ramdisk.h"
#include "minivhd/minivhd.h"
#include "minivhd/minivhd_util.h"

bool is_ramdisk_file(const char *fn) {
        const char *ext1 = ".rdimg";
        const char *ext2 = ".rdvhd";
        int ext_len = 6;
        int len = strlen(fn);

        if (len < ext_len)
                return false;

        const char *extp = fn + (len - ext_len);
        return (strncmp(extp, ext1, ext_len) == 0) ||
                (strncmp(extp, ext2, ext_len) == 0);
}

void hdd_load_ext(hdd_file_t *hdd, const char *fn, int spt, int hpc, int tracks, int read_only) {
        int requested_read_only = read_only;
        bool is_ramdisk = is_ramdisk_file(fn);
        if (is_ramdisk)
                read_only = 1;

#ifdef __EMSCRIPTEN__
        /* PCem-web: "browser:<id>:<name>" images are lazily-read views over a
           local browser File (see wasm/platform/wasm-diskio.c). Large images
           cannot be staged into MEMFS (they would have to fit in wasm memory). */
        if (!strncmp(fn, "browser:", 8)) {
                extern int wasm_diskio_parse(const char *fn);
                int id = wasm_diskio_parse(fn);
                if (id < 0) {
                        pclog("hdd_load: browser disk not registered: %s\n", fn);
                        hdd->f = NULL;
                        return;
                }
                hdd->f = (void *)(intptr_t)(id + 1); /* nonzero handle */
                hdd->img_type = HDD_IMG_BROWSER;
                hdd->spt = spt;
                hdd->hpc = hpc;
                hdd->tracks = tracks;
                hdd->sectors = spt * hpc * tracks;
                hdd->read_only = requested_read_only;
                return;
        }
#endif

        if (hdd->f == NULL) {
                /* Try to open existing hard disk image */
                if (read_only)
                        hdd->f = (void *)fopen(fn, "rb");
                else
                        hdd->f = (void *)fopen(fn, "rb+");
                if (hdd->f != NULL) {
                        hdd->img_type = HDD_IMG_RAW;

                        /* Check if the file we opened is a VHD */
                        if (mvhd_file_is_vhd((FILE *)hdd->f)) {
                                int err;
                                fclose((FILE *)hdd->f);
                                MVHDMeta *vhdm = mvhd_open(fn, (bool)read_only, &err);
                                if (vhdm == NULL) {
                                        hdd->f = NULL;
                                        if (err == MVHD_ERR_FILE) {
                                                pclog("Cannot open VHD file '%s' : %s", fn, strerror(mvhd_errno));
                                        } else {
                                                pclog("Cannot open VHD file '%s' : %s", fn, mvhd_strerr(err));
                                        }
                                        return;
                                }
                                /*else if (err == MVHD_ERR_TIMESTAMP)
                                {
                                        pclog("VHD: Parent/child timestamp mismatch");
                                        mvhd_close(vhdm);
                                        hdd->f = NULL;
                                        return;
                                }*/ // FIX: This is to see if this resolves Issue #59
                                hdd->f = (void *)vhdm;
                                hdd->img_type = HDD_IMG_VHD;
                        }
                } else {
                        /* Failed to open existing hard disk image */
                        if (errno == ENOENT && !read_only) {
                                /* Failed because it does not exist,
                                   so try to create new file */
                                hdd->f = (void *)fopen(fn, "wb+");
                                if (hdd->f == NULL) {
                                        pclog("Cannot create file '%s': %s", fn, strerror(errno));
                                        return;
                                }
                        } else {
                                /* Failed for another reason */
                                pclog("Cannot open file '%s': %s", fn, strerror(errno));
                                return;
                        }
                        hdd->img_type = HDD_IMG_RAW;
                }
        }
        if (hdd->img_type == HDD_IMG_VHD) {
                MVHDMeta *vhdm = (MVHDMeta *)hdd->f;
                MVHDGeom geom = mvhd_get_geometry(vhdm);
                hdd->spt = geom.spt;
                hdd->hpc = geom.heads;
                hdd->tracks = geom.cyl;
        } else if (hdd->img_type == HDD_IMG_RAW) {
                hdd->spt = spt;
                hdd->hpc = hpc;
                hdd->tracks = tracks;
        }
        hdd->sectors = hdd->spt * hdd->hpc * hdd->tracks;
        hdd->read_only = read_only;

        if (is_ramdisk) {
                ramdisk_t *ramdisk = ramdisk_init();
                if (ramdisk == NULL) {
                        pclog("Cannot initialize ramdisk '%s' : %s", fn, strerror(errno));
                        return;
                }

                // prepare ramdisk buffer to have enough space for whole raw image
                size_t size = hdd->sectors * 512 + 1;
                if (ramdisk_set_size(ramdisk, size) < 0) {
                        ramdisk_free(ramdisk);
                        pclog("Cannot set ramdisk '%s' size to %d: %s", fn, size, strerror(errno));
                        return;
                }

                if (hdd->img_type == HDD_IMG_RAW) {
                        if (ramdisk_load_file(ramdisk, (FILE *)hdd->f) < 0) {
                                pclog("Cannot load ramdisk from file '%s' : %s", fn, strerror(errno));
                                ramdisk_free(ramdisk);
                                return;
                        }
                        fclose((FILE *)hdd->f);
                } else if (hdd->img_type == HDD_IMG_VHD) {
                        char *rd_buf;
                        size_t rd_buf_size;

                        // get temporary buffer from beginning of disk (cursor is at 0)
                        if (ramdisk_get_cursor_mem(ramdisk, &rd_buf, &rd_buf_size) < 0) {
                                pclog("Unable to get ramdisk cursor memory pointer '%s'", fn);
                                ramdisk_free(ramdisk);
                                return;
                        }

                        // immediately read all sectors from VHD to our raw ramdisk memory
                        if (mvhd_read_sectors((MVHDMeta *)hdd->f, 0, hdd->sectors, rd_buf) < 0) {
                                pclog("Unable to read VHD image sectors to ramdisk '%s'", fn);
                                ramdisk_free(ramdisk);
                                return;
                        }
                        mvhd_close((MVHDMeta *)hdd->f);
                } else {
                        pclog("Unsupported HDD image type for ramdisk '%s'", fn);
                        ramdisk_free(ramdisk);
                        return;
                }

                hdd->f = (void *)ramdisk;
                hdd->img_type = HDD_IMG_RAW_RAM;
                hdd->read_only = requested_read_only;
        }

}

void hdd_load(hdd_file_t *hdd, int d, const char *fn) { hdd_load_ext(hdd, fn, hdc[d].spt, hdc[d].hpc, hdc[d].tracks, 0); }

void hdd_close(hdd_file_t *hdd) {
        if (hdd->f) {
                if (hdd->img_type == HDD_IMG_BROWSER)
                        ; /* PCem-web: registry owned by JS; nothing to close */
                else if (hdd->img_type == HDD_IMG_VHD)
                        mvhd_close((MVHDMeta *)hdd->f);
                else if (hdd->img_type == HDD_IMG_RAW)
                        fclose((FILE *)hdd->f);
                else if (hdd->img_type == HDD_IMG_RAW_RAM)
                        ramdisk_free((ramdisk_t *)hdd->f);
        }
        hdd->img_type = HDD_IMG_RAW;
        hdd->f = NULL;
}

int hdd_read_sectors(hdd_file_t *hdd, int offset, int nr_sectors, void *buffer) {
#ifdef __EMSCRIPTEN__
        if (hdd->img_type == HDD_IMG_BROWSER) {
                extern int wasm_diskio_read(int id, uint64_t byte_offset, int len, void *buf);
                int transfer_sectors = nr_sectors;
                if ((hdd->sectors - offset) < transfer_sectors)
                        transfer_sectors = hdd->sectors - offset;
                if (transfer_sectors <= 0)
                        return 1;
                if (wasm_diskio_read((int)(intptr_t)hdd->f - 1, (uint64_t)offset * 512, transfer_sectors * 512, buffer))
                        return 1;
                return (nr_sectors != transfer_sectors) ? 1 : 0;
        }
#endif
        if (hdd->img_type == HDD_IMG_VHD) {
                return mvhd_read_sectors((MVHDMeta *)hdd->f, offset, nr_sectors, buffer);
        } else if (hdd->img_type == HDD_IMG_RAW ||
                        hdd->img_type == HDD_IMG_RAW_RAM) {
                off_t addr;
                int transfer_sectors = nr_sectors;

                if ((hdd->sectors - offset) < transfer_sectors)
                        transfer_sectors = hdd->sectors - offset;
                addr = (uint64_t)offset * 512;

                if (hdd->img_type == HDD_IMG_RAW) {
                        fseeko64((FILE *)hdd->f, addr, SEEK_SET);
                        fread(buffer, transfer_sectors * 512, 1, (FILE *)hdd->f);
                } else if (hdd->img_type == HDD_IMG_RAW_RAM) {
                        ramdisk_t *ramdisk = (ramdisk_t *)hdd->f;
                        ramdisk_seek(ramdisk, addr, SEEK_SET);
                        ramdisk_read(ramdisk, buffer, transfer_sectors * 512);
                } else
                        return 1;

                if (nr_sectors != transfer_sectors)
                        return 1;
                return 0;
        }
        /* Keep the compiler happy */
        return 1;
}

int hdd_write_sectors(hdd_file_t *hdd, int offset, int nr_sectors, void *buffer) {
#ifdef __EMSCRIPTEN__
        if (hdd->img_type == HDD_IMG_BROWSER) {
                extern int wasm_diskio_write(int id, uint64_t byte_offset, int len, void *buf);
                int transfer_sectors = nr_sectors;
                if (hdd->read_only)
                        return 1;
                if ((hdd->sectors - offset) < transfer_sectors)
                        transfer_sectors = hdd->sectors - offset;
                if (transfer_sectors <= 0)
                        return 1;
                if (wasm_diskio_write((int)(intptr_t)hdd->f - 1, (uint64_t)offset * 512, transfer_sectors * 512, buffer))
                        return 1;
                return (nr_sectors != transfer_sectors) ? 1 : 0;
        }
#endif
        if (hdd->img_type == HDD_IMG_VHD) {
                return mvhd_write_sectors((MVHDMeta *)hdd->f, offset, nr_sectors, buffer);
        } else if (hdd->img_type == HDD_IMG_RAW ||
                        hdd->img_type == HDD_IMG_RAW_RAM) {
                off_t addr;
                int transfer_sectors = nr_sectors;

                if (hdd->read_only)
                        return 1;

                if ((hdd->sectors - offset) < transfer_sectors)
                        transfer_sectors = hdd->sectors - offset;
                addr = (uint64_t)offset * 512;

                if (hdd->img_type == HDD_IMG_RAW) {
                        fseeko64((FILE *)hdd->f, addr, SEEK_SET);
                        fwrite(buffer, transfer_sectors * 512, 1, (FILE *)hdd->f);
                } else if (hdd->img_type == HDD_IMG_RAW_RAM) {
                        ramdisk_t *ramdisk = (ramdisk_t *)hdd->f;
                        ramdisk_seek(ramdisk, addr, SEEK_SET);
                        ramdisk_write(ramdisk, buffer, transfer_sectors * 512);
                } else
                        return 1;

                if (nr_sectors != transfer_sectors)
                        return 1;
                return 0;
        }
        /* Keep the compiler happy */
        return 1;
}

int hdd_format_sectors(hdd_file_t *hdd, int offset, int nr_sectors) {
#ifdef __EMSCRIPTEN__
        if (hdd->img_type == HDD_IMG_BROWSER) {
                extern int wasm_diskio_write(int id, uint64_t byte_offset, int len, void *buf);
                static const uint8_t zeros[512];
                int c;
                int transfer_sectors = nr_sectors;
                if (hdd->read_only)
                        return 1;
                if ((hdd->sectors - offset) < transfer_sectors)
                        transfer_sectors = hdd->sectors - offset;
                for (c = 0; c < transfer_sectors; c++) {
                        if (wasm_diskio_write((int)(intptr_t)hdd->f - 1, (uint64_t)(offset + c) * 512, 512, (void *)zeros))
                                return 1;
                }
                return (nr_sectors != transfer_sectors) ? 1 : 0;
        }
#endif
        if (hdd->img_type == HDD_IMG_VHD) {
                return mvhd_format_sectors((MVHDMeta *)hdd->f, offset, nr_sectors);
        } else if (hdd->img_type == HDD_IMG_RAW ||
                        hdd->img_type == HDD_IMG_RAW_RAM) {
                off_t addr;
                int c;
                uint8_t zero_buffer[512];
                int transfer_sectors = nr_sectors;

                if (hdd->read_only)
                        return 1;

                memset(zero_buffer, 0, 512);

                if ((hdd->sectors - offset) < transfer_sectors)
                        transfer_sectors = hdd->sectors - offset;
                addr = (uint64_t)offset * 512;

                if (hdd->img_type == HDD_IMG_RAW) {
                        fseeko64((FILE *)hdd->f, addr, SEEK_SET);
                        for (c = 0; c < transfer_sectors; c++)
                                fwrite(zero_buffer, 512, 1, (FILE *)hdd->f);
                } else if (hdd->img_type == HDD_IMG_RAW_RAM) {
                        ramdisk_t *ramdisk = (ramdisk_t *)hdd->f;
                        ramdisk_seek(ramdisk, addr, SEEK_SET);
                        for (c = 0; c < transfer_sectors; c++)
                                ramdisk_write(ramdisk, zero_buffer, 512);
                } else
                        return 1;

                if (nr_sectors != transfer_sectors)
                        return 1;
                return 0;
        }
        /* Keep the compiler happy */
        return 1;
}
