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
    // unused (was a held light's carrier body)
    vec4 carrierFoot;
    // w: a held light's bounce reach (see calcPointLightBounce in lib/light/util.glsl)
    vec4 carrierAxis;
};

#endif
