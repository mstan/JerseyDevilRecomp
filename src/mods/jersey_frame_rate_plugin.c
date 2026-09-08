#include "mod_plugins.h"

/* Presentation only: never change the guest VBlank cadence. */
static void jersey_frame_rate_set(unsigned frames_per_second) {
    (void)psx_mod_set_frame_interpolation_blend(
        PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE);
    (void)psx_mod_set_frame_interpolation(frames_per_second);
}

static void jersey_frame_rate_display_activate(void) { jersey_frame_rate_set(0u); }
static void jersey_frame_rate_60_activate(void) { jersey_frame_rate_set(60u); }
static void jersey_frame_rate_90_activate(void) { jersey_frame_rate_set(90u); }
static void jersey_frame_rate_120_activate(void) { jersey_frame_rate_set(120u); }
static void jersey_frame_rate_144_activate(void) { jersey_frame_rate_set(144u); }
static void jersey_frame_rate_165_activate(void) { jersey_frame_rate_set(165u); }
static void jersey_frame_rate_240_activate(void) { jersey_frame_rate_set(240u); }

PSX_MOD_CONSTRUCTOR(jersey_register_frame_rate_plugins) {
    (void)psx_mod_register_activation_plugin(
        "jerseydevil.framerate.display", jersey_frame_rate_display_activate);
    (void)psx_mod_register_activation_plugin(
        "jerseydevil.framerate.60", jersey_frame_rate_60_activate);
    (void)psx_mod_register_activation_plugin(
        "jerseydevil.framerate.90", jersey_frame_rate_90_activate);
    (void)psx_mod_register_activation_plugin(
        "jerseydevil.framerate.120", jersey_frame_rate_120_activate);
    (void)psx_mod_register_activation_plugin(
        "jerseydevil.framerate.144", jersey_frame_rate_144_activate);
    (void)psx_mod_register_activation_plugin(
        "jerseydevil.framerate.165", jersey_frame_rate_165_activate);
    (void)psx_mod_register_activation_plugin(
        "jerseydevil.framerate.240", jersey_frame_rate_240_activate);
}
