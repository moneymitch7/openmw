#define SHADOWS @shadows_enabled

#if SHADOWS

#ifndef PER_PIXEL_LIGHTING
#define PER_PIXEL_LIGHTING 0
#endif

    uniform float maximumShadowMapDistance;
    uniform float shadowFadeStart;
    @foreach shadow_texture_unit_index @shadow_texture_unit_list
        uniform sampler2DShadow shadowTexture@shadow_texture_unit_index;
        varying vec4 shadowSpaceCoords@shadow_texture_unit_index;

#if @perspectiveShadowMaps
        varying vec4 shadowRegionCoords@shadow_texture_unit_index;
#endif
    @endforeach

// ============================================================================
// Soft Shadow Filtering (8-tap spiral disc, receiver plane depth)
// ============================================================================
#if @softShadows

// Filter radius in shadow map texels. Kept in texels (not world units) so each cascade is sampled evenly.
#ifndef SOFT_SHADOW_RADIUS_TEXELS
#define SOFT_SHADOW_RADIUS_TEXELS 5.2
#endif

// Steepest receiver plane slope (shadow depth per shadow map UV) the taps follow. Steeper surfaces are nearly
// parallel to the light and get no direct light anyway; the clamp only keeps the maths finite.
#ifndef SOFT_SHADOW_MAX_SLOPE
#define SOFT_SHADOW_MAX_SLOPE 16.0
#endif

// Interleaved Gradient Noise - smooth spatial variation for rotation
float getIGNRotation()
{
    float noise = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
    return noise * 6.283185;
}

// Precomputed 8-tap spiral with alternating flip
// Designed so rotated copies interleave rather than overlap
const vec2 spiralDisc[8] = vec2[8](
    vec2(-0.7071,  0.7071),
    vec2( 0.0000, -0.8750),
    vec2( 0.5303,  0.5303),
    vec2(-0.6250,  0.0000),
    vec2( 0.3536, -0.3536),
    vec2( 0.0000,  0.3750),
    vec2(-0.1768, -0.1768),
    vec2( 0.1250,  0.0000)
);

// Receiver plane depth slope: how the surface's shadow map depth changes per unit of shadow map UV, from the
// screen-space derivatives of its shadow coordinates (projected, so this is exact for perspective shadow maps too).
// Each filter tap compares against the surface's own depth at that tap, so a wide filter doesn't shadow the surface
// with itself.
vec2 receiverPlaneDepthSlope(vec3 uvzDx, vec3 uvzDy)
{
    float det = uvzDx.x * uvzDy.y - uvzDx.y * uvzDy.x;
    // degenerate (surface seen edge-on): plain filter
    if (abs(det) <= 1e-4 * length(uvzDx.xy) * length(uvzDy.xy))
        return vec2(0.0);
    vec2 slope = vec2(uvzDy.y * uvzDx.z - uvzDx.y * uvzDy.z, uvzDx.x * uvzDy.z - uvzDy.x * uvzDx.z) / det;
    return clamp(slope, vec2(-SOFT_SHADOW_MAX_SLOPE), vec2(SOFT_SHADOW_MAX_SLOPE));
}

float getFilteredShadowing(sampler2DShadow tex, vec3 uvz, vec2 depthSlope)
{
    float radius = float(SOFT_SHADOW_RADIUS_TEXELS) / @shadowMapResolution;

    // ---- Sample shadow map with rotated spiral disc ----
    float rotation = getIGNRotation();
    float c = cos(rotation) * radius;
    float s = sin(rotation) * radius;

    float shadow = 0.0;
    for (int i = 0; i < 8; i++)
    {
        vec2 p = spiralDisc[i];
        vec2 offset = vec2(p.x * c - p.y * s, p.x * s + p.y * c);
        shadow += shadow2D(tex, vec3(uvz.xy + offset, uvz.z + dot(depthSlope, offset))).r;
    }

    return shadow * 0.125;
}

#endif // @softShadows

#endif // SHADOWS

// ============================================================================

float unshadowedLightRatio(float distance)
{
    float shadowing = 1.0;
#if SHADOWS
#if @softShadows
    // The receiver planes of every cascade, here in uniform control flow: derivatives taken inside the cascade
    // branches below would be undefined wherever neighbouring pixels pick different cascades.
    @foreach shadow_texture_unit_index @shadow_texture_unit_list
        vec3 softUvz@shadow_texture_unit_index = shadowSpaceCoords@shadow_texture_unit_index.xyz / shadowSpaceCoords@shadow_texture_unit_index.w;
        vec2 softSlope@shadow_texture_unit_index = receiverPlaneDepthSlope(dFdx(softUvz@shadow_texture_unit_index), dFdy(softUvz@shadow_texture_unit_index));
    @endforeach
#endif
#if @limitShadowMapDistance
    float fade = clamp((distance - shadowFadeStart) / (maximumShadowMapDistance - shadowFadeStart), 0.0, 1.0);
    if (fade == 1.0)
        return shadowing;
#endif
    bool doneShadows = false;
    @foreach shadow_texture_unit_index @shadow_texture_unit_list
        if (!doneShadows)
        {
            vec3 shadowXYZ = shadowSpaceCoords@shadow_texture_unit_index.xyz / shadowSpaceCoords@shadow_texture_unit_index.w;
#if @perspectiveShadowMaps
            vec3 shadowRegionXYZ = shadowRegionCoords@shadow_texture_unit_index.xyz / shadowRegionCoords@shadow_texture_unit_index.w;
#endif
            if (all(lessThan(shadowXYZ.xy, vec2(1.0, 1.0))) && all(greaterThan(shadowXYZ.xy, vec2(0.0, 0.0))))
            {
#if @softShadows
                shadowing = min(getFilteredShadowing(shadowTexture@shadow_texture_unit_index,
                    softUvz@shadow_texture_unit_index, softSlope@shadow_texture_unit_index), shadowing);
#else
                shadowing = min(shadow2DProj(shadowTexture@shadow_texture_unit_index, shadowSpaceCoords@shadow_texture_unit_index).r, shadowing);
#endif

                doneShadows = all(lessThan(shadowXYZ, vec3(0.95, 0.95, 1.0))) && all(greaterThan(shadowXYZ, vec3(0.05, 0.05, 0.0)));
#if @perspectiveShadowMaps
                doneShadows = doneShadows && all(lessThan(shadowRegionXYZ, vec3(1.0, 1.0, 1.0))) && all(greaterThan(shadowRegionXYZ.xy, vec2(-1.0, -1.0)));
#endif
            }
        }
    @endforeach
#if @limitShadowMapDistance
    shadowing = mix(shadowing, 1.0, fade);
#endif
#endif // SHADOWS
    return shadowing;
}
void applyShadowDebugOverlay()
{
#if SHADOWS && @useShadowDebugOverlay
    bool doneOverlay = false;
    float colourIndex = 0.0;
    @foreach shadow_texture_unit_index @shadow_texture_unit_list
        if (!doneOverlay)
        {
            vec3 shadowXYZ = shadowSpaceCoords@shadow_texture_unit_index.xyz / shadowSpaceCoords@shadow_texture_unit_index.w;
#if @perspectiveShadowMaps
            vec3 shadowRegionXYZ = shadowRegionCoords@shadow_texture_unit_index.xyz / shadowRegionCoords@shadow_texture_unit_index.w;
#endif
            if (all(lessThan(shadowXYZ.xy, vec2(1.0, 1.0))) && all(greaterThan(shadowXYZ.xy, vec2(0.0, 0.0))))
            {
                colourIndex = mod(@shadow_texture_unit_index.0, 3.0);
                if (colourIndex < 1.0)
                    gl_FragData[0].x += 0.1;
                else if (colourIndex < 2.0)
                    gl_FragData[0].y += 0.1;
                else
                    gl_FragData[0].z += 0.1;

                doneOverlay = all(lessThan(shadowXYZ, vec3(0.95, 0.95, 1.0))) && all(greaterThan(shadowXYZ, vec3(0.05, 0.05, 0.0)));
#if @perspectiveShadowMaps
                doneOverlay = doneOverlay && all(lessThan(shadowRegionXYZ.xyz, vec3(1.0, 1.0, 1.0))) && all(greaterThan(shadowRegionXYZ.xy, vec2(-1.0, -1.0)));
#endif
            }
        }
    @endforeach
#endif // SHADOWS
}
