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

void main(void)
{
    gl_Position = modelToClip(gl_Vertex);

    position = gl_Vertex;

    worldPos = position.xyz + nodePosition.xyz;
    rippleMapUV = (worldPos.xy - playerPos.xy + (@rippleMapSize * @rippleMapWorldScale / 2.0)) / @rippleMapSize / @rippleMapWorldScale;

    vec4 viewPos = modelToView(gl_Vertex);
    passViewPos = viewPos.xyz;
    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);

    setupShadowCoords(viewPos, normalize((gl_NormalMatrix * gl_Normal).xyz));

    mgeWxEmitVaryings(); // weather verdict hoist (mge_fog.glsl)
}
