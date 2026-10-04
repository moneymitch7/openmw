#version 120

// OpenMGE XE: once-per-frame weather verdict pass (see the top of
// mge_fog.glsl). Each texel of an 8-wide row is one vec4 of the verdict the
// consumer vertex stages read; the engine draws two rows, 0 with the scene
// flavour of the model and 1 with water's full core (@mgeWxWater).
#define MGE_WX_PASS 1
#if @mgeWxWater
#define WX_NEED_FULL_CORE 1
#define MGE_WX_V4 1
#define MGE_WX_RESCUE 1
#endif

#include "compatibility/mge_fog.glsl"

void main()
{
    gl_FragData[0] = mgeWxVerdict(int(gl_FragCoord.x));
}
