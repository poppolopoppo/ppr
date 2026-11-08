// ------------------------------------------------------------------
// scalar immutable native terrain and sparse optical cell protocol
// ------------------------------------------------------------------
#ifndef PPR_WORLD_MATTER_TYPES_SLANG
#define PPR_WORLD_MATTER_TYPES_SLANG
struct WorldMaterial {
    float r,g,b;uint kind;
    float sigmaR,sigmaG,sigmaB,jR,jG,jB,albedoR,albedoG,albedoB;
};
struct WorldOverlay {
    uint cell;float coverage;
    float r,g,b,albedoR,albedoG,albedoB,sigmaR,sigmaG,sigmaB,jR,jG,jB;
};
struct WorldParticleMaterial { uint id;WorldMaterial value; };
#endif
