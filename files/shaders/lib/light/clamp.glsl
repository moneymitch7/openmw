// OpenMGE XE: per-subgraph clamp override. Actor faces and armour carry their
// contrast baked into the texture and clip badly at light peaks. The engine sets
// uClampLightingActor = 1 on actor roots (NPCs, creatures, the player) and 0 on the
// scene root; uClampLightingActorsGate is the live [Shaders] 'clamp lighting actors'
// switch. With 'clamp lighting' off, actors can still be clamped while the world
// keeps the unclamped (MGE) curve.
uniform float uClampLightingActor;
uniform float uClampLightingActorsGate;

void clampLighting(inout vec3 lighting)
{
#if @clamp
    lighting = clamp(lighting, vec3(0.0), vec3(1.0));
#else
    if (uClampLightingActor > 0.5 && uClampLightingActorsGate > 0.5)
        lighting = clamp(lighting, vec3(0.0), vec3(1.0));
    else
        lighting = max(lighting, 0.0);
#endif
}
