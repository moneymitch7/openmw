#include "lib/light/struct.glsl"

struct LightGrid {
    uint offset;
    uint count;
};

struct Cluster {
    vec4 minPoint;
    vec4 maxPoint;
};

layout(std430, binding = 1) restrict buffer clusterSSBO {
    Cluster clusters[];
};

layout(std430, binding = 2) restrict buffer pointLightSSBO {
    PointLight pointLight[];
};

layout(std430, binding = 3) restrict buffer lightGridSSBO {
    LightGrid lightGrid[];
};

layout(std430, binding = 4) restrict buffer lightIndexListSSBO {
    uint lightIndexList[];
};

layout(std430, binding = 5) restrict buffer lightIndexCounterSSBO {
    uint globalLightIndexCount;
};

uniform DirectionalLight sun;

// How much of each light of pointLight[] the world keeps from the object being drawn, 4 bits a light (bits
// 4 * (i % 8) of word i / 8 for light i): 0 none, all of the light reaches the object; 15 all, the light is hidden.
// See LightListCallback.
uniform uvec4 lightShade[32];

// The share of light index the object gets, 0 to 1.
float lightVisibility(uint index)
{
    if (index >= 1024u)
        return 1.0;
    uint word = index >> 3u;
    uint shade = (lightShade[word >> 2u][word & 3u] >> ((index & 7u) << 2u)) & 15u;
    return 1.0 - float(shade) / 15.0;
}
