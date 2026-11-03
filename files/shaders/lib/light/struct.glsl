#ifndef LIB_LIGHT_STRUCT
#define LIB_LIGHT_STRUCT

struct DirectionalLight {
    vec4 position;
    vec4 diffuse;
    vec4 ambient;
    vec4 specular;
};

struct PointLight {
    vec4 position;
    vec4 diffuse;
    vec4 ambient;
    vec4 specular;
    float constant;
    float linear;
    float quadratic;
    float radius;
    // lamp and lantern shadows: x a lamp's first still face layer + 1, y its first live face layer + 1 (0: none),
    // z the far end of its faces; w 1 for a carried light whose shadows are drawn (see lib/light/util.glsl)
    vec4 shadow;
};

#endif
