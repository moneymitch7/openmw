#version 120

#include "lib/core/vertex.h.glsl"

varying vec4  position;
varying float linearDepth;

#include "shadows_vertex.glsl"
#include "lib/view/depth.glsl"

uniform vec3 nodePosition;
uniform vec3 playerPos;
#define OMW_DECL_PLAYERPOS // mge_fog.glsl reuses it

// OpenMGE XE lighting: water's weather verdict (the full decomposition core,
// v4 + rescue, as in the 0.51 port) comes from row 1 of the once-per-frame
// pass texture (see the top of mge_fog.glsl) and is shipped through varyings;
// water.frag defines MGE_WX_STAGE 0 and only decodes.
#define MGE_WX_STAGE 0
#define MGE_WX_VERTEX 1
#define MGE_WX_ROW 1
#include "compatibility/mge_fog.glsl"

varying vec3 worldPos;
varying vec2 rippleMapUV;
varying vec3 passViewPos;

#if @waves
// OpenMGE XE 3D water: the wave grid (a dense grid that follows the camera in whole grid steps) is displaced by a sum
// of Gerstner waves; the big flat plane is cut out under it in water.frag.
uniform bool waveSurface;
uniform vec2 waveGridOffset; // grid centre in water-node space
uniform float waveAmplitude; // peak height now (weather-scaled), 0 = flat
uniform sampler2D waveDepthMap; // 0..1: water depth over the terrain around the player
uniform vec4 waveDepthMapRect; // xy: world position of the map's corner, z: 1 / its size
uniform float osg_SimulationTime;
varying vec3 waveNormal;

// Deep-water Gerstner wave. Morrowind units are ~1.4 cm, so gravity is ~690 units/s^2.
void addGerstnerWave(vec2 p, vec2 dir, float wavelength, float amplitude, float steepness, float t,
    inout vec3 displacement, inout vec3 normal)
{
    float k = 6.2831853 / wavelength;
    float phase = k * dot(dir, p) - sqrt(690.0 * k) * t;
    float c = cos(phase);
    float s = sin(phase);
    displacement.xy += (steepness * amplitude * c) * dir;
    displacement.z += amplitude * s;
    normal.xy -= (k * amplitude * c) * dir;
    normal.z -= steepness * k * amplitude * s;
}

// A swell and six shorter waves spread around one wind direction, wavelengths not multiples of each other so the
// pattern doesn't repeat visibly; the amplitudes add up to `height`. Steep enough to sharpen the crests, but the
// horizontal motion never folds the surface over itself below a height of ~180.
void computeWaves(vec2 p, float t, float height, out vec3 displacement, out vec3 normal)
{
    displacement = vec3(0.0);
    normal = vec3(0.0, 0.0, 1.0);
    addGerstnerWave(p, vec2(0.766, 0.643), 1730.0, 0.324 * height, 0.70, t, displacement, normal);
    addGerstnerWave(p, vec2(0.454, 0.891), 1130.0, 0.221 * height, 0.70, t, displacement, normal);
    addGerstnerWave(p, vec2(0.956, 0.292), 770.0, 0.157 * height, 0.65, t, displacement, normal);
    addGerstnerWave(p, vec2(0.208, 0.978), 530.0, 0.112 * height, 0.60, t, displacement, normal);
    addGerstnerWave(p, vec2(0.999, 0.035), 370.0, 0.081 * height, 0.55, t, displacement, normal);
    addGerstnerWave(p, vec2(-0.139, 0.990), 270.0, 0.061 * height, 0.50, t, displacement, normal);
    addGerstnerWave(p, vec2(0.857, 0.515), 190.0, 0.044 * height, 0.45, t, displacement, normal);
}
#endif

void main(void)
{
    vec4 vertex = gl_Vertex;
    vec3 vertexNormal = gl_Normal;
#if @waves
    waveNormal = vec3(0.0, 0.0, 1.0);
    if (waveSurface)
    {
        vertex.xy += waveGridOffset;
        vec2 world = vertex.xy + nodePosition.xy;
        // flat at the grid's edge (where it meets the plane) ...
        vec2 edge = abs(gl_Vertex.xy) / @waveGridHalfSize;
        float height = 1.0 - smoothstep(0.6, 0.97, max(edge.x, edge.y));
        // ... in shallow water ...
        // (texture2D in a vertex stage reads the base level; the map has no mips)
        height *= smoothstep(0.0, 1.0, texture2D(waveDepthMap, (world - waveDepthMapRect.xy) * waveDepthMapRect.z).r);
        // ... and around a camera at the waterline (swimming, wading), so the surface doesn't cut through the view
        vec3 eye = (gl_ModelViewMatrixInverse * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
        height *= clamp(max(abs(eye.z) / (2.0 * waveAmplitude + 1.0), length(vertex.xy - eye.xy) / 600.0), 0.0, 1.0);

        vec3 displacement;
        vec3 normal;
        computeWaves(world, osg_SimulationTime, waveAmplitude * height, displacement, normal);
        vertex.xyz += displacement;
        waveNormal = normalize(normal);
        vertexNormal = waveNormal;
    }
#endif

    gl_Position = modelToClip(vertex);

    position = vertex;

    worldPos = position.xyz + nodePosition.xyz;
    rippleMapUV = (worldPos.xy - playerPos.xy + (@rippleMapSize * @rippleMapWorldScale / 2.0)) / @rippleMapSize / @rippleMapWorldScale;

    vec4 viewPos = modelToView(vertex);
    passViewPos = viewPos.xyz;
    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);

    setupShadowCoords(viewPos, normalize((gl_NormalMatrix * vertexNormal).xyz));

    mgeWxEmitVaryings(); // weather verdict hoist (mge_fog.glsl)
}
