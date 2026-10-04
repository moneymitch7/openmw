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
#define MGE_STOCK_SKY_BLEND 1
#define MGE_STOCK_RTT_REBASE 0
// MGE_SKY_BLEND_MIRROR_GATE: skip the sky-blend epilogue in mirrored
//   passes on every tier (the reported "reflection without a
//   source"). The Full-tier tail of
//   mgeSkyBlendGate ran in the water reflection pass, where
//   sampleSkyColor reads the main camera's sky RTT at the mirror's own
//   fragcoords, a wrong-position paste. Beyond skyBlendingStart the
//   fadeValue tends to 0 and the fragment becomes that sample, so far
//   mirrored distant land (fully concealed in the direct view by the
//   correctly-positioned blend) rendered as pale ghost masses in the
//   reflection. The stock branch (mgeStockMirrored) and mgeSkyBehind
//   always carried this skip; the Full tail lacked it. Pairs with
//   MGE_MIRROR_SEAL_WIDEN in mge_fog.glsl (the concealment-parity
//   half). 0 restores the paste byte-exact for A/B.
#ifndef MGE_SKY_BLEND_MIRROR_GATE
#define MGE_SKY_BLEND_MIRROR_GATE 1
#endif

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
#if MGE_STOCK_SKY_BLEND
    if (mgeWeatherUniforms < 0.5 && (mgeStockMirrored() || !mgeCamAboveWater() || isRefraction))
        return vec4(0.0);
#else
    if (mgeWeatherUniforms < 0.5)
        return vec4(0.0);
#endif
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
#ifndef MGE_CORRIDOR_SKYROW
// the water-sky line during corridors and did not move the bank,
// rolled back same-day; the row-direction/row-choice questions are the
// next session's opening investigation. The code stays for it.
#define MGE_CORRIDOR_SKYROW 0
#endif
#if MGE_ENDPOINT_DECOMPOSITION && MGE_CORRIDOR_SKYROW
        // fogged far land converges to this sample, the RTT rows behind
        // it, which during storm arrivals carry the horizon glow:
        // measured brighter/paler than the visible sky above the ridge
        // line (measured profile: hidden rows 0.636-0.643 lum against
        // 0.626-0.631 visible; the reported pale-bright early bank).
        // The eye compares silhouettes
        // against the sky above them, so mid-corridor the target blends
        // toward a sample 0.06 uv higher (the C8/147 one-step lesson).
        // steady keeps the exact behind-row (weight 0 at i==j), that is
        // the seamless-horizon invariant's own mechanism. Contraction
        // toward a measured visible value: cannot recreate the
        // white-wall class (which was over-bright vs everything).
        // Weight: the hue family's 4x fade, conf-gated; nice<->nice
        // gated with the rest of the family. Both tiers (the corridor
        // unlock feeds conf on Full; tier consistency).
        {
            int rwI;
            int rwJ;
            float rwA;
            float rwH;
            float rwC = mgeDecomposeIdx(rwI, rwJ, rwA, rwH);
            float rwT = (rwI == rwJ) ? 0.0 : 4.0 * rwA * (1.0 - rwA);
#if MGE_WXT_NICE_GATE
            rwT *= mgeWxtNiceGate();
#endif
            float rwW = rwC * min(1.0, 4.0 * rwT);
            if (rwW > 0.001)
            {
                vec3 accUp = vec3(0.0);
                for (int i = -3; i <= 3; ++i)
                    accUp += sampleSkyColor(vec2(
                        clamp(uv.x + float(i) * 0.07, 0.02, 0.98),
                        min(uv.y + 0.06, 0.98)));
                accUp /= 7.0;
                if (dot(accUp, accUp) > 1e-6)
                    acc = mix(acc, accUp, rwW);
            }
        }
#endif
#if MGE_STOCK_RTT_REBASE
        if (mgeWeatherUniforms < 0.5)
            acc += mgeStockRttDelta(dirWorld); // vertical-sun rebase (refuted; off)
#endif
        return vec4(acc, 1.0);
    }
#endif
    return vec4(0.0);
}

// Sky-blend epilogue gate, shared by the three applyFog* variants below.
// Returns the fadeValue to use and rebases skySample in place on stock.
// Full tier semantics unchanged (bit-identical): skip underwater/refraction.
// Stock tier (behind MGE_STOCK_SKY_BLEND): the blend is re-enabled with
// the vertical-sun rebase; skipped in mirrored passes, underwater, and
// where the RTT pixel is black (below the atmosphere cylinder's bottom
// edge, interiors) so far content is never pulled toward a black sample.
float mgeSkyBlendGate(inout vec3 skySample, vec3 dirWorld, float fadeValue)
{
    if (mgeWeatherUniforms < 0.5)
    {
#if MGE_STOCK_SKY_BLEND
        if (mgeStockMirrored() || !mgeCamAboveWater() || isRefraction
            || dot(skySample, skySample) < 1e-6)
            return 1.0;
#if MGE_STOCK_RTT_REBASE
        skySample += mgeStockRttDelta(dirWorld);
#endif
        // (MGE_STOCK_SKY_BLEND_FADE).
        return fadeValue;
#else
        return 1.0;
#endif
    }
#if MGE_SKY_BLEND_MIRROR_GATE
    // main-camera sky RTT is positionally meaningless in a mirrored
    // pass, never blend toward it there. On Full this rides
    // isReflection (the reflection camera's stateset uniform,
    // water.cpp); mgeStockMirrored() covers the stock tier's known
    // per-program binding gaps (it returns false on Full by design).
    if (isReflection || mgeStockMirrored())
        return 1.0;
#endif
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
        vec4 f = mgeFogColour(pos, far, mgeSampleSkyCol(), mgeSkyBehind(dirWorld));
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
