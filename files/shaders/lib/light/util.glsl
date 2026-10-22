#ifndef LIB_LIGHT_UTIL
#define LIB_LIGHT_UTIL

#include "lib/light/struct.glsl"

uniform float clusterFar;

float fade(float x)
{
    x = clamp(x, 0.0, 1.0);
    x = 1.0 - x * x;
    x = 1.0 - x * x;
    return x;
}

float lambert(vec3 viewNormal, vec3 lightDir, vec3 viewDir)
{
    float lambert = dot(viewNormal, lightDir);
#ifndef GROUNDCOVER
    lambert = max(lambert, 0.0);
#else
    float eyeCosine = dot(viewNormal, viewDir);
    if (lambert < 0.0)
    {
        lambert = -lambert;
        eyeCosine = -eyeCosine;
    }
    lambert *= clamp(-8.0 * (1.0 - 0.3) * eyeCosine + 1.0, 0.3, 1.0);
#endif
    return lambert;
}

float specularIntensity(vec3 viewNormal, vec3 viewDir, float shininess, vec3 lightDir)
{
    if (dot(viewNormal, lightDir) > 0.0)
    {
        vec3 halfVec = normalize(lightDir - viewDir);
        float NdotH = max(dot(viewNormal, halfVec), 0.0);
        return pow(NdotH, shininess);
    }

    return 0.0;
}

// [Shaders] light brightness, light falloff, light bounce and light hotspot softening (SharedUniformStateUpdater):
// x = brightness - 1, y = falloff - 1, z = bounce, w = hotspot rounding strength (8 * softening). Zero, which is also
// what a program drawn outside the scene root reads, leaves point lights unchanged.
uniform vec4 pointLightTuning;

// OpenMGE XE: a lamp's own model (the lantern's glass and frame, the candle) sits around its light, mostly facing away
// from it, so it would get none of it. Within this distance of a light it takes the light from either side, at a
// quarter to a half of full strength: the glass glows and the candle is lit by its flame. Set on lamps, 0 elsewhere.
uniform float uSelfLitRange;

// The light's own distance curve, as the game data and attenuation settings make it.
float calcBaseAttenuation(PointLight light, float dist) {
    return 1.0 / (light.constant + light.linear * dist + light.quadratic * dist * dist);
}

float calcRadiusFade(PointLight light, float dist) {
    #if !@classicFalloff || @lightingMethodClustered
        // Fade illumination out to 0 when reaching the lights radius
        return 1.0 - fade((dist / light.radius - 0.75) / 0.25);
    #else
        return 1.0;
    #endif
}

float calcAttenuation(PointLight light, float dist) {
    float attenuation = calcBaseAttenuation(light, dist);
    // Falloff raises the distance curve to a power: above 1 it brightens where the light is past full strength
    // (close to the source) and darkens where it is below, so pools of light get tighter and more contrasted.
    // The radius fade below is left as is, so lights still end where they did.
    if (pointLightTuning.y != 0.0)
        attenuation = pow(attenuation, 1.0 + pointLightTuning.y);
    // Hotspot softening: right next to a light the curve climbs far past full strength (several times over, more
    // with falloff above 1), which burns the wall behind a candle out to white. Round off the part above full strength
    // towards a ceiling of 1 + 1/w; the curve and its slope are unchanged at full strength and below.
    if (pointLightTuning.w > 0.0 && attenuation > 1.0)
    {
        float over = attenuation - 1.0;
        attenuation = 1.0 + over / (1.0 + pointLightTuning.w * over);
    }
    attenuation *= 1.0 + pointLightTuning.x;
    return attenuation * calcRadiusFade(light, dist);
}

// Light bounce: the light a lamp sends onto the walls, floor and ceiling around it and that reaches everything else
// from there. Stood in for by a soft fill in the light's colour: no brighter than the light at full strength, falling
// off far more gently than its direct light (the square root of its distance curve, whatever the falloff) and lighting
// the sides facing away from it at half the strength of the side facing it. It adds to the ambient term, so it brings
// out a room's shapes in the light's colour rather than raising the room's base light.
// How much bounce a light gives: its own amount when it has one (held lights, in position.w), else the scene's.
float pointLightBounceAmount(PointLight light) {
    return light.position.w >= 0.0 ? light.position.w : pointLightTuning.z;
}

// A held light's bounce reaches only as far as its light did before the Light Radius Multiplier (carrierAxis.w): a
// lantern lights up the walls around whoever carries it, not the far end of a great hall.
vec3 calcPointLightBounce(PointLight light, float dist, vec3 lightDir, vec3 viewNormal) {
    float bounce = sqrt(min(calcBaseAttenuation(light, dist), 1.0));
    // a carried light's bounce ends at its reach, fading out over most of the way there, not as a visible ring
    if (light.position.w >= 0.0 && light.carrierAxis.w > 0.0)
        bounce *= 1.0 - smoothstep(0.2, 1.0, dist / light.carrierAxis.w);
    else
        bounce *= calcRadiusFade(light, dist);
    float wrap = 0.5 + 0.25 * (1.0 + dot(viewNormal, lightDir));
    return light.diffuse.xyz * (pointLightBounceAmount(light) * (1.0 + pointLightTuning.x) * bounce * wrap);
}

int getClusterTileIndex(vec2 screenRes, vec3 gridSize, float near, vec2 screenCoord, float viewSpaceZ) {
    int zTile = int((log(abs(viewSpaceZ) / near) * int(gridSize.z)) / log(clusterFar / near));
    vec2 tileSize = screenRes / vec2(gridSize.xy);
    ivec3 tile = ivec3(screenCoord / tileSize, zTile);
    int tileIndex = tile.x + (tile.y * int(gridSize.x)) + (tile.z * int(gridSize.x) * int(gridSize.y));

    return tileIndex;
}

void calcDirectionalLighting(DirectionalLight light, vec3 viewDir, vec3 viewNormal, float shininess, inout vec3 diffuseLight, inout vec3 ambientLight, inout vec3 specularLight) {
    vec3 dir = normalize(light.position.xyz);
    ambientLight += light.ambient.xyz;
    diffuseLight += light.diffuse.xyz * lambert(viewNormal, dir, viewDir);
    specularLight += light.specular.xyz * specularIntensity(viewNormal, viewDir, shininess, dir);
}

// Ensure lights with bounds crossing the far cluster plane fade out.
// Without this there will be a hard cutoff where objects outside the cluster far plane are not lit.
float clusterFade(vec3 viewPos, float radius) {
#if @lightingMethodClustered
    return 1.0 - fade(clamp((-viewPos.z - (clusterFar - radius)) / radius, 0.0, 1.0));
#else
    return 1.0;
#endif
}

#if @lightingMethodClustered
// OpenMGE XE lantern shadows (SceneUtil::HeldLightShadow): the lights the player carries cast real shadows from depth
// cube maps drawn around them each frame. heldLightShadowPos[i]: light i in world space, w the far end of its cube map
// (0: no light).
uniform samplerCubeShadow heldLightShadowMap0;
uniform samplerCubeShadow heldLightShadowMap1;
uniform vec4 heldLightShadowPos[2];
// where the faces of light i were drawn from (where it was, with the cache); in w, where its shadows start to fade out
// when they end before its light does (held light shadow distance, 0: they don't)
uniform vec4 heldLightShadowCenter[2];
uniform vec2 heldLightShadowRange; // near, resolution
uniform mat4 osg_ViewMatrix;
uniform mat4 osg_ViewMatrixInverse;

float heldLightShadowLookup(int index, vec4 coords) {
    return index == 0 ? texture(heldLightShadowMap0, coords) : texture(heldLightShadowMap1, coords);
}

// Whether the light sees the point v (from the light), compared at the depth the faces store for it.
float heldLightShadowTap(int index, vec3 v, float n, float f) {
    float z = max(max(abs(v.x), abs(v.y)), abs(v.z));
    float ref = ((f + n) / (f - n) - 2.0 * f * n / ((f - n) * z)) * 0.5 + 0.5;
    return heldLightShadowLookup(index, vec4(v, ref));
}

// How much of the light reaches the point, 1 for all (and for every other light). wide: the light's bounce, looked up
// over a much wider spread (bounce comes off the surfaces around, so it softens shadows instead of keeping their
// edges), but still kept from behind walls the light can't reach.
float heldLightShadowLit(PointLight light, vec3 viewPos, vec3 viewNormal, bool wide) {
    // only carried lights have their own bounce (position.w), and only they can be one of these: every other light
    // leaves before the search below
    if (light.position.w < 0.0)
        return 1.0;
    int index = -1;
    for (int i = 0; i < 2; ++i) {
        if (heldLightShadowPos[i].w <= 0.0)
            continue;
        vec3 lightView = (osg_ViewMatrix * vec4(heldLightShadowPos[i].xyz, 1.0)).xyz;
        if (dot(light.position.xyz - lightView, light.position.xyz - lightView) <= 16.0)
            index = i;
    }
    if (index < 0)
        return 1.0;
    vec4 lightPos = heldLightShadowPos[index];
    float n = heldLightShadowRange.x;
    float f = lightPos.w;
    vec3 worldPos = (osg_ViewMatrixInverse * vec4(viewPos, 1.0)).xyz;
    vec3 worldNormal = normalize(mat3(osg_ViewMatrixInverse) * viewNormal);
    vec3 v = worldPos - heldLightShadowCenter[index].xyz;
    float dist = length(v);
    if (dist >= f || dist < 1.0)
        return 1.0;
    // one texel of the face at this distance, about
    float texel = 2.0 * max(max(abs(v.x), abs(v.y)), abs(v.z)) / heldLightShadowRange.y;
    // push the lookup off the surface towards the light by a texel and a half against acne (the back of a surface the
    // other way, it is dark from the light's angle anyway)
    float facing = dot(worldNormal, -v / dist);
    v += worldNormal * (texel * 1.5 * (facing < 0.0 ? -1.0 : 1.0));
    // The softening taps are spread along the surface (not across the light's view of it), each compared at its own
    // distance: spread across the view, a tap tilted towards the light finds the same floor nearer than the point
    // looked up and shades it - the floor lit fully only right under the light, a box around the player.
    vec3 side = normalize(cross(worldNormal, abs(worldNormal.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    vec3 up = cross(worldNormal, side);
    float spread = wide ? max(texel * 1.25, dist * 0.08) : texel * 1.25;
    float lit = heldLightShadowTap(index, v, n, f);
    lit += heldLightShadowTap(index, v + side * spread, n, f);
    lit += heldLightShadowTap(index, v - side * spread, n, f);
    lit += heldLightShadowTap(index, v + up * spread, n, f);
    lit += heldLightShadowTap(index, v - up * spread, n, f);
    float fadeStart = heldLightShadowCenter[index].w;
    return fadeStart > 0.0 ? mix(lit * 0.2, 1.0, smoothstep(fadeStart, f, dist)) : lit * 0.2;
}

float heldLightShadow(PointLight light, vec3 viewPos, vec3 viewNormal) {
    return heldLightShadowLit(light, viewPos, viewNormal, false);
}

float heldLightShadowBounce(PointLight light, vec3 viewPos, vec3 viewNormal) {
    return heldLightShadowLit(light, viewPos, viewNormal, true);
}
#if @lampShadows
// OpenMGE XE lamp shadows (SceneUtil::LampShadows): lamps placed in the room cast shadows from six depth faces each,
// layers of these arrays: what doesn't move in lampShadowStill, drawn once, characters in lampShadowLive, drawn each
// frame for the nearest lamps. A light's carrierFoot: x its first still layer + 1, y its first live layer + 1 (0: none),
// z the far end of its faces.
uniform sampler2DArrayShadow lampShadowStill;
uniform sampler2DArrayShadow lampShadowLive;
uniform vec3 lampShadowParams; // near, resolution, tangent of half a face's field of view
uniform vec3 lampShadowFaceS[6];
uniform vec3 lampShadowFaceU[6];

// Whether the lamp sees the point v (from the lamp): its still face, and its live one when it has them.
float lampShadowTap(vec3 v, float stillLayer, float liveLayer, float n, float f) {
    vec3 a = abs(v);
    int face = (a.x >= a.y && a.x >= a.z) ? (v.x > 0.0 ? 0 : 1) : (a.y >= a.z ? (v.y > 0.0 ? 2 : 3) : (v.z > 0.0 ? 4 : 5));
    float z = max(max(a.x, a.y), a.z);
    vec2 uv = vec2(dot(lampShadowFaceS[face], v), dot(lampShadowFaceU[face], v)) / (z * lampShadowParams.z) * 0.5 + 0.5;
    float ref = ((f + n) / (f - n) - 2.0 * f * n / ((f - n) * z)) * 0.5 + 0.5;
    float lit = texture(lampShadowStill, vec4(uv, stillLayer + float(face), ref));
    if (liveLayer >= 0.0)
        lit *= texture(lampShadowLive, vec4(uv, liveLayer + float(face), ref));
    return lit;
}

// How much of a lamp's light reaches the point, 1 for all (and for lights without faces). wide: its bounce, looked up
// over a wider spread (see heldLightShadowLit).
float lampShadowLit(PointLight light, vec3 viewPos, vec3 viewNormal, bool wide) {
    if (light.carrierFoot.x < 0.5)
        return 1.0;
    float stillLayer = light.carrierFoot.x - 1.0;
    float liveLayer = light.carrierFoot.y - 1.0;
    float n = lampShadowParams.x;
    float f = light.carrierFoot.z;
    vec3 v = mat3(osg_ViewMatrixInverse) * (viewPos - light.position.xyz);
    float dist = length(v);
    if (dist >= f || dist < 1.0)
        return 1.0;
    vec3 worldNormal = normalize(mat3(osg_ViewMatrixInverse) * viewNormal);
    float texel = 2.0 * lampShadowParams.z * max(max(abs(v.x), abs(v.y)), abs(v.z)) / lampShadowParams.y;
    float facing = dot(worldNormal, -v / dist);
    v += worldNormal * (texel * 1.5 * (facing < 0.0 ? -1.0 : 1.0));
    // spread along the surface, each tap compared at its own distance (see heldLightShadowLit)
    vec3 side = normalize(cross(worldNormal, abs(worldNormal.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    vec3 up = cross(worldNormal, side);
    float spread = wide ? max(texel * 1.25, dist * 0.08) : texel * 1.25;
    float lit = lampShadowTap(v, stillLayer, liveLayer, n, f);
    lit += lampShadowTap(v + side * spread, stillLayer, liveLayer, n, f);
    lit += lampShadowTap(v - side * spread, stillLayer, liveLayer, n, f);
    lit += lampShadowTap(v + up * spread, stillLayer, liveLayer, n, f);
    lit += lampShadowTap(v - up * spread, stillLayer, liveLayer, n, f);
    return lit * 0.2;
}
#else
float lampShadowLit(PointLight light, vec3 viewPos, vec3 viewNormal, bool wide) {
    return 1.0;
}
#endif
#else
float lampShadowLit(PointLight light, vec3 viewPos, vec3 viewNormal, bool wide) {
    return 1.0;
}

float heldLightShadow(PointLight light, vec3 viewPos, vec3 viewNormal) {
    return 1.0;
}

float heldLightShadowBounce(PointLight light, vec3 viewPos, vec3 viewNormal) {
    return 1.0;
}
#endif

void calcPointLighting(PointLight light, vec3 viewDir, vec3 viewPos, vec3 viewNormal, float shininess, inout vec3 diffuseLight, inout vec3 ambientLight, inout vec3 specularLight) {
    vec3 lightPos = light.position.xyz - viewPos;
    float lightDistance = length(lightPos);

    // cull point lighting by radius, light is guaranteed to not fall outside this bound with our cutoff
#if !@classicFalloff
    if (lightDistance > light.radius)
        return;
#elif @lightingMethodClustered
    // OpenMGE XE: clustered lighting fades every light to nothing at its radius whatever the falloff (calcRadiusFade),
    // so a point past it gets nothing (a carried light's bounce reaches carrierAxis.w): skip the work, shadow lookups
    // included. A cluster lists every light that touches any part of it, so many points in it are past some of them.
    if (lightDistance > max(light.radius, light.carrierAxis.w))
        return;
#endif

    vec3 lightDir = lightPos / lightDistance;

    float edgeFade = clusterFade(viewPos, light.radius);
    float attenuation = calcAttenuation(light, lightDistance) * edgeFade;
    // a lamp's shadow, looked up once: its bounce reuses it (see below)
    float lampLit = lampShadowLit(light, viewPos, viewNormal, false);
    float direct = attenuation * heldLightShadow(light, viewPos, viewNormal) * lampLit;

    float lambertTerm = lambert(viewNormal, lightDir, viewDir);
    if (lightDistance < uSelfLitRange)
        lambertTerm = max(lambertTerm, 0.25 + 0.25 * abs(dot(viewNormal, lightDir)));
    diffuseLight += light.diffuse.xyz * lambertTerm * direct;
    ambientLight += light.ambient.xyz * attenuation;
    if (pointLightBounceAmount(light) > 0.0)
        ambientLight += calcPointLightBounce(light, lightDistance, lightDir, viewNormal) * edgeFade
            // A lamp's bounce takes its direct shadow rather than a wider lookup of its own: five more shadow samples
            // per lamp for every point it reaches cost about as much as all its other light together, for a fill that
            // is soft anyway. Walls still keep it out of the next room.
            * heldLightShadowBounce(light, viewPos, viewNormal) * lampLit;
    specularLight += light.specular.xyz * specularIntensity(viewNormal, viewDir, shininess, lightDir) * direct;
}

#endif
