/*
 * wasm-bridge.cpp — PCem-web C++ <-> JavaScript API surface (embind).
 *
 * Everything the web front-end knows about the emulator flows through here:
 *
 *   - hardware inventory, enumerated live from the same registration tables
 *     the native "Configure machine" dialog uses (models[], video_cards[],
 *     sound cards, HDD controllers), filtered by which ROM sets were found
 *     in the server's /roms directory;
 *   - JSON machine profiles -> pcem.cfg translation (the JSON schema is the
 *     single source of truth for the UI; the INI text fed to config_load()
 *     is generated here with exactly the keys pc.c's loadconfig() reads);
 *   - lifecycle control, media hot-swap, input injection and the
 *     framebuffer/audio shared-memory layout exports.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "ibm.h"
#include "device.h"
#include "model.h"
#include "video.h"
#include "sound.h"
#include "hdd.h"
#include "cpu.h"
#include "nethandler.h"    /* network card table (USE_NETWORKING build) */
#include "gameport.h"      /* joystick type metadata */
#include "plat-joystick.h" /* POV_X / POV_Y mapping encodings */
#include "plat-midi.h"     /* midi_get_num_devs / midi_get_dev_name */

/* wasm platform exports */
void pcem_wasm_scan_roms();
int pcem_wasm_start(const char *cfg_text);
void pcem_wasm_stop();
void pcem_wasm_set_paused(int p);
void pcem_wasm_reset(int hard);
void pcem_wasm_fdd_load(int drive, const char *path);
void pcem_wasm_fdd_eject(int drive);
void pcem_wasm_cdrom_load(const char *path);
void pcem_wasm_cdrom_eject();
int pcem_wasm_state();
int pcem_wasm_speed_pct();
int pcem_wasm_video_fps();

uint8_t *pcem_fb_ptr();
int pcem_fb_width();
int pcem_fb_height();
int pcem_fb_stride();
uint32_t pcem_fb_generation();
int pcem_fb_aspect_w();
int pcem_fb_aspect_h();

uint32_t *pcem_audio_layout();
void pcem_audio_set_muted(int m);

int wasm_jit_selftest(void);
int wasm_jit_selftest_all(void);

uint8_t *pcem_key_ptr();
void pcem_key_set(int scancode, int down);
void pcem_key_release_all();
void pcem_mouse_event(int dx, int dy, int dz, int buttons);
void pcem_set_mousecapture(int c);

extern int romspresent[ROM_MAX];
extern int gfx_present[GFX_MAX];
extern VIDEO_CARD *video_cards[GFX_MAX];

/* The 3Dfx add-on board is not part of the video card table; it is its own
   device the core instantiates when `voodoo = 1` (vid_voodoo.c). */
extern device_t voodoo_device;
}

using emscripten::val;

/* ------------------------------------------------------------------------- */
/* Hardware inventory                                                          */
/* ------------------------------------------------------------------------- */

static val cpu_to_val(const CPU &c) {
        val o = val::object();
        o.set("name", std::string(c.name));
        o.set("speed", c.rspeed);
        o.set("supports_dynarec", (c.cpu_flags & CPU_SUPPORTS_DYNAREC) ? true : false);
        o.set("requires_dynarec", (c.cpu_flags & CPU_REQUIRES_DYNAREC) ? true : false);

        val fpus = val::array();
        if (c.fpus) {
                for (int f = 0; c.fpus[f].name; f++) {
                        val fo = val::object();
                        fo.set("name", std::string(c.fpus[f].name));
                        fo.set("internal_name", std::string(c.fpus[f].internal_name));
                        fpus.call<void>("push", fo);
                }
        }
        o.set("fpus", fpus);
        return o;
}

static val get_machines() {
        val arr = val::array();

        for (int m = 0; m < model_count(); m++) {
                MODEL *mod = models[m];
                val o = val::object();

                o.set("index", m);
                o.set("name", std::string(mod->name));
                o.set("internal_name", std::string(mod->internal_name));
                o.set("romset", mod->id);
                o.set("available", (mod->id >= 0 && mod->id < ROM_MAX && romspresent[mod->id]) ? true : false);
                o.set("is_at", (mod->flags & MODEL_AT) ? true : false);
                o.set("has_ide", (mod->flags & MODEL_HAS_IDE) ? true : false);
                o.set("is_pci", (mod->flags & MODEL_PCI) ? true : false);
                o.set("fixed_gfx", model_has_fixed_gfx(m) ? true : false);
                o.set("optional_gfx", model_has_optional_gfx(m) ? true : false);
                o.set("min_ram", mod->min_ram);
                o.set("max_ram", mod->max_ram);
                o.set("ram_granularity", mod->ram_granularity);

                /* CPU family tables terminate on cpus == NULL. The NAME is
                   often "" (single-family machines like every XT) — an empty
                   name is NOT a terminator, it just means the family has no
                   manufacturer label. */
                val manufacturers = val::array();
                for (int cm = 0; cm < 5; cm++) {
                        if (!mod->cpu[cm].cpus)
                                break;
                        val mo = val::object();
                        mo.set("name", std::string(mod->cpu[cm].name[0] ? mod->cpu[cm].name : "Default"));
                        val cpus = val::array();
                        for (int c = 0; mod->cpu[cm].cpus[c].cpu_type != -1; c++)
                                cpus.call<void>("push", cpu_to_val(mod->cpu[cm].cpus[c]));
                        mo.set("cpus", cpus);
                        manufacturers.call<void>("push", mo);
                }
                o.set("cpu_manufacturers", manufacturers);

                arr.call<void>("push", o);
        }
        return arr;
}

static val get_video_cards() {
        val arr = val::array();

        for (int c = 0; c < GFX_MAX; c++) {
                if (!video_cards[c])
                        continue;
                val o = val::object();
                o.set("name", std::string(video_cards[c]->name));
                o.set("internal_name", std::string(video_cards[c]->internal_name));
                o.set("available", video_card_available(c) ? true : false);
                arr.call<void>("push", o);
        }
        return arr;
}

static val get_sound_cards() {
        val arr = val::array();

        for (int c = 0; c < 64; c++) {
                char *name = sound_card_getname(c);
                if (!name || !name[0])
                        break;
                val o = val::object();
                o.set("name", std::string(name));
                o.set("internal_name", std::string(sound_card_get_internal_name(c)));
                o.set("available", sound_card_available(c) ? true : false);
                arr.call<void>("push", o);
        }
        return arr;
}

static val get_hdd_controllers() {
        val arr = val::array();

        for (int c = 0; c < 64; c++) {
                char *name = hdd_controller_get_name(c);
                if (!name || !name[0])
                        break;
                char *iname = hdd_controller_get_internal_name(c);
                val o = val::object();
                o.set("name", std::string(name));
                o.set("internal_name", std::string(iname));
                o.set("available", hdd_controller_available(c) ? true : false);
                o.set("is_mfm", hdd_controller_is_mfm(iname) ? true : false);
                o.set("is_ide", hdd_controller_is_ide(iname) ? true : false);
                o.set("is_scsi", hdd_controller_is_scsi(iname) ? true : false);
                arr.call<void>("push", o);
        }
        return arr;
}

static val get_network_cards() {
        val arr = val::array();

        /* network_cards[] terminates on NULL; slot 0 is "None" (internal
           name "" — network_card_get_from_internal_name("") resolves to it,
           which is also what an absent `netcard` cfg key means). */
        for (int c = 0; c < NETWORK_CARD_MAX; c++) {
                char *name = network_card_getname(c);
                if (!name || !name[0])
                        break;
                device_t *dev = network_card_getdevice(c);
                val o = val::object();
                o.set("name", std::string(name));
                o.set("internal_name", std::string(network_card_get_internal_name(c)));
                o.set("available", network_card_available(c) ? true : false);
                o.set("is_pci", (dev && (dev->flags & DEVICE_PCI)) ? true : false);
                arr.call<void>("push", o);
        }
        return arr;
}

static val get_joystick_types() {
        val arr = val::array();

        for (int j = 0; joystick_get_name(j); j++) {
                val o = val::object();
                o.set("name", std::string(joystick_get_name(j)));
                o.set("max_joysticks", joystick_get_max_joysticks(j));
                o.set("axis_count", joystick_get_axis_count(j));
                o.set("button_count", joystick_get_button_count(j));
                o.set("pov_count", joystick_get_pov_count(j));
                arr.call<void>("push", o);
        }
        return arr;
}

static val get_inventory() {
        val o = val::object();
        o.set("machines", get_machines());
        o.set("video_cards", get_video_cards());
        o.set("sound_cards", get_sound_cards());
        o.set("hdd_controllers", get_hdd_controllers());
        o.set("network_cards", get_network_cards());
        o.set("joystick_types", get_joystick_types());
        return o;
}

/* ------------------------------------------------------------------------- */
/* Device configuration descriptors                                           */
/*                                                                             */
/* Native PCem builds per-device "Configure" dialogs straight from each        */
/* device's device_config_t table (wx-deviceconfig.cc). This exports the same  */
/* tables to JS so the web editor can render identical dialogs. Values edited  */
/* there travel back through the profile's device_settings pass-through as a   */
/* [section] named after device_t.name — exactly the section                   */
/* device_get_config_int() reads (see plugin-api/device.c).                    */
/* ------------------------------------------------------------------------- */

static val config_items_to_val(device_config_t *config) {
        val items = val::array();
        while (config->type != -1) {
                /* CONFIG_MIDI selects a host midi-out device. Hidden while
                   midi_get_num_devs() == 0 (native behaviour with no MIDI
                   ports); once web/js/midi.js registers WebMIDI outputs the
                   row appears with the real device list as its options.
                   The value is the MACHINE-level `midi` cfg key, not a
                   [section] key — the JS dialog special-cases it. */
                if (config->type == CONFIG_MIDI && midi_get_num_devs() == 0) {
                        config++;
                        continue;
                }
                val o = val::object();
                o.set("name", std::string(config->name));
                o.set("description", std::string(config->description));
                o.set("type", config->type);
                o.set("default_int", config->default_int);
                o.set("default_string", std::string(config->default_string));
                if (config->type == CONFIG_SELECTION) {
                        val sels = val::array();
                        device_config_selection_t *sel = config->selection;
                        /* selection lists terminate on an empty description
                           (trailing array slots are zero-initialised) */
                        while (sel->description[0]) {
                                val s = val::object();
                                s.set("description", std::string(sel->description));
                                s.set("value", sel->value);
                                sels.call<void>("push", s);
                                sel++;
                        }
                        o.set("selection", sels);
                }
                if (config->type == CONFIG_MIDI) {
                        val sels = val::array();
                        for (int d = 0; d < midi_get_num_devs(); d++) {
                                char name[64];
                                midi_get_dev_name(d, name);
                                val s = val::object();
                                s.set("description", std::string(name));
                                s.set("value", d);
                                sels.call<void>("push", s);
                        }
                        o.set("selection", sels);
                }
                items.call<void>("push", o);
                config++;
        }
        return items;
}

static int model_index_by_name(const std::string &internal_name) {
        for (int m = 0; m < model_count(); m++)
                if (internal_name == models[m]->internal_name)
                        return m;
        return -1;
}

/* kind: "machine" | "video" | "sound" | "hdd" | "voodoo".
   name: the card's internal name ("builtin" allowed for video).
   machine: machine internal name — needed because built-in video resolves
   through the romset (video_card_getdevice), same as the native dialog. */
static device_t *find_config_device(const std::string &kind, const std::string &name, const std::string &machine) {
        int m_idx = model_index_by_name(machine);

        if (kind == "voodoo")
                return &voodoo_device;

        if (kind == "machine")
                return (m_idx >= 0) ? models[m_idx]->device : NULL;

        if (kind == "sound") {
                for (int c = 0; c < 64; c++) {
                        char *iname = sound_card_get_internal_name(c);
                        if (!iname || !iname[0])
                                break;
                        if (name == iname)
                                return sound_card_getdevice(c);
                }
                return NULL;
        }

        if (kind == "video") {
                int romset = (m_idx >= 0) ? model_getromset_from_model(m_idx) : -1;
                if (name == "builtin") {
                        /* only meaningful on machines that actually have
                           built-in video — video_card_getdevice()'s romset
                           switch covers exactly those (the same states the
                           native dialog can reach) */
                        if (m_idx < 0 || !(model_has_fixed_gfx(m_idx) || model_has_optional_gfx(m_idx)))
                                return NULL;
                        return video_card_getdevice(GFX_BUILTIN, romset);
                }
                for (int c = 0; c < GFX_MAX; c++) {
                        if (!video_cards[c])
                                continue;
                        if (name == video_cards[c]->internal_name)
                                return video_card_getdevice(c, romset);
                }
                return NULL;
        }

        if (kind == "hdd")
                return hdd_controller_get_device((char *)name.c_str());

        if (kind == "network") {
                for (int c = 0; c < NETWORK_CARD_MAX; c++) {
                        char *cname = network_card_getname(c);
                        if (!cname || !cname[0])
                                break;
                        if (name == network_card_get_internal_name(c))
                                return network_card_getdevice(c);
                }
                return NULL;
        }

        return NULL;
}

static val js_device_config(std::string kind, std::string name, std::string machine) {
        device_t *dev = find_config_device(kind, name, machine);
        if (!dev || !dev->config)
                return val::null();

        val items = config_items_to_val(dev->config);
        if (items["length"].as<int>() == 0)
                return val::null(); /* e.g. config was MIDI-only */

        val o = val::object();
        o.set("device_name", std::string(dev->name));
        /* device_get_config_*() reads section [device name] from pcem.cfg */
        o.set("section", std::string(dev->name));
        o.set("items", items);
        return o;
}

/* ------------------------------------------------------------------------- */
/* JSON profile -> pcem.cfg                                                    */
/* ------------------------------------------------------------------------- */

static void cfg_kv(std::string &out, const char *key, const std::string &value) {
        out += key;
        out += " = ";
        out += value;
        out += "\n";
}

static void cfg_kv_int(std::string &out, const char *key, int value) { cfg_kv(out, key, std::to_string(value)); }

static std::string p_str(val p, const char *key, const char *dflt) {
        val v = p[key];
        if (v.isUndefined() || v.isNull())
                return dflt;
        return v.as<std::string>();
}

static int p_int(val p, const char *key, int dflt) {
        val v = p[key];
        if (v.isUndefined() || v.isNull())
                return dflt;
        return v.as<int>();
}

static bool p_bool(val p, const char *key, bool dflt) {
        val v = p[key];
        if (v.isUndefined() || v.isNull())
                return dflt;
        return v.as<bool>();
}

/* Build the exact INI text pc.c::loadconfig() + device config sections read. */
static std::string build_cfg_text(val profile) {
        std::string out;

        cfg_kv(out, "model", p_str(profile, "machine", "genxt"));
        cfg_kv_int(out, "cpu_manufacturer", p_int(profile, "cpu_manufacturer", 0));
        cfg_kv_int(out, "cpu", p_int(profile, "cpu", 0));
        cfg_kv(out, "fpu", p_str(profile, "fpu", "none"));
        /* PCem-web: the wasm recompiler backend (runtime-generated wasm
           modules) services the dynarec path. Profiles opt in via
           "cpu_dynarec"; default stays off until a profile/devcfg enables
           it. "cpu_dynarec_oracle" additionally runs every compiled block
           under the interpreter-differential oracle (validation builds). */
        /* The runtime wasm CPU recompiler is OPT-IN. It is oracle-verified
           bit-exact and wins ~2.7x on tight compute, but on dispatch-bound
           workloads (a Win9x desktop: timer ticks, tiny blocks, I/O) it
           measures ~0.6x the interpreter — per-block wasm dispatch overhead
           dominates. Field report matched the bench (67% at an idle desktop),
           so the honest default is the interpreter. */
        cfg_kv_int(out, "cpu_use_dynarec", p_bool(profile, "cpu_dynarec", false) ? 1 : 0);
        cfg_kv_int(out, "cpu_dynarec_oracle", p_bool(profile, "cpu_dynarec_oracle", false) ? 1 : 0);
        cfg_kv_int(out, "cpu_waitstates", p_int(profile, "cpu_waitstates", 0));
        cfg_kv_int(out, "mem_size", p_int(profile, "mem_kb", 640));

        cfg_kv(out, "gfxcard", p_str(profile, "gfxcard", ""));
        cfg_kv_int(out, "video_speed", p_int(profile, "video_speed", -1));

        cfg_kv(out, "sndcard", p_str(profile, "sndcard", "none"));
        cfg_kv_int(out, "gameblaster", p_bool(profile, "gameblaster", false) ? 1 : 0);
        cfg_kv_int(out, "gus", p_bool(profile, "gus", false) ? 1 : 0);
        cfg_kv_int(out, "ssi2001", p_bool(profile, "ssi2001", false) ? 1 : 0);
        cfg_kv_int(out, "voodoo", p_bool(profile, "voodoo", false) ? 1 : 0);

        cfg_kv(out, "hdd_controller", p_str(profile, "hdd_controller", "none"));

        /* Floppy drives: types + initial images. */
        val fdd = profile["fdd"];
        int fda_type = 7, fdb_type = 7;
        std::string fda_fn, fdb_fn;
        if (!fdd.isUndefined() && !fdd.isNull()) {
                int len = fdd["length"].as<int>();
                if (len > 0) {
                        fda_type = p_int(fdd[0], "type", 7);
                        fda_fn = p_str(fdd[0], "image", "");
                }
                if (len > 1) {
                        fdb_type = p_int(fdd[1], "type", 7);
                        fdb_fn = p_str(fdd[1], "image", "");
                }
        }
        cfg_kv_int(out, "drive_a_type", fda_type);
        cfg_kv_int(out, "drive_b_type", fdb_type);
        cfg_kv(out, "disc_a", fda_fn);
        cfg_kv(out, "disc_b", fdb_fn);
        cfg_kv_int(out, "bpb_disable", p_bool(profile, "bpb_disable", false) ? 1 : 0);

        /* Hard disks: pc.c reads seven (hdc..hdi), keys are historical. */
        static const char *hd_prefix[7] = {"hdc", "hdd", "hde", "hdf", "hdg", "hdh", "hdi"};
        val hdd = profile["hdd"];
        for (int i = 0; i < 7; i++) {
                int spt = 0, hpc = 0, tracks = 0;
                std::string fn;
                if (!hdd.isUndefined() && !hdd.isNull() && i < hdd["length"].as<int>()) {
                        val d = hdd[i];
                        if (!d.isUndefined() && !d.isNull()) {
                                spt = p_int(d, "sectors", 0);
                                hpc = p_int(d, "heads", 0);
                                tracks = p_int(d, "cylinders", 0);
                                fn = p_str(d, "file", "");
                        }
                }
                char key[32];
                snprintf(key, sizeof(key), "%s_sectors", hd_prefix[i]);
                cfg_kv_int(out, key, spt);
                snprintf(key, sizeof(key), "%s_heads", hd_prefix[i]);
                cfg_kv_int(out, key, hpc);
                snprintf(key, sizeof(key), "%s_cylinders", hd_prefix[i]);
                cfg_kv_int(out, key, tracks);
                snprintf(key, sizeof(key), "%s_fn", hd_prefix[i]);
                cfg_kv(out, key, fn);
        }

        /* CD-ROM: image-backed or disabled. 200 == CDROM_IMAGE. */
        val cd = profile["cdrom"];
        bool cd_enabled = false;
        std::string cd_image;
        int cd_channel = 2;
        if (!cd.isUndefined() && !cd.isNull()) {
                cd_enabled = p_bool(cd, "enabled", false);
                cd_image = p_str(cd, "image", "");
                cd_channel = p_int(cd, "channel", 2);
        }
        /* 200 == CDROM_IMAGE. -1 == the unix "null CD" backend (empty drive):
           pc.c only routes it to cdrom_null_open() — which is what makes the
           global `atapi` ops valid — for -1, not 0. 0 is the WINDOWS "no host
           drive" value and routes to ioctl_set_drive(), a no-op in the dummy
           ioctl backend this build uses, leaving `atapi` NULL: the first IDE
           reset in POST then dies on atapi->stop(). */
        cfg_kv_int(out, "cdrom_drive", (cd_enabled && !cd_image.empty()) ? 200 : -1);
        cfg_kv_int(out, "cdrom_channel", cd_channel);
        cfg_kv(out, "cdrom_path", cd_image);
        cfg_kv_int(out, "cd_speed", p_int(profile, "cd_speed", 24));
        cfg_kv(out, "cd_model", p_str(profile, "cd_model", "pcemcd"));

        cfg_kv_int(out, "zip_channel", p_int(profile, "zip_channel", -1));
        cfg_kv_int(out, "mouse_type", p_int(profile, "mouse_type", 0));
        int joystick_type_sel = p_int(profile, "joystick_type", 0);
        cfg_kv_int(out, "joystick_type", joystick_type_sel);
        cfg_kv_int(out, "enable_sync", p_int(profile, "enable_sync", 1));

        /* Network: card selection is the machine-level `netcard` internal
           name (pc.c loadconfig); the NE2000's MAC is `macaddr`. Which
           backend the frames flow through (relay URL etc.) is a browser
           concern — web/js/net.js arms wasm-net.c before start. */
        cfg_kv(out, "netcard", p_str(profile, "netcard", ""));
        cfg_kv(out, "macaddr", p_str(profile, "macaddr", ""));

        /* Host MIDI-out device index (WebMIDI output list order) — the same
           machine-level `midi` key native platforms read in midi_init(). */
        cfg_kv_int(out, "midi", p_int(profile, "midi_dev", 0));

        /* [Joysticks]: map host gamepads onto the emulated sticks 1:1 when
           the profile's host-gamepad toggle is on. Default mappings are
           axis d->d and button d->d (the Standard Gamepad order already
           matches: axes 0/1 = left stick, 2/3 = right stick), and each POV
           reads hat d through the POV_X/POV_Y encodings — exactly the
           mapping the native SDL layer defaults to, made explicit. */
        out += "\n[Joysticks]\n";
        {
                bool pads_on = p_bool(profile, "gamepad_enabled", true);
                int maxj = joystick_get_max_joysticks(joystick_type_sel);
                for (int c = 0; c < maxj; c++) {
                        char key[48];
                        snprintf(key, sizeof(key), "joystick_%i_nr", c);
                        cfg_kv_int(out, key, pads_on ? c + 1 : 0);
                        if (!pads_on)
                                continue;
                        for (int d = 0; d < joystick_get_axis_count(joystick_type_sel); d++) {
                                snprintf(key, sizeof(key), "joystick_%i_axis_%i", c, d);
                                cfg_kv_int(out, key, d);
                        }
                        for (int d = 0; d < joystick_get_button_count(joystick_type_sel); d++) {
                                snprintf(key, sizeof(key), "joystick_%i_button_%i", c, d);
                                cfg_kv_int(out, key, d);
                        }
                        for (int d = 0; d < joystick_get_pov_count(joystick_type_sel); d++) {
                                snprintf(key, sizeof(key), "joystick_%i_pov_%i_x", c, d);
                                cfg_kv_int(out, key, (int)(POV_X | d));
                                snprintf(key, sizeof(key), "joystick_%i_pov_%i_y", c, d);
                                cfg_kv_int(out, key, (int)(POV_Y | d));
                        }
                }
        }

        /* Pass-through for per-device sections ({"Section": {"key": val}}). */
        val ds = profile["device_settings"];
        if (!ds.isUndefined() && !ds.isNull()) {
                val keys = val::global("Object").call<val>("keys", ds);
                int nsec = keys["length"].as<int>();
                for (int s = 0; s < nsec; s++) {
                        std::string section = keys[s].as<std::string>();
                        out += "\n[" + section + "]\n";
                        val sec = ds[section];
                        val skeys = val::global("Object").call<val>("keys", sec);
                        int nkey = skeys["length"].as<int>();
                        for (int k = 0; k < nkey; k++) {
                                std::string kn = skeys[k].as<std::string>();
                                std::string kv = sec[kn].call<std::string>("toString");
                                cfg_kv(out, kn.c_str(), kv);
                        }
                }
        }

        return out;
}

/* ------------------------------------------------------------------------- */
/* embind surface                                                              */
/* ------------------------------------------------------------------------- */

static int js_jit_selftest() { return wasm_jit_selftest_all(); }

/* Voodoo A/B framebuffer oracle (wasm/jit/voodoo_ab.c). */
extern "C" int pcem_voodoo_ab_test(void);
extern "C" int pcem_voodoo_ab_cases(void);
extern "C" int pcem_voodoo_ab_first_bad(void);
extern "C" int pcem_voodoo_ab_first_bad_off(void);
extern "C" int pcem_voodoo_ab_first_bad_ref(void);
extern "C" int pcem_voodoo_ab_first_bad_got(void);
extern "C" int pcem_voodoo_ab_fallback_cases(void);
extern "C" int pcem_voodoo_ab_fallback_mismatches(void);
extern "C" int pcem_voodoo_ab_fallback_installed(void);
extern "C" int pcem_voodoo_ab_covered_installed(void);
static val js_voodoo_ab() {
        int total_mism = pcem_voodoo_ab_test();
        val o = val::object();
        o.set("mismatches", total_mism - pcem_voodoo_ab_fallback_mismatches()); /* covered-sweep mismatches */
        o.set("cases", pcem_voodoo_ab_cases());
        o.set("first_bad", pcem_voodoo_ab_first_bad());
        o.set("first_bad_off", pcem_voodoo_ab_first_bad_off());
        o.set("first_bad_ref", pcem_voodoo_ab_first_bad_ref());
        o.set("first_bad_got", pcem_voodoo_ab_first_bad_got());
        o.set("fallback_cases", pcem_voodoo_ab_fallback_cases());
        o.set("fallback_mismatches", pcem_voodoo_ab_fallback_mismatches());
        o.set("fallback_installed", pcem_voodoo_ab_fallback_installed());
        o.set("covered_installed", pcem_voodoo_ab_covered_installed());
        return o;
}

/* JIT CPU stats for the oracle regression test. */
extern "C" int wjit_oracle_blocks_verified;
extern "C" int wjit_oracle_blocks_skipped;
extern "C" int wjit_oracle_mismatch;
extern "C" int codegen_wasm_install_failures(void);
extern "C" int cpu_recomp_blocks, cpu_new_blocks;
extern "C" int wjit_cold_runs, wjit_compile_threshold, wjit_nojit_blocks;
static val js_jit_stats() {
        val o = val::object();
        o.set("oracle_verified", wjit_oracle_blocks_verified);
        o.set("oracle_skipped", wjit_oracle_blocks_skipped);
        o.set("oracle_mismatch", wjit_oracle_mismatch);
        o.set("install_failures", codegen_wasm_install_failures());
        o.set("recomp_blocks", cpu_recomp_blocks);
        o.set("new_blocks", cpu_new_blocks);
        o.set("cold_runs", wjit_cold_runs);
        o.set("compile_threshold", wjit_compile_threshold);
        o.set("nojit_blocks", wjit_nojit_blocks);
        return o;
}

static void js_scan_roms() { pcem_wasm_scan_roms(); }
static val js_inventory() { return get_inventory(); }
static std::string js_build_cfg(val profile) { return build_cfg_text(profile); }
static int js_start(val profile) { return pcem_wasm_start(build_cfg_text(profile).c_str()); }
static void js_stop() { pcem_wasm_stop(); }
static void js_pause(bool p) { pcem_wasm_set_paused(p ? 1 : 0); }
static void js_reset(bool hard) { pcem_wasm_reset(hard ? 1 : 0); }
static int js_state() { return pcem_wasm_state(); }
static int js_speed() { return pcem_wasm_speed_pct(); }
static int js_fps() { return pcem_wasm_video_fps(); }

static void js_fdd_load(int drive, std::string path) { pcem_wasm_fdd_load(drive, path.c_str()); }
static void js_fdd_eject(int drive) { pcem_wasm_fdd_eject(drive); }
static void js_cd_load(std::string path) { pcem_wasm_cdrom_load(path.c_str()); }
static void js_cd_eject() { pcem_wasm_cdrom_eject(); }

static void js_key(int scancode, bool down) { pcem_key_set(scancode, down ? 1 : 0); }
static void js_keys_up() { pcem_key_release_all(); }
static void js_mouse(int dx, int dy, int dz, int buttons) { pcem_mouse_event(dx, dy, dz, buttons); }
static void js_mouse_capture(bool c) { pcem_set_mousecapture(c ? 1 : 0); }

static uintptr_t js_fb_ptr() { return (uintptr_t)pcem_fb_ptr(); }
static int js_fb_w() { return pcem_fb_width(); }
static int js_fb_h() { return pcem_fb_height(); }
static int js_fb_stride() { return pcem_fb_stride(); }
static uint32_t js_fb_gen() { return pcem_fb_generation(); }
static int js_fb_aw() { return pcem_fb_aspect_w(); }
static int js_fb_ah() { return pcem_fb_aspect_h(); }

static uintptr_t js_audio_layout() { return (uintptr_t)pcem_audio_layout(); }
static void js_audio_muted(bool m) { pcem_audio_set_muted(m ? 1 : 0); }

EMSCRIPTEN_BINDINGS(pcem) {
        emscripten::function("scanRoms", &js_scan_roms);
        emscripten::function("getInventory", &js_inventory);
        emscripten::function("getDeviceConfig", &js_device_config);
        emscripten::function("jitSelfTest", &js_jit_selftest);
        emscripten::function("jitStats", &js_jit_stats);
        emscripten::function("voodooABTest", &js_voodoo_ab);
        emscripten::function("buildCfgText", &js_build_cfg);
        emscripten::function("start", &js_start);
        emscripten::function("stop", &js_stop);
        emscripten::function("setPaused", &js_pause);
        emscripten::function("reset", &js_reset);
        emscripten::function("getState", &js_state);
        emscripten::function("getSpeedPct", &js_speed);
        emscripten::function("getVideoFps", &js_fps);

        emscripten::function("fddLoad", &js_fdd_load);
        emscripten::function("fddEject", &js_fdd_eject);
        emscripten::function("cdLoad", &js_cd_load);
        emscripten::function("cdEject", &js_cd_eject);

        emscripten::function("keyEvent", &js_key);
        emscripten::function("releaseAllKeys", &js_keys_up);
        emscripten::function("mouseEvent", &js_mouse);
        emscripten::function("setMouseCapture", &js_mouse_capture);

        emscripten::function("fbPtr", &js_fb_ptr);
        emscripten::function("fbWidth", &js_fb_w);
        emscripten::function("fbHeight", &js_fb_h);
        emscripten::function("fbStride", &js_fb_stride);
        emscripten::function("fbGeneration", &js_fb_gen);
        emscripten::function("fbAspectW", &js_fb_aw);
        emscripten::function("fbAspectH", &js_fb_ah);

        emscripten::function("audioLayoutPtr", &js_audio_layout);
        emscripten::function("setAudioMuted", &js_audio_muted);
}
