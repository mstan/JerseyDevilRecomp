#undef NDEBUG

#include "cpu_state.h"
#include "mod_plugins.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RAM_BASE 0x80000000u
#define RAM_SIZE (2u * 1024u * 1024u)
#define MAX_TAGS 16
#define MAX_ACTIVATION_REGS 8
#define MAX_FUNCTION_REGS 8

static uint8_t ram[RAM_SIZE];
static int32_t mock_margin;
static uint32_t mock_display_width = 512u;
static uint32_t fixed_aspect_num, fixed_aspect_den;
static uint32_t adaptive_aspect_num, adaptive_aspect_den;

typedef struct {
    uint32_t prim;
    int anchor;
} HudTag;

static HudTag hud_tags[MAX_TAGS];
static uint32_t hud_tag_count;
static uint32_t reveal_tag_count;
static uint32_t reveal_tag_prim;
static uint32_t repeat_tag_count;

typedef struct {
    const char *id;
    void (*callback)(void);
} ActivationReg;

typedef struct {
    const char *id;
    uint32_t address;
    void (*callback)(CPUState *cpu, uint32_t address);
} FunctionReg;

static ActivationReg activation_regs[MAX_ACTIVATION_REGS];
static FunctionReg function_regs[MAX_FUNCTION_REGS];
static uint32_t activation_reg_count;
static uint32_t function_reg_count;

static uint32_t phys(uint32_t address) {
    return address & 0x1FFFFFu;
}

static void reset_mock(void) {
    memset(ram, 0, sizeof(ram));
    mock_margin = 0;
    mock_display_width = 512u;
    fixed_aspect_num = fixed_aspect_den = 0;
    adaptive_aspect_num = adaptive_aspect_den = 0;
    hud_tag_count = 0;
    reveal_tag_count = 0;
    reveal_tag_prim = 0;
    repeat_tag_count = 0;
}

static void write_u32(uint32_t address, uint32_t value) {
    uint32_t p = phys(address);
    assert(p + 4u <= RAM_SIZE);
    ram[p + 0u] = (uint8_t)value;
    ram[p + 1u] = (uint8_t)(value >> 8);
    ram[p + 2u] = (uint8_t)(value >> 16);
    ram[p + 3u] = (uint8_t)(value >> 24);
}

static uint32_t read_u32(uint32_t address) {
    uint32_t p = phys(address);
    assert(p + 4u <= RAM_SIZE);
    return (uint32_t)ram[p + 0u] |
           ((uint32_t)ram[p + 1u] << 8) |
           ((uint32_t)ram[p + 2u] << 16) |
           ((uint32_t)ram[p + 3u] << 24);
}

static uint8_t read_u8(uint32_t address) {
    uint32_t p = phys(address);
    assert(p < RAM_SIZE);
    return ram[p];
}

uint8_t psx_mod_read_byte(uint32_t address) {
    return read_u8(address);
}

void psx_mod_write_byte(uint32_t address, uint8_t value) {
    uint32_t p = phys(address);
    assert(p < RAM_SIZE);
    ram[p] = value;
}

uint16_t psx_mod_read_half(uint32_t address) {
    return (uint16_t)read_u8(address) |
           ((uint16_t)read_u8(address + 1u) << 8);
}

void psx_mod_write_half(uint32_t address, uint16_t value) {
    psx_mod_write_byte(address, (uint8_t)value);
    psx_mod_write_byte(address + 1u, (uint8_t)(value >> 8));
}

uint32_t psx_mod_read_word(uint32_t address) {
    return read_u32(address);
}

void psx_mod_write_word(uint32_t address, uint32_t value) {
    write_u32(address, value);
}

void psx_mod_write_code_word(uint32_t address, uint32_t value) {
    write_u32(address, value);
}

uint32_t psx_mod_alloc_guest_memory(uint32_t size, uint32_t alignment) {
    (void)size;
    (void)alignment;
    return 0;
}

uint32_t psx_mod_alloc_gpu_dma_memory(uint32_t size, uint32_t alignment) {
    (void)size;
    (void)alignment;
    return 0;
}

int32_t psx_mod_widescreen_x_margin(void) {
    return mock_margin;
}

uint32_t psx_mod_display_width(void) {
    return mock_display_width;
}

uint32_t psx_mod_display_height(void) {
    return 240u;
}

int psx_mod_set_fixed_display_aspect(uint32_t numerator,
                                     uint32_t denominator) {
    fixed_aspect_num = numerator;
    fixed_aspect_den = denominator;
    return 1;
}

int psx_mod_set_adaptive_display_aspect(uint32_t max_numerator,
                                        uint32_t max_denominator) {
    adaptive_aspect_num = max_numerator;
    adaptive_aspect_den = max_denominator;
    return 1;
}

int psx_mod_register_activation_plugin(const char *id,
                                       PSXModActivationCallback callback) {
    assert(activation_reg_count < MAX_ACTIVATION_REGS);
    activation_regs[activation_reg_count].id = id;
    activation_regs[activation_reg_count].callback = callback;
    activation_reg_count++;
    return 1;
}

int psx_mod_register_vblank_plugin(const char *id,
                                   PSXModVBlankCallback callback) {
    (void)id;
    (void)callback;
    return 1;
}

int psx_mod_register_function_entry_plugin(
    const char *id, uint32_t address, PSXModFunctionEntryCallback callback) {
    assert(function_reg_count < MAX_FUNCTION_REGS);
    function_regs[function_reg_count].id = id;
    function_regs[function_reg_count].address = address;
    function_regs[function_reg_count].callback = callback;
    function_reg_count++;
    return 1;
}

void psx_mod_function_entry(CPUState *cpu, uint32_t address) {
    for (uint32_t i = 0; i < function_reg_count; i++) {
        if (function_regs[i].address == address)
            function_regs[i].callback(cpu, address);
    }
}

int psx_mod_game_started(void) {
    return 1;
}

int psx_mod_option_value(const char *package_id, const char *feature_id,
                         const char *option_id, char *out,
                         uint32_t out_size) {
    (void)package_id;
    (void)feature_id;
    (void)option_id;
    if (out_size)
        out[0] = '\0';
    return 0;
}

int psx_mod_current_resource_path(const char *resource_id, char *out,
                                  uint32_t out_size) {
    (void)resource_id;
    if (out_size)
        out[0] = '\0';
    return 0;
}

int psx_mod_set_native_vblank_rate(uint32_t frames_per_second) {
    (void)frames_per_second;
    return 1;
}

int psx_mod_set_frame_interpolation(uint32_t frames_per_second) {
    (void)frames_per_second;
    return 1;
}

int psx_mod_set_frame_interpolation_blend(uint32_t blend_mode) {
    (void)blend_mode;
    return 1;
}

int psx_mod_set_auto_skip_fmv(int enabled) {
    (void)enabled;
    return 1;
}

int psx_mod_set_bezel_artwork(const char *path) {
    (void)path;
    return 1;
}

int psx_mod_set_load_acceleration(uint32_t wall_clock_multiplier,
                                  uint32_t release_frames) {
    (void)wall_clock_multiplier;
    (void)release_frames;
    return 1;
}

int psx_mod_set_disc_speed(uint32_t divisor, uint32_t instant_max_per_frame) {
    (void)divisor;
    (void)instant_max_per_frame;
    return 1;
}

int psx_mod_set_controller_mode_override(uint32_t player,
                                         uint32_t controller_mode) {
    (void)player;
    (void)controller_mode;
    return 1;
}

int psx_mod_set_controller_presentation_policy(
    uint32_t player, PSXModControllerPresentationCallback callback,
    uint32_t initial_mode, int config_capable) {
    (void)player;
    (void)callback;
    (void)initial_mode;
    (void)config_capable;
    return 1;
}

void gpu_ws_tag_hud_prim(uint32_t prim, int anchor) {
    assert(hud_tag_count < MAX_TAGS);
    hud_tags[hud_tag_count].prim = prim;
    hud_tags[hud_tag_count].anchor = anchor;
    hud_tag_count++;
}

void gpu_ws_tag_black_reveal_rect(uint32_t prim) {
    reveal_tag_prim = prim;
    reveal_tag_count++;
}

void gpu_ws_tag_repeat_rect(uint32_t prim, int32_t period) {
    assert(prim >= 0x80011004u && prim <= 0x80011064u);
    assert(period == 512);
    ++repeat_tag_count;
}

#include "../src/mods/jersey_widescreen_plugin.c"

static void activate_fixed(void) {
    jersey_widescreen_16_9_activate();
}

static void activate_adaptive(void) {
    jersey_widescreen_adaptive_activate();
}

static uint32_t link_for(uint32_t address) {
    return address & 0x00FFFFFFu;
}

static void write_node(uint32_t address, uint32_t next, uint32_t word_count) {
    write_u32(address, (word_count << 24) | link_for(next));
}

static uint32_t vertex(int16_t x, int16_t y) {
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16);
}

static void write_textured_quad_2e(uint32_t command, int16_t min_x,
                                   int16_t min_y, int16_t max_x,
                                   int16_t max_y, uint32_t color) {
    write_u32(command + 0u, color);
    write_u32(command + 4u, vertex(min_x, min_y));
    write_u32(command + 8u, 0x00000000u);
    write_u32(command + 12u, vertex(max_x, min_y));
    write_u32(command + 16u, 0x00000000u);
    write_u32(command + 20u, vertex(min_x, max_y));
    write_u32(command + 24u, 0x00000000u);
    write_u32(command + 28u, vertex(max_x, max_y));
    write_u32(command + 32u, 0x00000000u);
}

static void build_final_rank_ot(uint32_t head, uint32_t packet) {
    for (uint32_t i = 0; i < 512u; i++) {
        uint32_t node = head + i * 4u;
        uint32_t next = (i == 511u) ? packet : node + 4u;
        write_node(node, next, 0u);
    }
    write_node(packet, RAM_BASE | JERSEY_LINK_TERMINATOR, 27u);
}

static void build_rank_zero_ot(uint32_t head, uint32_t packet) {
    write_node(head, packet, 0u);
    write_node(packet, RAM_BASE | JERSEY_LINK_TERMINATOR, 9u);
}

static void call_final_ot(uint32_t head) {
    CPUState cpu;
    write_u32(JERSEY_MAIN_DRAW_OT, 0x3C028005u);
    write_u32(JERSEY_MAIN_DRAW_OT + 4u, 0x90423792u);
    write_u32(JERSEY_MAIN_DRAW_OT + 8u, 0x27BDFFE8u);
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[4] = head;
    jersey_final_ot_entry(&cpu, JERSEY_MAIN_DRAW_OT);
}

static void test_gameplay_executable_hooks(void) {
    const uint32_t hooks[] = {JERSEY_MAIN_DRAW_OT, JERSEY_PLAYER_DRAW_OT};
    const uint32_t loads[] = {0x90423792u, 0x90423262u};
    for (unsigned i = 0; i < 2; ++i) {
        CPUState cpu = {0};
        reset_mock();
        activate_fixed();
        mock_margin = 93;
        build_final_rank_ot(0x80010000u, 0x80012000u);
        write_textured_quad_2e(0x80012004u, -246, -100, -214, -68, 0x2E808080u);
        cpu.gpr[4] = 0x10000u;
        jersey_final_ot_entry(&cpu, hooks[i]);
        assert(hud_tag_count == 0);
        write_u32(hooks[i], 0x3C028005u);
        write_u32(hooks[i] + 4u, loads[i]);
        write_u32(hooks[i] + 8u, 0x27BDFFE8u);
        jersey_final_ot_entry(&cpu, hooks[i]);
        assert(hud_tag_count == 1);
    }
}

static void snapshot_bytes(uint32_t address, uint8_t *out, uint32_t size) {
    for (uint32_t i = 0; i < size; i++)
        out[i] = read_u8(address + i);
}

static void test_disabled_identity(void) {
    reset_mock();
    mock_margin = 53;
    build_final_rank_ot(0x80010000u, 0x80011000u);
    write_textured_quad_2e(0x80011004u, -240, -100, -190, -80,
                           0x2E808080u);

    call_final_ot(0x80010000u);

    assert(hud_tag_count == 0);
    assert(fixed_aspect_num == 0 && adaptive_aspect_num == 0);
}

static void test_margin_zero_no_tags(void) {
    reset_mock();
    activate_fixed();
    mock_margin = 0;
    build_final_rank_ot(0x80010000u, 0x80011000u);
    write_textured_quad_2e(0x80011004u, -240, -100, -190, -80,
                           0x2E808080u);

    call_final_ot(0x80010000u);

    assert(hud_tag_count == 0);
    assert(fixed_aspect_num == 16 && fixed_aspect_den == 9);
}

static void test_valid_final_rank_groups_and_preserves_packets(void) {
    uint8_t before[27u * 4u];
    uint8_t after[27u * 4u];
    const uint32_t packet = 0x80012000u;

    reset_mock();
    activate_adaptive();
    mock_margin = 53;
    build_final_rank_ot(0x80010000u, packet);
    write_textured_quad_2e(packet + 4u, -240, -100, -190, -80,
                           0x2E808080u);
    write_textured_quad_2e(packet + 40u, -40, 68, 40, 92,
                           0x2E808080u);
    write_textured_quad_2e(packet + 76u, 190, -100, 240, -80,
                           0x2E808080u);
    snapshot_bytes(packet, before, sizeof(before));

    call_final_ot(0x80010000u);
    snapshot_bytes(packet, after, sizeof(after));

    assert(hud_tag_count == 3);
    assert(hud_tags[0].prim == packet && hud_tags[0].anchor == -1);
    assert(hud_tags[1].prim == packet + 36u && hud_tags[1].anchor == 0);
    assert(hud_tags[2].prim == packet + 72u && hud_tags[2].anchor == 1);
    assert(memcmp(before, after, sizeof(before)) == 0);
    assert(fixed_aspect_num == 16 && fixed_aspect_den == 9);
    assert(adaptive_aspect_num == 32 && adaptive_aspect_den == 9);
}

static void test_same_shape_non_hud_rank_no_tag(void) {
    reset_mock();
    activate_fixed();
    mock_margin = 53;
    build_rank_zero_ot(0x80010000u, 0x80011000u);
    write_textured_quad_2e(0x80011004u, -240, -100, -190, -80,
                           0x2E808080u);

    call_final_ot(0x80010000u);

    assert(hud_tag_count == 0);
}

static void test_perspective_world_shape_no_tag(void) {
    reset_mock();
    activate_fixed();
    mock_margin = 53;
    build_final_rank_ot(0x80010000u, 0x80011000u);
    write_textured_quad_2e(0x80011004u, -240, -100, -190, -80,
                           0x2E808080u);
    write_u32(0x80011010u, vertex(-180, -92));

    call_final_ot(0x80010000u);

    assert(hud_tag_count == 0);
}

static void test_malformed_packet_and_links_bounded(void) {
    reset_mock();
    activate_fixed();
    mock_margin = 53;

    write_node(0x80010000u, 0x80010004u, 0u);
    write_node(0x80010004u, 0x80010004u, 0u);
    call_final_ot(0x80010000u);
    assert(hud_tag_count == 0);

    reset_mock();
    activate_fixed();
    mock_margin = 53;
    write_node(0x80010002u, 0x80011000u, 0u);
    call_final_ot(0x80010002u);
    assert(hud_tag_count == 0);

    reset_mock();
    activate_fixed();
    mock_margin = 53;
    for (uint32_t i = 0; i < 512u; i++) {
        uint32_t node = 0x80010000u + i * 4u;
        write_node(node, i == 511u ? 0x80011000u : node + 4u, 0u);
    }
    write_node(0x80011000u, RAM_BASE | JERSEY_LINK_TERMINATOR, 8u);
    write_textured_quad_2e(0x80011004u, -240, -100, -190, -80,
                           0x2E808080u);
    call_final_ot(0x80010000u);
    assert(hud_tag_count == 0);
}

static void build_sky_band(unsigned phase, unsigned height) {
    const uint32_t packet = 0x80011000u;
    int32_t x[4] = {256 - (int32_t)phase, 0, -(int32_t)phase, -256};
    uint32_t width[4] = {phase, 256u - phase, phase, 256u - phase};
    unsigned count = 4;
    if (phase == 0) {
        x[0] = 0; x[1] = -256;
        width[0] = width[1] = 256;
        count = 2;
    }
    write_node(0x80010000u, 0x80010F00u, 0u);
    write_node(0x80010F00u, packet, 3u);
    write_u32(0x80010F04u, 0x60000000u);
    write_u32(0x80010F08u, vertex(-256, (int16_t)(height - 120)));
    write_u32(0x80010F0Cu, ((240u - height) << 16) | 512u);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t p = packet + i * 32u;
        write_node(p, i + 1 == count ? RAM_BASE | JERSEY_LINK_TERMINATOR : p + 32u, 5u);
        uint32_t page = phase == 0 ? (i == 0 ? 5u : 3u) :
                                    (i == 0 || i == 3 ? 3u : 5u);
        write_u32(p + 4u, 0xE1000280u | page);
        write_u32(p + 8u, 0x64808080u);
        write_u32(p + 12u, vertex((int16_t)x[i], -120));
        write_u32(p + 16u, 0x79800000u | (phase != 0 && (i & 1u) ? phase : 0u));
        write_u32(p + 20u, (height << 16) | width[i]);
    }
}

static void test_background_scrolling_coverage(void) {
    const unsigned heights[] = {1u, 124u, 180u, 240u};
    for (unsigned h = 0; h < sizeof(heights) / sizeof(heights[0]); ++h) {
        for (unsigned phase = 0; phase < 256; ++phase) {
            uint8_t before[512], after[512];
            reset_mock();
            activate_fixed();
            mock_margin = 93;
            build_sky_band(phase, heights[h]);
            snapshot_bytes(0x80010F00u, before, sizeof(before));
            call_final_ot(0x80010000u);
            snapshot_bytes(0x80010F00u, after, sizeof(after));
            assert(reveal_tag_count == 1 && hud_tag_count == 0);
            assert(repeat_tag_count == (phase == 0 ? 2u : 4u));
            assert(reveal_tag_prim == 0x80011004u);
            assert(memcmp(before, after, sizeof(before)) == 0);
        }
    }
}

static void test_background_reveal_clear_scope(void) {
    reset_mock();
    activate_fixed();
    mock_margin = 93;
    build_sky_band(67u, 180u);
    mock_margin = 0;
    call_final_ot(0x80010000u);
    assert(reveal_tag_count == 0);
    mock_margin = 93;
    const struct { uint32_t addr, value; } invalid[] = {
        {0x80011074u, 0x00B400BCu}, /* one-pixel gap despite full outer bounds */
        {0x80011014u, 0x00B40000u}, /* empty piece */
        {0x80011014u, 0x00B40101u}, /* width outside texture-strip contract */
        {0x80011014u, 0x00000043u}, /* zero height */
        {0x80011014u, 0x00F10043u}, /* excessive height */
        {0x80011014u, 0x007C0043u}, /* mixed heights */
        {0x8001100Cu, 0xFF8900BDu}, /* wrong row */
        {0x80011008u, 0x66808080u}, /* translucent primitive */
        {0x80011008u, 0x64000000u}, /* different modulation */
        {0x80011004u, 0xE3000000u}, /* target change within band */
        {0x80010F08u, 0x003BFF00u}, /* lower clear does not meet sky */
        {0x80011000u, 0x05011000u}, /* cycle */
        {0x80011000u, 0x05FFFFFFu}, /* incomplete band */
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        build_sky_band(67u, 180u);
        write_u32(invalid[i].addr, invalid[i].value);
        call_final_ot(0x80010000u);
        assert(reveal_tag_count == 0);
    }
    build_sky_band(67u, 180u);
    JerseySkyBand band = {0};
    jersey_tag_packet_commands(0x80011000u, 5u, 511u, &band);
    jersey_tag_sky_band(&band);
    assert(reveal_tag_count == 0);
}

static void test_background_independent_half_splits(void) {
    reset_mock();
    activate_fixed();
    mock_margin = 228;
    build_sky_band(113u, 180u);
    /* Moving-stage census: right widths 113/143, left widths 241/15. */
    write_u32(0x8001104Cu, vertex(-241, -120));
    write_u32(0x80011054u, 0x00B400F1u);
    write_u32(0x80011074u, 0x00B4000Fu);
    write_u32(0x80011004u, 0xE1000286u);
    write_u32(0x80011024u, 0xE1000284u);
    write_u32(0x80011044u, 0xE1000283u);
    write_u32(0x80011064u, 0xE1000286u);
    call_final_ot(0x80010000u);
    assert(reveal_tag_count == 1 && reveal_tag_prim == 0x80011004u);
    assert(repeat_tag_count == 4);
    repeat_tag_count = 0;
    write_u32(0x80011024u, 0xE1000285u);
    call_final_ot(0x80010000u);
    assert(repeat_tag_count == 0); /* mixed source phase: clear but do not repeat */
}

int main(void) {
    assert(activation_reg_count >= 2);
    assert(function_reg_count >= 1);

    test_disabled_identity();
    test_margin_zero_no_tags();
    test_valid_final_rank_groups_and_preserves_packets();
    test_same_shape_non_hud_rank_no_tag();
    test_perspective_world_shape_no_tag();
    test_malformed_packet_and_links_bounded();
    test_gameplay_executable_hooks();
    test_background_reveal_clear_scope();
    test_background_scrolling_coverage();
    test_background_independent_half_splits();

    puts("test_jersey_widescreen: PASS");
    return 0;
}
