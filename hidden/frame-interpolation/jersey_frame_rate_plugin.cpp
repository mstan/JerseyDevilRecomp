#include "render_pass_replay.hpp"
#include "gpu.h"

/* MAIN's three gameplay loops build a ring of OTs. The VBlank callback
 * 3498C submits a completed OT and flips through 485F0. Replay the world
 * section before its next wait, into the currently shown rectangle (SHOWN),
 * rather than attempting to run a pass inside that interrupt callback. */
namespace {
struct Section {uint32_t first,word,last,return_pc,sdk_delta;};
constexpr Section Sections[]={
    {0x8001391C,0x3C04800A,0x800139AC,0x800139B4,0},
    {0x80013F04,0x3C04800A,0x80013F8C,0x80013F9C,0},
    {0x8001454C,0x3C04800A,0x800145D4,0x800145E4,0},
    // PLAYER.EXE relocates both the loops and their SDK/data bank.
    {0x80013914,0x3C04800A,0x800139A4,0x800139AC,0x530},
    {0x80013E28,0x3C04800A,0x80013EB0,0x80013EC0,0x530},
    {0x8001446C,0x3C04800A,0x800144F4,0x80014504,0x530},
};
PSXDrawReplay replay;
uint32_t selected=0,buffer_index=0,vblanks=0;
PSXModRenderPassFrame frame{};
void tick(){if(!g_psx_render_pass_active)++vblanks;}
void begin(CPUState* cpu,uint32_t pc){
    if(g_psx_render_pass_active)return;
    for(unsigned i=0;i<6;++i)if(pc==Sections[i].first){
        const uint32_t sdk=0x80045770-Sections[i].sdk_delta;
        if(psx_mod_read_word(sdk)!=0x3C028005 ||
           psx_mod_read_word(sdk+4)!=(Sections[i].sdk_delta ? 0x90423262 : 0x90423792))return;
        if(selected!=i)replay.invalidate(); selected=i;
        buffer_index=psx_mod_read_half(0x80053DF6-Sections[i].sdk_delta);
        if(buffer_index>=3){replay.invalidate();return;}
        replay.capture(cpu,Sections[i].first,Sections[i].last);return;
    }
}
int pass(CPUState* cpu,void*,uint32_t alpha){
    if(!replay.restore(cpu,alpha))return 0;
    // The game's shared DRAWENV is prepared by 48570 for real submissions.
    // Retarget its clip and offset to the rectangle the host pass owns.
    const uint32_t delta=Sections[selected].sdk_delta, env=0x80079870-delta;
    psx_mod_write_half(env,frame.x);psx_mod_write_half(env+2,frame.y);
    psx_mod_write_half(env+4,frame.w);psx_mod_write_half(env+6,frame.h);
    psx_mod_write_half(env+8,frame.x);psx_mod_write_half(env+10,frame.y);
    psx_mod_write_byte(env+24,1);
    PSXDrawReplay::call(cpu,0x800457E0-delta,env);
    // PutDrawEnv writes its command buffer into the DRAWENV itself. Build and
    // submit it before replay allocates world packets: subdivision can extend
    // those packets into the same memory in the sandbox.
    if(!replay.draw(cpu)){psx_mod_counter_add("jersey.fr.draw_failed",1);return 0;}
    const uint32_t tail=psx_mod_read_word(0x800682D0-delta+20*buffer_index+0x10);
    PSXDrawReplay::call(cpu,0x80045770-delta,tail);
    PSXDrawReplay::call(cpu,0x800450E4-delta,0);
    psx_mod_counter_add("jersey.fr.interpolated_projections",replay.stats.replayed);
    return 1;
}
void finish(CPUState* cpu,uint32_t){
    if(g_psx_render_pass_active || cpu->gpr[31]!=Sections[selected].return_pc)return;
    if(psx_mod_read_half(0x80053DF6-Sections[selected].sdk_delta)!=buffer_index){replay.invalidate();return;}
    const bool ready=replay.prepare(vblanks);
    psx_mod_counter_add("jersey.fr.frames",1);
    psx_mod_counter_add("jersey.fr.projections",replay.stats.captured);
    psx_mod_counter_add("jersey.fr.matched",replay.stats.matched);
    psx_mod_counter_add("jersey.fr.moving",replay.stats.changed);
    if(!ready)return;
    GpuDisplayInfo display{};gpu_get_display_info(&display);
    if(display.disabled || display.depth24 || !display.width || !display.height)return;
    frame={};frame.struct_size=sizeof frame;frame.period_vblanks=replay.ticks;
    frame.x=display.display_x;frame.y=display.display_y;frame.w=display.width;frame.h=display.height;
    psx_mod_counter_add("jersey.fr.passes",psx_mod_render_pass_frame(cpu,&frame,pass,nullptr));
}
void activate(unsigned fps){replay.invalidate();vblanks=0;PSXDrawReplay::rate(fps,PSX_MOD_RENDER_PASS_FLIP_SHOWN);}
void display(){activate(0);}void rate60(){activate(60);}void rate90(){activate(90);}
void rate120(){activate(120);}void rate144(){activate(144);}void rate165(){activate(165);}void rate240(){activate(240);}
}
PSX_MOD_CONSTRUCTOR(jersey_register_frame_rate_plugins){
    const char* ids[]={"jerseydevil.framerate.display","jerseydevil.framerate.60","jerseydevil.framerate.90",
        "jerseydevil.framerate.120","jerseydevil.framerate.144","jerseydevil.framerate.165","jerseydevil.framerate.240"};
    void(*callbacks[])()={display,rate60,rate90,rate120,rate144,rate165,rate240};
    for(unsigned i=0;i<7;++i){
        psx_mod_register_activation_plugin(ids[i],callbacks[i]);psx_mod_register_vblank_plugin(ids[i],tick);
        for(const auto& s:Sections)psx_mod_register_instruction_plugin(ids[i],s.first,s.word,begin);
        psx_mod_register_function_entry_plugin(ids[i],0x8003438C,finish);
        psx_mod_register_function_entry_plugin(ids[i],0x8003FCCC,finish);
        psx_mod_register_function_entry_plugin(ids[i],0x800341F0,finish);
        psx_mod_register_function_entry_plugin(ids[i],0x8003F79C,finish);
    }
}
