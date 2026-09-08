#include "cpu_state.h"
#include "gpu.h"
#include "mod_plugins.h"

#include <stdint.h>

#define JERSEY_MAIN_DRAW_OT         0x80045770u
#define JERSEY_PLAYER_DRAW_OT       0x80045240u
#define JERSEY_MAIN_RAM_BASE         0x80000000u
#define JERSEY_MAIN_RAM_END          0x80200000u
#define JERSEY_LINK_TERMINATOR       0x00FFFFFFu
#define JERSEY_OT_WALK_LIMIT         8192u
#define JERSEY_HUD_OT_RANK           511u

#define JERSEY_HUD_ANCHOR_LEFT       (-1)
#define JERSEY_HUD_ANCHOR_CENTER     0
#define JERSEY_HUD_ANCHOR_RIGHT      1

static int s_widescreen_enabled;

static int jersey_is_main_ram(uint32_t address) {
    return address >= JERSEY_MAIN_RAM_BASE && address < JERSEY_MAIN_RAM_END &&
           (address & 3u) == 0u;
}

static uint32_t jersey_link_to_ram(uint32_t link) {
    if ((link & 0x00FFFFFFu) == JERSEY_LINK_TERMINATOR)
        return 0;
    return JERSEY_MAIN_RAM_BASE | (link & 0x00FFFFFFu);
}

static int jersey_poly_command_words(uint8_t op) {
    uint32_t verts;
    uint32_t gouraud;
    uint32_t textured;
    uint32_t words;

    if (op < 0x20u || op > 0x3Fu)
        return 0;

    verts = (op & 0x08u) ? 4u : 3u;
    gouraud = (op & 0x10u) ? 1u : 0u;
    textured = (op & 0x04u) ? 1u : 0u;

    words = 1u + verts;
    if (textured)
        words += verts;
    if (gouraud)
        words += verts - 1u;
    return (int)words;
}

static int jersey_rect_command_words(uint8_t op) {
    uint32_t size_selector;

    if (op < 0x60u || op > 0x7Fu)
        return 0;

    size_selector = (op >> 3) & 3u;
    if (size_selector == 0u)
        return (op & 0x04u) ? 4 : 3;
    return (op & 0x04u) ? 3 : 2;
}

static int jersey_gp0_command_words(uint8_t op) {
    if (op >= 0x20u && op <= 0x3Fu)
        return jersey_poly_command_words(op);
    if (op >= 0x60u && op <= 0x7Fu)
        return jersey_rect_command_words(op);

    switch (op) {
        case 0x00: return 1;
        case 0x01: return 1;
        case 0x02: return 3;
        case 0x1F: return 1;
        case 0x80: return 4;
        case 0xE1: return 1;
        case 0xE2: return 1;
        case 0xE3: return 1;
        case 0xE4: return 1;
        case 0xE5: return 1;
        case 0xE6: return 1;
        default: return 0;
    }
}

static void jersey_read_textured_quad_extent(uint32_t command,
                                             int32_t *min_x, int32_t *max_x,
                                             int32_t *min_y, int32_t *max_y) {
    const uint32_t vertex_offsets[4] = {4u, 12u, 20u, 28u};

    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t word = psx_mod_read_word(command + vertex_offsets[i]);
        int32_t x = (int32_t)(int16_t)(word & 0xFFFFu);
        int32_t y = (int32_t)(int16_t)(word >> 16);
        if (i == 0u || x < *min_x) *min_x = x;
        if (i == 0u || x > *max_x) *max_x = x;
        if (i == 0u || y < *min_y) *min_y = y;
        if (i == 0u || y > *max_y) *max_y = y;
    }
}

static int jersey_hud_anchor_for_2e(uint32_t command, int32_t *anchor) {
    int32_t min_x = 0;
    int32_t max_x = 0;
    int32_t min_y = 0;
    int32_t max_y = 0;
    int32_t width;
    int32_t height;

    /* Census: frontmost HUD quads are unlit, rectangular, and use these two
     * fixed rows. World packets share opcode 0x2E, but not this HUD contract. */
    if (psx_mod_read_word(command) != 0x2E808080u) return 0;
    uint32_t p0 = psx_mod_read_word(command + 4u);
    uint32_t p1 = psx_mod_read_word(command + 12u);
    uint32_t p2 = psx_mod_read_word(command + 20u);
    uint32_t p3 = psx_mod_read_word(command + 28u);
    if ((uint16_t)p0 != (uint16_t)p2 ||
        (uint16_t)p1 != (uint16_t)p3 ||
        (p0 >> 16) != (p1 >> 16) || (p2 >> 16) != (p3 >> 16))
        return 0;

    jersey_read_textured_quad_extent(command, &min_x, &max_x, &min_y, &max_y);
    width = max_x - min_x;
    height = max_y - min_y;

    if (width <= 0 || width > 96 || height <= 0 || height > 64)
        return 0;
    if (min_y != -100 && min_y != 68)
        return 0;

    if (min_x >= -280 && max_x <= -130) {
        *anchor = JERSEY_HUD_ANCHOR_LEFT;
        return 1;
    }
    if (min_x >= -80 && max_x <= 80) {
        *anchor = JERSEY_HUD_ANCHOR_CENTER;
        return 1;
    }
    if (min_x >= 130 && max_x <= 280) {
        *anchor = JERSEY_HUD_ANCHOR_RIGHT;
        return 1;
    }

    return 0;
}

#define JERSEY_SKY_PIECES 8u

typedef struct {
    uint32_t first_command;
    uint32_t count;
    uint32_t height;
    int invalid;
    int has_lower_clear;
    int32_t lower_y;
    int32_t left[JERSEY_SKY_PIECES];
    int32_t right[JERSEY_SKY_PIECES];
    uint32_t commands[JERSEY_SKY_PIECES];
    uint32_t draw_mode;
    uint32_t source_row;
    uint32_t source_phase;
    int have_draw_mode;
    int repeat_invalid;
} JerseySkyBand;

static void jersey_collect_sky_command(JerseySkyBand *band, uint32_t command,
                                        uint8_t op) {
    if (band->invalid) return;
    if (op == 0x00u || op == 0x01u) return;
    if (op == 0xE1u) {
        band->draw_mode = psx_mod_read_word(command);
        band->have_draw_mode = 1;
        return;
    }
    if (op != 0x60u && op != 0x64u) {
        band->invalid = 1;
        return;
    }
    uint32_t xy = psx_mod_read_word(command + 4u);
    int32_t x = (int16_t)xy;
    int32_t y = (int16_t)(xy >> 16);
    if (op == 0x60u && band->count == 0u && !band->has_lower_clear &&
        psx_mod_read_word(command) == 0x60000000u) {
        uint32_t size = psx_mod_read_word(command + 8u);
        if (x == -256 && (size & 0xFFFFu) == 512u &&
            y >= -120 && y <= 120 && y + (int32_t)(size >> 16) == 120) {
            band->has_lower_clear = 1;
            band->lower_y = y;
            return;
        }
    }
    if (op != 0x64u || psx_mod_read_word(command) != 0x64808080u) {
        band->invalid = 1;
        return;
    }
    uint32_t size = psx_mod_read_word(command + 12u);
    uint32_t width = size & 0xFFFFu;
    uint32_t height = size >> 16;
    if (y != -120 || x < -256 || width == 0u || width > 256u ||
        x + (int32_t)width > 256 || height == 0u || height > 240u ||
        band->count == JERSEY_SKY_PIECES ||
        (band->count && height != band->height)) {
        band->invalid = 1;
        return;
    }
    if (!band->count) band->first_command = command;
    /* The engine wraps four 128-texel pages. Check that all clipped pieces
     * belong to that same 512-texel ring before repeating the composite. */
    uint32_t uv = psx_mod_read_word(command + 8u);
    uint32_t u = uv & 0xFFu;
    uint32_t phase = ((band->draw_mode & 15u) * 128u + u - (uint32_t)x) & 511u;
    uint32_t row = (uv & 0xFFFFFF00u) | (band->draw_mode & 16u);
    if (!band->have_draw_mode || ((band->draw_mode >> 7) & 3u) != 1u ||
        u + width > 256u || (band->count &&
        (phase != band->source_phase || row != band->source_row)))
        band->repeat_invalid = 1;
    if (!band->count) {
        band->source_phase = phase;
        band->source_row = row;
    }
    band->height = height;
    band->commands[band->count] = command;
    band->left[band->count] = x;
    band->right[band->count++] = x + (int32_t)width;
}

static void jersey_tag_sky_band(const JerseySkyBand *band) {
    if (band->invalid || !band->count ||
        (band->has_lower_clear && band->lower_y != (int32_t)band->height - 120))
        return;
    /* Scrolling splits the sky into two or four pieces. Prove full interval
     * coverage, not just the bounding box, before clearing at the first draw. */
    int32_t covered = -256;
    for (uint32_t pass = 0; pass < band->count && covered < 256; ++pass) {
        int32_t next = covered;
        for (uint32_t i = 0; i < band->count; ++i)
            if (band->left[i] <= covered && band->right[i] > next)
                next = band->right[i];
        if (next == covered) return;
        covered = next;
    }
    if (covered == 256) {
        gpu_ws_tag_black_reveal_rect(band->first_command - 4u);
        if (!band->repeat_invalid)
            for (uint32_t i = 0; i < band->count; ++i)
                gpu_ws_tag_repeat_rect(band->commands[i] - 4u, 512);
    }
}

static void jersey_tag_packet_commands(uint32_t packet, uint32_t word_count,
                                       uint32_t rank, JerseySkyBand *band) {
    uint32_t word_index = 0;

    while (word_index < word_count) {
        uint32_t command = packet + 4u + word_index * 4u;
        uint8_t op = psx_mod_read_byte(command + 3u);
        int command_words = jersey_gp0_command_words(op);
        int32_t anchor = 0;

        if (command_words <= 0 ||
            word_index + (uint32_t)command_words > word_count) {
            if (rank == 0u) band->invalid = 1;
            return;
        }

        if (rank == JERSEY_HUD_OT_RANK && op == 0x2Eu && command_words == 9 &&
            jersey_hud_anchor_for_2e(command, &anchor))
            gpu_ws_tag_hud_prim(command - 4u, anchor);

        if (rank == 0u) jersey_collect_sky_command(band, command, op);

        word_index += (uint32_t)command_words;
    }
}

static void jersey_final_ot_entry(CPUState *cpu, uint32_t address) {
    uint32_t current;
    uint32_t rank = UINT32_MAX;
    uint32_t expected_load;
    JerseySkyBand sky = {0};

    if (!s_widescreen_enabled)
        return;
    switch (address) {
        case JERSEY_MAIN_DRAW_OT: expected_load = 0x90423792u; break;
        case JERSEY_PLAYER_DRAW_OT: expected_load = 0x90423262u; break;
        default: return;
    }
    /* MAIN and PLAYER occupy the same RAM range with different layouts. */
    if (psx_mod_read_word(address) != 0x3C028005u ||
        psx_mod_read_word(address + 4u) != expected_load ||
        psx_mod_read_word(address + 8u) != 0x27BDFFE8u)
        return;
    if (psx_mod_widescreen_x_margin() <= 0)
        return;
    if (psx_mod_display_width() != 512u)
        return;

    current = cpu->gpr[4];
    if (!jersey_is_main_ram(current))
        current = jersey_link_to_ram(current);
    if (!jersey_is_main_ram(current))
        return;

    for (uint32_t visited = 0; visited < JERSEY_OT_WALK_LIMIT; visited++) {
        uint32_t tag;
        uint32_t next;
        uint32_t word_count;

        if (!jersey_is_main_ram(current))
            break;

        tag = psx_mod_read_word(current);
        next = tag & 0x00FFFFFFu;
        word_count = tag >> 24;
        if (word_count > (JERSEY_MAIN_RAM_END - current - 4u) / 4u)
            return;
        if (word_count == 0u)
            rank = rank == UINT32_MAX ? 0u : rank + 1u;
        else if (rank == 0u || rank == JERSEY_HUD_OT_RANK)
            jersey_tag_packet_commands(current, word_count, rank, &sky);

        if (next == JERSEY_LINK_TERMINATOR) {
            jersey_tag_sky_band(&sky);
            break;
        }
        current = jersey_link_to_ram(next);
        if (current == 0u)
            break;
    }
}

static void jersey_widescreen_16_9_activate(void) {
    (void)psx_mod_set_fixed_display_aspect(16u, 9u);
    s_widescreen_enabled = 1;
}

static void jersey_widescreen_adaptive_activate(void) {
    (void)psx_mod_set_fixed_display_aspect(16u, 9u);
    (void)psx_mod_set_adaptive_display_aspect(32u, 9u);
    s_widescreen_enabled = 1;
}

PSX_MOD_CONSTRUCTOR(jersey_register_widescreen_plugins) {
    (void)psx_mod_register_activation_plugin(
        "jerseydevil.widescreen.16-9", jersey_widescreen_16_9_activate);
    (void)psx_mod_register_activation_plugin(
        "jerseydevil.widescreen.adaptive", jersey_widescreen_adaptive_activate);
    (void)psx_mod_register_function_entry_plugin(
        "jerseydevil.widescreen.final-ot", JERSEY_MAIN_DRAW_OT,
        jersey_final_ot_entry);
    (void)psx_mod_register_function_entry_plugin(
        "jerseydevil.widescreen.final-ot", JERSEY_PLAYER_DRAW_OT,
        jersey_final_ot_entry);
}
