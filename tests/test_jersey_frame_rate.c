#undef NDEBUG
#include "mod_plugins.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *ids[7];
static void (*callbacks[7])(void);
static unsigned count, calls, selected_rate, selected_blend;

int psx_mod_register_activation_plugin(const char *id, void (*callback)(void)) {
    assert(count < 7);
    ids[count] = id;
    callbacks[count++] = callback;
    return 1;
}

int psx_mod_set_frame_interpolation_blend(uint32_t mode) {
    assert(calls++ == 0);
    selected_blend = mode;
    return 1;
}

int psx_mod_set_frame_interpolation(uint32_t rate) {
    assert(calls++ == 1);
    selected_rate = rate;
    return 1;
}

#include "../src/mods/jersey_frame_rate_plugin.c"

int main(void) {
    const char *suffixes[] = {"display", "60", "90", "120", "144", "165", "240"};
    const unsigned rates[] = {0, 60, 90, 120, 144, 165, 240};
    assert(count == 7);
    for (unsigned i = 0; i < count; ++i) {
        char expected[64];
        snprintf(expected, sizeof(expected), "jerseydevil.framerate.%s", suffixes[i]);
        assert(strcmp(ids[i], expected) == 0);
        calls = 0;
        callbacks[i]();
        assert(calls == 2);
        assert(selected_rate == rates[i]);
        assert(selected_blend == PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE);
    }
    puts("Jersey Devil frame rate plugin: PASS");
    return 0;
}
