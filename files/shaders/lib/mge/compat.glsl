#ifndef LIB_MGE_COMPAT
#define LIB_MGE_COMPAT

// OpenMGE XE lighting: compatibility layer for OpenMW 0.52.
//
// The MGE XE shader port (compatibility/mge_fog.glsl) was written against
// OpenMW 0.51, where scene programs read the sun as light 0 of the old
// lighting library (lcalcPosition(0), lcalcDiffuse(0), lcalcSpecular(0))
// and the fog state through the legacy gl_Fog built-in. OpenMW 0.52
// replaced both:
//   * the sun is the `sun` DirectionalLight uniform: position is view
//     space (same as the old light 0), diffuse/specular unchanged, and
//     specular.a is still specular strength * sun visibility;
//   * fog is the `fog` struct uniform, carrying above-water and underwater
//     ranges side by side instead of the viewer-medium pair in gl_Fog.
// The accessors below rebuild the 0.51 values on top of the new uniforms,
// so the MGE model keeps its original semantics unchanged.

#include "lib/light/struct.glsl"
#include "lib/fog/uniforms.glsl"

#ifndef OMW_DECL_SUN
#define OMW_DECL_SUN
uniform DirectionalLight sun;
#endif

// True when the main viewer is underwater; set per frame on the root
// stateset (SharedUniformStateUpdater). 0.51 switched gl_Fog with the same
// state, so it selects which pair of fog ranges the legacy view returns.
#ifndef OMW_DECL_VIEWER_UNDERWATER
#define OMW_DECL_VIEWER_UNDERWATER
uniform bool viewerUnderwater;
#endif

struct MgeLegacyFog
{
    vec4 color;
    float start;
    float end;
    float scale;
};

// gl_Fog as OpenMW 0.51 fed it: the viewer medium's colour and range.
MgeLegacyFog mgeLegacyFog()
{
    MgeLegacyFog f;
    f.color = viewerUnderwater ? fog.underwaterColor : fog.color;
    f.start = viewerUnderwater ? fog.underwaterStart : fog.start;
    f.end = viewerUnderwater ? fog.underwaterEnd : fog.end;
    f.scale = 1.0 / max(f.end - f.start, 0.0001);
    return f;
}

// Light 0 of the 0.51 lighting library (the sun), view space.
vec3 mgeSunViewPos()
{
    return sun.position.xyz;
}

vec3 mgeSunDiffuse()
{
    return sun.diffuse.xyz;
}

vec4 mgeSunSpecular()
{
    return sun.specular;
}

// MGE XE-style per-object tonemap: polynomial maps [0, 2.2] -> [0, 1].
vec3 perObjectTonemap(vec3 c)
{
    c = clamp(c, 0.0, 2.2);
    return (((0.0548303 * c - 0.189786) * c - 0.154732) * c + 1.12969) * c;
}

// Sun shadow strength set by the engine each frame (SharedUniformStateUpdater): fades shadows out at night when
// [Shadows] night shadows is off.
uniform float sunShadowFade;

// User option: the cloud-cover shadow fade. 0.25 = MGE XE behaviour
// (cloud cover weakens shadows: overcast shadows run ~half of vanilla
// OpenMW's depth), 1.0 = no fade (shadows keep near-vanilla strength in
// every weather). Keep the define on one line.
#define MGE_CLOUD_SHADOW_FADE_FLOOR 0.25

// MGE XE shadow receiver, ported from "XE Mod Shadow.fx" (MGE XE 0.16.0).
// shade (0.4) is the half-saturation constant of the saturating curve
// x/(shade+x) on incoming sun luminance. The result multiplies the final
// colour, ambient included (XE Main.fx: SrcBlend=Zero,
// DestBlend=InvSrcColor). Cloud cover enters inside x
// (x *= 0.25 + 0.75*sunVis), weakening overcast shadows along the same
// curve. Applied before fog, so fog is never shadowed. Callers keep the
// sun at full strength in their lighting and multiply by this instead.
vec3 mgeShadowMult(float shadowing, vec3 viewNormal)
{
    vec3 sunPos = mgeSunViewPos();
    // No usable sun in this pass (zero vector): leave the colour alone
    // rather than normalising a zero vector into NaN.
    if (dot(sunPos, sunPos) < 1e-12)
        return vec3(1.0);
    float lambert = clamp(dot(viewNormal, normalize(sunPos)), 0.0, 1.0);
    float x = lambert * dot(mgeSunDiffuse(), vec3(0.36, 0.53, 0.11));
    x *= MGE_CLOUD_SHADOW_FADE_FLOOR
        + (1.0 - MGE_CLOUD_SHADOW_FADE_FLOOR) * clamp(mgeSunSpecular().a, 0.0, 1.0);
    float light = x / (0.4 + x) * sunShadowFade;
    return vec3(1.0) - (1.0 - shadowing) * light * vec3(1.0, 0.97, 0.81);
}

#endif
