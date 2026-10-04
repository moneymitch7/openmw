#ifndef LIB_FOG_UNIFORMS
#define LIB_FOG_UNIFORMS

// Fog state fed by the engine (SceneUtil::StateUpdater). Shared by
// compatibility/fog.glsl and any vertex stage that needs the fog ranges
// (the OpenMGE XE weather model runs per vertex). The struct must stay
// identical in every stage of a program for the uniform to link.
struct Fog {
    vec4 color;
    vec4 underwaterColor;
    float start;
    float underwaterStart;
    float end;
    float underwaterEnd;
    float depth;
};

uniform Fog fog;

#endif
