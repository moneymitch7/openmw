#version 120

// OpenMGE XE: once-per-frame weather verdict pass, see mge_weather.frag.
// The quads are given in clip space.
void main()
{
    gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0);
}
