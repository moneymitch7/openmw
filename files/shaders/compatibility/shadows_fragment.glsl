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

// Filter radius in shadow map texels. Kept in texels (not world units) so each cascade is sampled evenly. 5.2 texels
// at a 1024 shadow map, scaled with the map size so the softening keeps the same width on the ground: a 512 map's
// texels are twice as wide, so it gets 2.6 rather than a blur twice as wide. Held between 2.5 texels, below which the
// filter stops hiding the texels, and 8, beyond which 8 taps sample the disc too sparsely.
#ifndef SOFT_SHADOW_RADIUS_TEXELS
#define SOFT_SHADOW_RADIUS_TEXELS clamp(5.2 * @shadowMapResolution / 1024.0, 2.5, 8.0)
#endif

// Steepest receiver plane slope (shadow depth per shadow map UV) the taps follow. Steeper surfaces are nearly
// parallel to the light and get no direct light anyway; the clamp only keeps the maths finite.
// How much wider than usual the edges are, 0 to 1 (twice as wide), set by the engine from the weather: crisp under a
// clear sky, softer through cloud and haze, as the light scatters. Held to 10 texels.
uniform float sunShadowSoftness;

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
    float radius = min(float(SOFT_SHADOW_RADIUS_TEXELS) * (1.0 + clamp(sunShadowSoftness, 0.0, 1.0)), 10.0)
        / @shadowMapResolution;

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
    // Each cascade covers a square of the light's view; the first that holds the point shadows it. Across the outer
    // band of a cascade's square its shadow fades into the next cascade's, so the change of resolution and filter width
    // between them is a gradient rather than a line on the ground that walks along with the camera.
    float remaining = 1.0; // the share of the shadowing still to come from later cascades
    float lastShadowing = 1.0;
    float blended = 0.0;
    @foreach shadow_texture_unit_index @shadow_texture_unit_list
        if (remaining > 0.0)
        {
            vec3 shadowXYZ = shadowSpaceCoords@shadow_texture_unit_index.xyz / shadowSpaceCoords@shadow_texture_unit_index.w;
#if @perspectiveShadowMaps
            vec3 shadowRegionXYZ = shadowRegionCoords@shadow_texture_unit_index.xyz / shadowRegionCoords@shadow_texture_unit_index.w;
#endif
            if (all(lessThan(shadowXYZ.xy, vec2(1.0, 1.0))) && all(greaterThan(shadowXYZ.xy, vec2(0.0, 0.0))))
            {
#if @softShadows
                float cascadeShadowing = getFilteredShadowing(shadowTexture@shadow_texture_unit_index,
                    softUvz@shadow_texture_unit_index, softSlope@shadow_texture_unit_index);
#else
                float cascadeShadowing = shadow2DProj(shadowTexture@shadow_texture_unit_index, shadowSpaceCoords@shadow_texture_unit_index).r;
#endif
                // 0 inside, rising to 1 by 5% from the square's edge (the filter's taps stay within the map)
                vec2 edge = abs(shadowXYZ.xy * 2.0 - 1.0);
                float handOver = smoothstep(0.78, 0.9, max(edge.x, edge.y));
                if (shadowXYZ.z >= 1.0 || shadowXYZ.z <= 0.0)
                    handOver = 1.0; // beyond this cascade's depth range: its sample means nothing
                else
                    lastShadowing = cascadeShadowing;
#if @perspectiveShadowMaps
                if (!(all(lessThan(shadowRegionXYZ, vec3(1.0, 1.0, 1.0))) && all(greaterThan(shadowRegionXYZ.xy, vec2(-1.0, -1.0)))))
                    handOver = 1.0;
#endif
                blended += remaining * (1.0 - handOver) * cascadeShadowing;
                remaining *= handOver;
            }
        }
    @endforeach
    // past the last cascade's band: its own shadowing, as before
    shadowing = blended + remaining * lastShadowing;
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
