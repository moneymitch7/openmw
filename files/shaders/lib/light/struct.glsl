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
    // a held light's carrier as an upright cylinder (see carrierShade in lib/light/util.glsl): xyz its axis at the
    // feet, w its radius (0: none); then the axis from feet to head
    vec4 carrierFoot;
    vec4 carrierAxis;
};

#endif
