#undef NDEBUG
#include "mod_plugins.h"
#include "cpu_state.h"
#include "gpu.h"
#include "render_pass_projection.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static const char* ids[7];
static void(*callbacks[7])();
static unsigned count, calls, selected_rate, selected_blend, selected_source, selected_flip;
static unsigned entry_hooks, instruction_hooks, tick_hooks;
static uint8_t test_ram[0x200000], test_scratch[1024];
static bool exercise_draw_order;
extern "C" {
int g_psx_render_pass_active=0;
int(*g_psx_projection_command)(CPUState*,uint32_t)=nullptr;
int psx_mod_register_activation_plugin(const char* id,void(*callback)()) {
    assert(count<7);ids[count]=id;callbacks[count++]=callback;return 1;
}
int psx_mod_register_vblank_plugin(const char*,PSXModVBlankCallback){++tick_hooks;return 1;}
int psx_mod_register_function_entry_plugin(const char*,uint32_t,PSXModFunctionEntryCallback){++entry_hooks;return 1;}
int psx_mod_register_instruction_plugin(const char*,uint32_t,uint32_t,PSXModFunctionEntryCallback){++instruction_hooks;return 1;}
int psx_mod_set_frame_interpolation_source(uint32_t v){assert(calls++==0);selected_source=v;return 1;}
int psx_mod_set_render_pass_flip(uint32_t v){assert(calls++==1);selected_flip=v;return 1;}
int psx_mod_set_frame_interpolation_blend(uint32_t v){assert(calls++==2);selected_blend=v;return 1;}
int psx_mod_set_frame_interpolation(uint32_t v){assert(calls++==3);selected_rate=v;return 1;}
uint8_t* memory_get_ram_ptr(){return test_ram;}
uint8_t* memory_get_scratchpad_ptr(){return test_scratch;}
uint32_t memory_get_ram_bytes(){return sizeof test_ram;}
uint32_t psx_mod_read_word(uint32_t a){uint32_t v;std::memcpy(&v,test_ram+(a&0x1FFFFF),4);return v;}
uint16_t psx_mod_read_half(uint32_t a){uint16_t v;std::memcpy(&v,test_ram+(a&0x1FFFFF),2);return v;}
void psx_mod_write_word(uint32_t a,uint32_t v){std::memcpy(test_ram+(a&0x1FFFFF),&v,4);}
void psx_mod_write_half(uint32_t a,uint16_t v){std::memcpy(test_ram+(a&0x1FFFFF),&v,2);}
void psx_mod_write_byte(uint32_t a,uint8_t v){test_ram[a&0x1FFFFF]=v;}
void pgxp_invalidate_all(){}
void psx_mod_counter_add(const char*,uint32_t){}
uint32_t psx_mod_render_pass_frame(CPUState*,const PSXModRenderPassFrame*,PSXModRenderPassFn,void*){return 0;}
int psx_mod_run_guest_span(CPUState* cpu,uint32_t,uint32_t){
    if(!exercise_draw_order)return 0;
    // A replay packet uses the same address as the SDK DRAWENV command area.
    psx_mod_write_word(0x8007989C,0x2C403633);
    assert(psx_projection_command(cpu,0x0180001));
    return 1;
}
void psx_dispatch_call(CPUState*,uint32_t function,uint32_t){
    assert(exercise_draw_order);
    if(function==0x800457E0)psx_mod_write_word(0x8007989C,0xE1000B3F);
    else if(function==0x80045770)assert(psx_mod_read_word(0x8007989C)==0x2C403633);
    else assert(function==0x800450E4);
}
void gpu_get_display_info(GpuDisplayInfo* out){*out={};}
}
#include "../src/mods/jersey_frame_rate_plugin.cpp"

static CPUState projection_cpu(int x=0,uint32_t vertex=0x00100020) {
    CPUState cpu{};cpu.gpr[31]=0x80012348;
    cpu.gte_ctrl[0]=4096;cpu.gte_ctrl[2]=4096;cpu.gte_ctrl[4]=4096;
    cpu.gte_ctrl[5]=uint32_t(x);cpu.gte_ctrl[7]=1000;
    cpu.gte_data[0]=vertex;cpu.gte_data[1]=30;return cpu;
}
static void capture_one(PSXProjectionHistory* h,CPUState cpu,unsigned ticks=2) {
    psx_projection_capture_begin(h);assert(g_psx_projection_command);
    assert(!psx_projection_command(&cpu,0x0180001));
    psx_projection_capture_end(h,ticks,100);assert(!g_psx_projection_command);
}
int main(){
    assert(count==7 && tick_hooks==7 && entry_hooks==28 && instruction_hooks==42);
    const char* suffixes[]={"display","60","90","120","144","165","240"};
    const unsigned rates[]={0,60,90,120,144,165,240};
    for(unsigned i=0;i<7;++i){
        char expected[64];std::snprintf(expected,sizeof expected,"jerseydevil.framerate.%s",suffixes[i]);
        assert(std::strcmp(ids[i],expected)==0);calls=0;callbacks[i]();
        assert(calls==4 && selected_rate==rates[i]);
        assert(selected_source==PSX_MOD_FRAME_SOURCE_FLIP && selected_flip==PSX_MOD_RENDER_PASS_FLIP_SHOWN);
        assert(selected_blend==PSX_MOD_FRAME_INTERPOLATION_HOLD);
    }
    auto* h=psx_projection_create(4);assert(h);
    capture_one(h,projection_cpu(-20));capture_one(h,projection_cpu(20));
    PSXProjectionStats st{};psx_projection_stats(h,&st);assert(st.matched==1 && st.changed==1);
    g_psx_render_pass_active=1;
    psx_projection_replay_begin(h,32768);auto cpu=projection_cpu(20);
    assert(psx_projection_command(&cpu,0x0180001) && cpu.gte_ctrl[5]==0); // actual halfway position
    psx_projection_replay_end(h);
    psx_projection_replay_begin(h,16384);cpu=projection_cpu(20);
    assert(psx_projection_command(&cpu,0x0180001) && int32_t(cpu.gte_ctrl[5])==-10);
    psx_projection_replay_end(h);
    // Different vertices/callers never borrow the matching ordinal's motion.
    psx_projection_replay_begin(h,32768);cpu=projection_cpu(20,0x00400050);
    assert(!psx_projection_command(&cpu,0x0180001) && cpu.gte_ctrl[5]==20);
    cpu=projection_cpu(20);cpu.gpr[31]+=4;assert(!psx_projection_command(&cpu,0x0180001));
    psx_projection_replay_end(h);g_psx_render_pass_active=0;
    capture_one(h,projection_cpu(5000));psx_projection_stats(h,&st);assert(st.discontinuities==1 && !st.matched);
    capture_one(h,projection_cpu(5010),20);psx_projection_stats(h,&st);assert(!st.matched);
    psx_projection_invalidate(h);capture_one(h,projection_cpu(10));psx_projection_stats(h,&st);assert(!st.matched);
    // RTPT identity includes all three vertices, not just V0.
    psx_projection_invalidate(h);psx_projection_capture_begin(h);cpu=projection_cpu();cpu.gte_data[4]=123;
    psx_projection_command(&cpu,0x0280030);psx_projection_capture_end(h,2,100);
    psx_projection_capture_begin(h);cpu.gte_data[4]=456;psx_projection_command(&cpu,0x0280030);
    psx_projection_capture_end(h,2,100);psx_projection_stats(h,&st);assert(!st.matched);
    // A capacity overrun poisons history rather than reusing a partial trace.
    psx_projection_capture_begin(h);for(unsigned i=0;i<5;++i)psx_projection_command(&cpu,0x0280030);
    psx_projection_capture_end(h,2,100);psx_projection_stats(h,&st);assert(st.overflow && !st.matched);
    // Watchdog cleanup must not modify the following real frame.
    g_psx_render_pass_active=1;psx_projection_replay_begin(h,32768);g_psx_render_pass_active=0;
    assert(!psx_projection_command(&cpu,0x0280030) && !g_psx_projection_command);
    psx_projection_destroy(h);
    // SDK command generation must precede packet allocation, or it overwrites
    // a world packet with E1/E2/environment words before DrawOTag reads it.
    selected=0;buffer_index=0;exercise_draw_order=true;replay.invalidate();
    cpu=projection_cpu(-20);replay.capture(&cpu,Sections[0].first,Sections[0].last);
    psx_projection_command(&cpu,0x0180001);assert(!replay.prepare(2));
    cpu=projection_cpu(20);replay.capture(&cpu,Sections[0].first,Sections[0].last);
    psx_projection_command(&cpu,0x0180001);assert(replay.prepare(4));
    g_psx_render_pass_active=1;frame.x=512;frame.w=512;frame.h=240;
    assert(pass(&cpu,nullptr,32768));g_psx_render_pass_active=0;
    exercise_draw_order=false;
    puts("Native frame rate activation, projection identity, midpoint, cut and watchdog guards: PASS");
}
