#include "lib/light/struct.glsl"
#include "lib/fog/uniforms.glsl"

#if @skyBlending
#include "lib/core/fragment.h.glsl"

uniform float skyBlendingStart;
#endif

uniform float waterHeight;
uniform bool waterEnabled;
uniform bool waterSurface;
#ifndef OMW_DECL_IS_REFLECTION
#define OMW_DECL_IS_REFLECTION
uniform bool isReflection;
#endif
#ifndef OMW_DECL_VIEWMATRIXINVERSE
#define OMW_DECL_VIEWMATRIXINVERSE
uniform mat4 osg_ViewMatrixInverse;
#endif
#ifndef OMW_DECL_SUN
#define OMW_DECL_SUN
uniform DirectionalLight sun;
#endif

const vec3 WATER_COLOR = vec3(0.090195, 0.115685, 0.12745);

#ifdef MGE_FOG
// OpenMGE XE lighting: MGE XE fog & atmospheric scattering. Programs opt in
// by defining MGE_FOG before including this file (the scene programs that
// carried the old lighting library in 0.51: objects, terrain, groundcover,
// bs/default, water, sky). Everything else keeps the stock fog below.
// See mge_fog.glsl for the model and its provenance.
#include "compatibility/mge_fog.glsl"

// Sky-blend helpers carried over from the 0.51 port. The "stock tier"
// branches (mgeWeatherUniforms < 0.5) only matter for the shader-only
// Redux package; this engine always feeds the MGE uniforms.
// Mirrored passes skip the sky-blend epilogue on every tier: there the main camera's sky RTT would be read at the
// mirror's own fragcoords, a wrong-position paste that showed far land in reflections as pale ghost masses ("a
// reflection without a source"). The horizon seal in mge_fog.glsl widens in mirrored passes to cover the same
// window instead.

vec3 mgeStockRttDelta(vec3 dirWorld)
{
    float nice = mgeGetNiceWeather();
    if (nice < 0.001)
        return vec3(0.0);
    vec3 skyCol = mgeSampleSkyCol();
    return nice * (mgeScatter(dirWorld, 1.0, skyCol)
                 - mgeScatterWithSun(dirWorld, 1.0, skyCol, vec3(0.0, 0.0, 1.0)));
}

// Per-pixel sky sample behind this fragment (the sky-blending RTT holds
// the actual rendered sky incl. the cloud layer). a=0 when unavailable or
// in a reflection pass (the RTT belongs to the main camera; reflection
// fragcoords would sample wrong positions).
vec4 mgeSkyBehind(vec3 dirWorld)
{
#if @skyBlending
    // Stock exes: usable after the vertical-sun rebase (mgeStockRttDelta
    // above); mirrored/refraction passes keep the analytic fallback (the
    // main-camera RTT sample at their fragcoords is garbage). Gated with
    // the sky-blend toggle (same A/B unit).
    if (mgeWeatherUniforms < 0.5 && (mgeStockMirrored() || !mgeCamAboveWater() || isRefraction))
        return vec4(0.0);
    // Mirror detection needs both channels: isReflection binds per-program
    // and provably misses some of the reflection RTT's programs on stock,
    // where this main-camera RTT sample at the reflection's fragcoords is
    // garbage - camera-dependent neon tints on reflected silhouettes.
    if (!isReflection && !mgeStockMirrored())
    {
        // Wide horizontal average, not the raw pixel: any per-pixel image
        // is camera-locked, so its brightness unevenness reads as a static
        // screen-space fog mask on mid-saturated geometry, even with cloud
        // shapes weighted out. Averaging along the same screen row keeps
        // the sky's true brightness at that elevation (clouds included,
        // which is what fixes the cutouts) while flattening the pattern to
        // a smooth gradient.
        vec2 uv = gl_FragCoord.xy / screenRes;
        vec3 acc = vec3(0.0);
        for (int i = -3; i <= 3; ++i)
            acc += sampleSkyColor(vec2(clamp(uv.x + float(i) * 0.07, 0.02, 0.98), uv.y));
        acc /= 7.0;
        // Degenerate-sample guard (unbound sampler in this program, or an
        // interior/black RTT): report "unavailable" rather than converging
        // fog to black.
        if (dot(acc, acc) < 1e-6)
            return vec4(0.0);
        return vec4(acc, 1.0);
    }
#endif
    return vec4(0.0);
}

// Sky-blend epilogue gate, shared by the three applyFog* variants below.
// Returns the fadeValue to use and rebases skySample in place on stock.
// Full tier semantics unchanged (bit-identical): skip underwater/refraction.
// Stock tier: the blend is re-enabled with
// the vertical-sun rebase; skipped in mirrored passes, underwater, and
// where the RTT pixel is black (below the atmosphere cylinder's bottom
// edge, interiors) so far content is never pulled toward a black sample.
float mgeSkyBlendGate(inout vec3 skySample, vec3 dirWorld, float fadeValue)
{
    if (mgeWeatherUniforms < 0.5)
    {
        if (mgeStockMirrored() || !mgeCamAboveWater() || isRefraction
            || dot(skySample, skySample) < 1e-6)
            return 1.0;
        return fadeValue;
    }
    // main-camera sky RTT is positionally meaningless in a mirrored
    // pass, never blend toward it there. On Full this rides
    // isReflection (the reflection camera's stateset uniform,
    // water.cpp); mgeStockMirrored() covers the stock tier's known
    // per-program binding gaps (it returns false on Full by design).
    if (isReflection || mgeStockMirrored())
        return 1.0;
    if (!mgeCamAboveWater() || isRefraction)
        return 1.0;
    return fadeValue;
}

#endif // MGE_FOG

void computeFog(vec3 pos, float euclideanDist, float linearDist, float near, float far, out float colorScale, out vec3 colorOffset)
{
#if @radialFog
    float dist = euclideanDist;
#else
    float dist = abs(linearDist);
#endif

    bool cameraBelowWater = false;
    bool useWaterDepthFog = false;
    float underwaterFogFactor = 1.0;
    float waterDepth = 0.0;

    if (waterEnabled) {
        vec3 cameraPos = osg_ViewMatrixInverse[3].xyz;
        cameraBelowWater = cameraPos.z < waterHeight;
        if (!cameraBelowWater && !isReflection && !waterSurface) {
            vec3 worldPos = (osg_ViewMatrixInverse * vec4(pos, 1)).xyz;

            if (worldPos.z < waterHeight) {
                useWaterDepthFog = true;
                const float visibility = 2500.0;
                const float depthFade = 0.25;
                waterDepth = dist * clamp((waterHeight - worldPos.z) / (cameraPos.z - worldPos.z), 0.0, 1.0);
                float depthCorrection = sqrt(1.0 + 4.0 * depthFade * depthFade);
                underwaterFogFactor = depthFade * depthFade
                    / (-0.5 * depthCorrection + 0.5 - waterDepth / visibility) + 0.5 * depthCorrection + 0.5;
                underwaterFogFactor = clamp(underwaterFogFactor, 0.0, 1.0);
            }
        }
    }

#ifdef MGE_FOG
    // Above the water surface the MGE model replaces the stock distance fog:
    // transmittance * scene + inscatter, the inscatter coloured by
    // atmospheric scattering in nice weather (mgeFogColour, radial distance).
    // Underwater views, and draws with a fixed fog depth, keep the stock path.
    // So do scenes outside the main scene root (the inventory character preview
    // disables fog with fog.depth = -1 too): they have neither the weather
    // uniforms nor the once-per-frame verdict texture (mgeWeatherUniforms = 0).
    if (!cameraBelowWater && fog.depth < 0.0 && length(pos) > 0.0 && mgeWeatherUniforms > 0.5)
    {
        vec3 dirWorld = normalize((osg_ViewMatrixInverse * vec4(normalize(pos), 0.0)).xyz);
        // Geometry under the water seen from above takes the atmosphere only for the stretch of the view through
        // the air, as the stock branch below does (dist -= waterDepth); the water-depth murk covers the rest. The
        // whole distance turned rocks and seabed under the surface into bright fog-coloured shapes showing through
        // the water in dense weather.
        vec3 fogPos = pos;
        if (useWaterDepthFog && dist > 0.0)
            fogPos = pos * max(1.0 - waterDepth / dist, 0.001);
        vec4 f = mgeFogColour(fogPos, far, mgeSampleSkyCol(), mgeSkyBehind(dirWorld));
        colorScale = f.a;
#ifdef ADDITIVE_BLENDING
        colorOffset = vec3(0.0);
#else
        colorOffset = f.rgb;
#endif

        // Geometry below the water plane seen from above: the stock
        // water-depth murk first (scene * (1 - u) + murk * u), then the air
        // path on top of it (* transmittance + inscatter), matching the
        // order of the stock branch below.
        if (useWaterDepthFog) {
            colorScale *= 1.0 - underwaterFogFactor;
#ifndef ADDITIVE_BLENDING
            colorOffset += WATER_COLOR * length(sun.ambient.xyz) * underwaterFogFactor * f.a;
#endif
        }

#if @skyBlending
        float fadeValue = clamp((far - euclideanDist) / (far - skyBlendingStart), 0.0, 1.0);
        fadeValue *= fadeValue;
        vec3 skySample = sampleSkyColor(gl_FragCoord.xy / screenRes);
        // underwater/refraction, mirrored-pass and black-sample skips
        fadeValue = mgeSkyBlendGate(skySample, dirWorld, fadeValue);
        colorScale *= fadeValue;
#ifndef ADDITIVE_BLENDING
        colorOffset = colorOffset * fadeValue + skySample * (1.0 - fadeValue);
#endif
#endif
        return;
    }
#endif // MGE_FOG

    vec4 fogColor = cameraBelowWater ? fog.underwaterColor : fog.color;
    float start = cameraBelowWater ? fog.underwaterStart : fog.start;
    float end = cameraBelowWater ? fog.underwaterEnd : fog.end;

    if (fog.depth >= 0.0) {
        start = near * fog.depth + far * (1.0 - fog.depth);
        end = far;
        useWaterDepthFog = false;
    }

    if (useWaterDepthFog)
        dist -= waterDepth;

#if @exponentialFog
    float fogValue = 1.0 - exp(-2.0 * max(0.0, dist - start / 2.0) / (end - start / 2.0));
#else
    float fogValue = clamp((dist - start) * (1.0 / (end - start)), 0.0, 1.0);
#endif
    colorScale = 1.0 - fogValue;
#ifdef ADDITIVE_BLENDING
    colorOffset = vec3(0.0);
#else
    colorOffset = fogColor.xyz * fogValue;
#endif

    if (useWaterDepthFog) {
        colorScale *= 1.0 - underwaterFogFactor;
#ifndef ADDITIVE_BLENDING
        colorOffset += WATER_COLOR * length(sun.ambient.xyz) * underwaterFogFactor * (1.0 - fogValue);
#endif
    }

#if @skyBlending
    if (!cameraBelowWater && !isReflection) {
        float fadeValue = clamp((far - dist) / (far - skyBlendingStart), 0.0, 1.0);
        fadeValue *= fadeValue;
        colorScale *= fadeValue;
#ifndef ADDITIVE_BLENDING
        colorOffset = colorOffset * fadeValue + sampleSkyColor(gl_FragCoord.xy / screenRes) * (1.0 - fadeValue);
#endif
    }
#endif
}

vec4 applyFogAtDist(vec4 color, vec3 pos, float euclideanDist, float linearDist, float near, float far)
{
    float colorScale;
    vec3 colorOffset;
    computeFog(pos, euclideanDist, linearDist, near, far, colorScale, colorOffset);
    color.xyz = color.xyz * colorScale + colorOffset;
    return color;
}

vec4 applyFogAtPos(vec4 color, vec3 pos, float near, float far)
{
    return applyFogAtDist(color, pos, length(pos), pos.z, near, far);
}
