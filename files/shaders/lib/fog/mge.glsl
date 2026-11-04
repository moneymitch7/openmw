#ifndef LIB_FOG_MGE
#define LIB_FOG_MGE

// OpenMGE XE: MGE XE's distant fog and atmospheric scattering ([Fog] mge fog). Only compiled with @mgeFog, from
// compatibility/fog.glsl, so with the setting off no shader changes.
//
// The model (MGE XE shaders\core\XE Common.fx, fogColourScatter / fogColourSky, exp fog with scattering):
//   x         = (dist - expStart) / expDivisor               optical depth
//   fog       = saturate(exp(-x))                            transmittance
//   inscatter = scatter(dir, saturate(0.224 x))              in nice weather (Clear, Cloudy)
//             = (1 - fog) * fog colour                       otherwise
//   colour'   = fog * colour + inscatter
// with the fog range from MGE XE's distantland.cpp adjustFog for each weather's distant land fog factor and offset.
// Everything that is the same for the whole frame (the fog range for the weather and its transition, the near-range
// fit, the weather's weights) is worked out once per frame on the CPU (apps/openmw/mwrender/mgefog.cpp) and comes in
// through the uniforms below; what is left here is per pixel.

uniform vec4 mgeFogA;      // x exp fog start, y exp fog divisor (units), z dense weather weight, w height layer weight
uniform vec4 mgeFogB;      // x near fog start, y near fog end (units), z nice weather, w bad weather transition weight
uniform vec2 mgeFogState;  // x exterior, y the viewer is underwater
uniform vec3 mgeSkyColor;  // the weather's sky colour
uniform vec3 mgeFogColor;  // the weather's fog colour (the storm's own through a nice<->storm transition)
uniform vec3 mgeSunDir;    // world space, below the horizon at night

#ifndef PLAYER_POS_DECLARED
#define PLAYER_POS_DECLARED
uniform vec3 playerPos;
#endif

// Scattering coefficients (the MGG preset the fog was tuned with) and XE's sky colour terms.
const vec3 mgeOutscatter = vec3(0.2411, 0.4339, 0.6677);
const vec3 mgeInscatter = vec3(0.0443, 0.1836, 0.2887);
const vec3 mgeSkyBase = vec3(0.23, 0.39, 0.68);
const float mgeSkyWeight = 0.38;
// Morrowind's own draw range in MGE XE: inside it the fog follows the linear range fitted to the exp curve.
const float mgeNearViewRange = 7168.0;
const float mgeInscatterDistScale = 0.224;
// Dense weather: the fog never clears entirely, and is thicker in valleys than on summits (from the player's height).
const float mgeFogFloor = 0.04;
const float mgeFogScaleHeight = 4608.0;

// XE's dome blend: the fog colour below about 4 degrees, the sky colour from about 30 up.
float mgeSkyFogH(float dirZ)
{
    return 1.0 - pow(clamp(1.0 - 2.22 * clamp(dirZ - 0.075, 0.0, 1.0), 0.0, 1.0), 1.15);
}

struct MgeScatterPrep
{
    vec3 att;
    vec3 colour;
    float gain;
    float sunB;
};

MgeScatterPrep mgeScatterPrep(vec3 dir, vec3 skyCol)
{
    float sunZ = mgeSunDir.z;
    float sunaltitude = max(pow(1.0 + sunZ, 10.0), 1e-6);
    float sunaltitude_a = 2.8 + 4.3 / sunaltitude;
    float sunaltitude_b = clamp(1.0 - exp2(-1.9 * sunaltitude), 0.0, 1.0);
    float sunaltitude2 = clamp(exp(-2.0 * sunZ), 0.0, 1.0) * clamp(sunaltitude, 0.0, 1.0);
    vec3 newSkyCol = mgeSkyWeight * skyCol + mgeSkyBase;

    float suncos = dot(dir, mgeSunDir);
    float mie = (1.62 / (1.3 - suncos)) * sunaltitude2;
    float rayl = 1.0 - 0.09 * mie;
    float atmdep = 1.33 * exp(-2.0 * clamp(dir.z, 0.0, 1.0));
    vec3 sunscatter = mix(mgeInscatter, mgeOutscatter, 0.5 * (1.0 + suncos));

    MgeScatterPrep p;
    p.att = atmdep * sunscatter * (sunaltitude_a + mie);
    p.colour = vec3(0.125 * mie) + newSkyCol * rayl;
    p.gain = 1.1 * atmdep + 0.5;
    p.sunB = sunaltitude_b;
    return p;
}

vec3 mgeScatterAt(MgeScatterPrep p, float fogdist)
{
    vec3 att = (1.0 - exp(-fogdist * p.att)) / p.att;
    return p.colour * att * p.gain * p.sunB;
}

// The fog over a distance in a world direction: rgb = what the air adds, a = how much of the scene shows through.
// skyBehind (a = 1 when there is one) is the sky drawn behind this pixel (sky blending); far is the viewing distance.
vec4 mgeFogColour(float dist, vec3 dirWorld, float far, vec4 skyBehind)
{
    // passes drawn without fog (the local map)
    if (fog.start > 1000000.0)
        return vec4(0.0, 0.0, 0.0, 1.0);

    // interiors: MGE's linear fog with the cell's own colour and range
    if (mgeFogState.x < 0.5)
    {
        float f = clamp((fog.end - dist) / max(fog.end - fog.start, 1.0), 0.0, 1.0);
        return vec4((1.0 - f) * fog.color.xyz, f);
    }

    float wDense = mgeFogA.z;
    float distEff = dist;
    if (wDense > 0.001)
    {
        // height layer: the distance counts for more below the player's height and less above it
        float dz = (osg_ViewMatrixInverse[3].z + dirWorld.z * dist) - playerPos.z;
        float layer = 1.0;
        if (abs(dz) > 1.0)
            layer = (mgeFogScaleHeight / dz) * (1.0 - exp(-clamp(dz / mgeFogScaleHeight, -30.0, 30.0)));
        float steep = smoothstep(0.25, 0.6, dirWorld.z);
        layer = clamp(layer, mix(0.25, 0.08, steep), 2.5);
        distEff = dist * mix(1.0, layer, mgeFogA.w);
    }

    float x = (distEff - mgeFogA.x) / mgeFogA.y;
    float fogT;
    if (distEff <= mgeNearViewRange)
        fogT = clamp((mgeFogB.y - distEff) / (mgeFogB.y - mgeFogB.x), 0.0, 1.0);
    else
        fogT = clamp(exp(-x), 0.0, 1.0);
    fogT = mix(fogT, max(fogT, mgeFogFloor), wDense);
    float fogdist = clamp(mgeInscatterDistScale * x, 0.0, 1.0);

    vec3 convBase = mix(mgeFogColor, mgeSkyColor, mgeSkyFogH(dirWorld.z));
    if (skyBehind.a > 0.5)
        convBase = mix(convBase, skyBehind.rgb, smoothstep(0.55, 0.92, 1.0 - fogT));

    float nice = mgeFogB.z;
    MgeScatterPrep scatter = MgeScatterPrep(vec3(1.0), vec3(0.0), 0.0, 0.0);
    if (nice > 0.001)
        scatter = mgeScatterPrep(dirWorld, mgeSkyColor);

    // the water reflection has no sky behind to take: far land melts into the dome's colour instead
    bool mirrored = isReflection && skyBehind.a < 0.5;
    vec3 mirrorDome = convBase;
    if (mirrored)
    {
        if (nice > 0.001)
            mirrorDome = mix(convBase, mgeScatterAt(scatter, 1.0), nice);
        convBase = mix(convBase, mirrorDome, smoothstep(0.55, 0.92, 1.0 - fogT));
    }

    vec3 rgb = (1.0 - fogT) * convBase;
    if (nice > 0.001)
        rgb = mix(rgb, mgeScatterAt(scatter, fogdist), nice);

    // horizon seal: the last stretch before the viewing distance turns into the dome's colour
    float seal = isReflection ? smoothstep(0.80, 0.96, dist / far) : smoothstep(0.88, 0.995, dist / far);
    if (seal > 0.001)
    {
        vec3 domeCol = convBase;
        if (nice > 0.001)
            domeCol = mix(convBase, mgeScatterAt(scatter, 1.0), nice);
        if (mirrored)
            domeCol = mirrorDome;
        rgb = mix(rgb, domeCol, seal);
        fogT *= 1.0 - seal;
    }

    // a transition to or from a bad weather: distant land never reads brighter than the sky behind it, and its hue goes to that sky
    float clampWeight = min(1.0, 2.0 * mgeFogB.w);
    if (clampWeight > 0.001)
    {
        const vec3 lumW = vec3(0.299, 0.587, 0.114);
        float skyL = (skyBehind.a > 0.5) ? dot(skyBehind.rgb, lumW)
                                         : max(dot(fog.color.xyz, lumW), dot(mgeSkyColor, lumW));
        float ceiling = skyL * (1.0 - fogT);
        float l = dot(rgb, lumW);
        if (l > ceiling && l > 1e-5)
            rgb *= mix(1.0, ceiling / l, clampWeight);
        if (skyBehind.a > 0.5)
        {
            float skyBehindL = dot(skyBehind.rgb, lumW);
            float rgbL = dot(rgb, lumW);
            float hueWeight = min(1.0, 4.0 * mgeFogB.w) * smoothstep(0.55, 0.92, 1.0 - fogT);
            if (skyBehindL > 1e-5 && rgbL > 1e-5 && hueWeight > 0.001)
                rgb = mix(rgb, skyBehind.rgb * (rgbL / skyBehindL), hueWeight);
        }
    }

    return vec4(rgb, fogT);
}

// The sky dome's colour in a world direction: the same scattering at fogdist 1, so the horizon is seamless.
vec3 mgeFogColourSky(vec3 dirWorld, vec3 zenithCol, vec3 skyCol)
{
    vec3 base = mix(mgeFogColor, zenithCol, mgeSkyFogH(dirWorld.z));
    float nice = mgeFogB.z;
    if (nice > 0.001 && mgeFogState.y < 0.5)
        return mix(base, mgeScatterAt(mgeScatterPrep(dirWorld, skyCol), 1.0), nice);
    return base;
}

// a 4x4 ordered dither for the dome (smooth gradients band in 8 bits)
float mgeSkyDither(vec2 fragCoord)
{
    const float d[16] = float[16](0.001176, 0.001961, -0.001176, -0.001699, -0.000654, -0.000915, 0.000392, 0.000131,
        -0.000131, -0.001961, 0.000654, 0.000915, 0.001699, 0.001438, -0.000392, -0.001438);
    int i = int(mod(fragCoord.x, 4.0)) * 4 + int(mod(fragCoord.y, 4.0));
    return d[i];
}

#endif
