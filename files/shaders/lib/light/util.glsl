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

vec3 calcPointLightBounce(PointLight light, float dist, vec3 lightDir, vec3 viewNormal) {
    float bounce = sqrt(min(calcBaseAttenuation(light, dist), 1.0)) * calcRadiusFade(light, dist);
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

// How much of a held light's direct light gets past whoever carries it, 1 for all: the carrier's body as an upright
// cylinder, so the light no longer shines through them onto what lies behind. The edge softens with the distance
// behind the body, as from a small flame. Their own body and gear are lit as usual, and the light's bounce is not
// blocked, so it still wraps around them.
float carrierShade(PointLight light, vec3 viewPos) {
    float radius = light.carrierFoot.w;
    if (radius <= 0.0)
        return 1.0;
    vec3 axis = light.carrierAxis.xyz;
    float height = length(axis);
    vec3 up = axis / height;
    vec3 fromLight = light.position.xyz - light.carrierFoot.xyz;
    vec3 toPoint = viewPos - light.carrierFoot.xyz;
    vec3 pointAcross = toPoint - up * dot(toPoint, up);
    float pointHeight = dot(toPoint, up);
    if (dot(pointAcross, pointAcross) < radius * radius * 2.6 && pointHeight > -8.0 && pointHeight < height + 8.0)
        return 1.0;
    vec3 ray = toPoint - fromLight;
    vec3 rayAcross = ray - up * dot(ray, up);
    vec3 lightAcross = fromLight - up * dot(fromLight, up);
    float across2 = dot(rayAcross, rayAcross);
    float t = across2 > 1e-4 ? clamp(-dot(lightAcross, rayAcross) / across2, 0.0, 1.0) : 0.0;
    float dist = length(lightAcross + rayAcross * t);
    float h = dot(fromLight + ray * t, up);
    float soft = 2.0 + 12.0 * (1.0 - t);
    float past = smoothstep(radius - soft, radius + soft, dist);
    float overHead = smoothstep(height - 6.0, height + 10.0, h);
    return max(past, overHead);
}

void calcPointLighting(PointLight light, vec3 viewDir, vec3 viewPos, vec3 viewNormal, float shininess, inout vec3 diffuseLight, inout vec3 ambientLight, inout vec3 specularLight) {
    vec3 lightPos = light.position.xyz - viewPos;
    float lightDistance = length(lightPos);

    // cull point lighting by radius, light is guaranteed to not fall outside this bound with our cutoff
#if !@classicFalloff
    if (lightDistance > light.radius)
        return;
#endif

    vec3 lightDir = lightPos / lightDistance;

    float attenuation = calcAttenuation(light, lightDistance) * clusterFade(viewPos, light.radius);
    float direct = attenuation * carrierShade(light, viewPos);

    float lambertTerm = lambert(viewNormal, lightDir, viewDir);
    if (lightDistance < uSelfLitRange)
        lambertTerm = max(lambertTerm, 0.25 + 0.25 * abs(dot(viewNormal, lightDir)));
    diffuseLight += light.diffuse.xyz * lambertTerm * direct;
    ambientLight += light.ambient.xyz * attenuation;
    if (pointLightBounceAmount(light) > 0.0)
        ambientLight += calcPointLightBounce(light, lightDistance, lightDir, viewNormal) * clusterFade(viewPos, light.radius);
    specularLight += light.specular.xyz * specularIntensity(viewNormal, viewDir, shininess, lightDir) * direct;
}

#endif
