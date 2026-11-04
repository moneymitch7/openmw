#version 120

#include "lib/core/vertex.h.glsl"

#include "lib/sky/passes.glsl"

uniform int pass;
uniform mat4 texMat0;

varying vec4 passColor;
varying vec2 diffuseMapUV;
#if @mgeFog
varying vec3 passViewPos;
#endif

void main()
{
#if @mgeFog
    // [Fog] mge fog: the atmosphere band (sky_atmosphere.nif, rings at about 3.6 and 26.7 degrees) becomes a closed
    // dome, its lower ring well below the horizon and its upper ring drawn in to the zenith, since sky.frag colours
    // it by direction. The ring's sign is kept, so it works whether or not the mesh's flip was baked in.
    vec4 vertex = gl_Vertex;
    if (pass == PASS_ATMOSPHERE)
    {
        if (abs(vertex.z) > 400.0)
        {
            vertex.xy *= 0.02;
            vertex.z = sign(vertex.z) * 6000.0;
        }
        else
            vertex.z = -sign(vertex.z) * 1200.0;
    }
    gl_Position = modelToClip(vertex);
    passViewPos = (gl_ModelViewMatrix * vertex).xyz;
#else
    gl_Position = modelToClip(gl_Vertex);
#endif
    passColor = gl_Color;

    if (pass == PASS_CLOUDS)
        diffuseMapUV = (texMat0 * gl_MultiTexCoord0).xy;
    else
        diffuseMapUV = gl_MultiTexCoord0.xy;
}
