// Selected ErikSom/pbmpm 859c449 numerical/storage core.
// BSD-3-Clause notice: LICENSE.md. Arithmetic expressions are retained verbatim.
// Engine-only hooks preserve the no-hook conformance order.
struct Hot {
    float px, py, dx, dy; SourceMatrix F, D; float J, logJp, lambda, enabled;
#ifdef PBMPM_ENGINE_PARTICLE_ROWS
    float compaction, solidFraction;
#endif
};
#if defined(PBMPM_ENGINE_PARTICLE_ROWS) && defined(PBMPM_CONFORMANCE_PARTICLE_ROWS)
#error Select exactly one PBMPM particle row ABI
#elif defined(PBMPM_ENGINE_PARTICLE_ROWS)
struct Cold { float mass, volume, material, r, g, b; uint propertyRow; };
#elif defined(PBMPM_CONFORMANCE_PARTICLE_ROWS)
struct Cold { float mass, volume, material, r, g, b; };
#else
#error Explicit PBMPM particle row ABI selection required
#endif
#if (defined(PBMPM_ENGINE_ADAPTERS) || defined(PBMPM_SPARSE_STORAGE)) && !defined(PBMPM_ENGINE_PARTICLE_ROWS)
#error Engine solver and maintenance require the engine particle row ABI
#endif
struct Bucket { uint start, count, x, y; };
ConstantBuffer<Parameters> g_parameters;
#ifdef PBMPM_ENGINE_ADAPTERS
struct RadialForce { float centerX, centerY, radius, acceleration; };
ConstantBuffer<RadialForce> g_radial_force;
// Immutable per-substance rows; referenceDensity describes physical inventory
// and deliberately does not rescale the normalized liquid state.
struct MaterialProperties {
    uint substanceId, family;
    float elasticityRatio, elasticRelaxation, liquidRelaxation, liquidViscosity;
    float frictionAngle, plasticity, referenceDensity, sandYieldRatio;
    float cohesion, cap, tension, cohesionHardening, capHardening;
    float solidFraction0, compactionRate, solidExponent, denseEpsilon;
};
StructuredBuffer<MaterialProperties> g_properties;
#define PBMPM_COEFFICIENTS coefficients
#else
#define PBMPM_COEFFICIENTS g_parameters
#endif
RWStructuredBuffer<Hot> g_hot;
StructuredBuffer<Cold> g_cold;
#ifdef PBMPM_ENGINE_ADAPTERS
MaterialProperties particleProperties(uint row) {
    uint index = g_cold[row].propertyRow;
    if (index != 0xffffffffu) return g_properties[index];

    // Explicit profile-only compatibility path for source/Phase A fixtures.
    MaterialProperties result = {0u, uint(g_cold[row].material),
        g_parameters.elasticityRatio, g_parameters.elasticRelaxation,
        g_parameters.liquidRelaxation, g_parameters.liquidViscosity,
        g_parameters.frictionAngle, g_parameters.plasticity, 1.0,
        g_parameters.elasticityRatio, 0.05, 0.2, 0.02, 0.2, 0.5, 0.3, 0.5, 2.0, 0.01};
    return result;
}
#endif
StructuredBuffer<StaticShape> g_shapes;
#ifndef PBMPM_SPARSE_STORAGE
StructuredBuffer<int> g_gridSrc;
RWStructuredBuffer<int> g_gridDst;
RWStructuredBuffer<uint> g_budget;
#endif
RWStructuredBuffer<uint> g_errors;
#ifndef PBMPM_SPARSE_STORAGE
RWStructuredBuffer<uint> g_counts;
RWStructuredBuffer<uint> g_insertCounts;
RWStructuredBuffer<uint> g_starts;
#endif
RWStructuredBuffer<uint> g_rows;
#ifndef PBMPM_SPARSE_STORAGE
RWStructuredBuffer<uint> g_allocator;
RWStructuredBuffer<uint> g_args;
#endif
RWStructuredBuffer<Bucket> g_buckets;
groupshared int s_src[256];
groupshared int s_dst[256];

Particle loadParticle(uint row) {
    Hot h = g_hot[row];
    Cold c = g_cold[row];
    Particle p = {h.px,h.py,h.dx,h.dy,h.F,h.D,h.J,c.mass,c.material,c.volume,h.lambda,h.logJp,c.r,c.g,c.b,h.enabled,0.0,0.3};
#ifdef PBMPM_ENGINE_PARTICLE_ROWS
    p.compaction = h.compaction;
    p.solidFraction = h.solidFraction;
#endif

    return p;
}

void saveParticle(uint row, Particle p) {
    Hot h = {p.positionX,p.positionY,p.displacementX,p.displacementY,p.deformationGradient,
        p.deformationDisplacement,p.liquidDensity,p.logJp,p.lambda,p.enabled
#ifdef PBMPM_ENGINE_PARTICLE_ROWS
        ,p.compaction,p.solidFraction
#endif
    };
    g_hot[row] = h;
}

int checkedEncode(float value, uint row, uint field) {
    float scaled = value*float(g_parameters.fixedPointMultiplier);
    if (!isfinite(scaled) || scaled<=-2147483648.0 || scaled>=2147483648.0) {
        fail(2u,row,field); return 0;
    }

    return int(scaled);
}

// WGSL2026-10-21 section15.7.6: finite f32-to-i32 endpoints must also be
// representable as f32. Only the nonaccumulating source node stores use this.
// Scatter retains checkedEncode and its separate absolute-sum proof below.
int checkedEncodeNode(float value, uint node, uint field) {
    float scaled = value*float(g_parameters.fixedPointMultiplier);
    if (!isfinite(scaled)) {
        failNode(node,field);
        return 0;
    }

    return int(clamp(scaled,-2147483648.0,2147483520.0));
}

float decodeWord(int value) { return float(value)/float(g_parameters.fixedPointMultiplier); }

#ifndef PBMPM_SPARSE_STORAGE
void scatterChecked(uint localAddress, uint globalAddress, float value, uint row, uint field) {
    int encoded = checkedEncode(value,row,field);
    uint magnitude = uint(encoded<0 ? -encoded : encoded);
    uint before;
    InterlockedAdd(g_budget[globalAddress],0u,before);
    for (;;) {
        if (magnitude>0x7fffffffu-before) {
#ifdef PBMPM_ENGINE_ADAPTERS
            recordError(4u,globalAddress/4u,field,row);
#else
            fail(4u,row,field);
#endif
            return;
        }

        uint observed;
        InterlockedCompareExchange(g_budget[globalAddress],before,before+magnitude,observed);
        if (observed==before) break;
        before=observed;
    }

    // Global absolute reservation precedes the local signed sum. Every shared
    // subset and the eventual global merge are bounded by the same INT_MAX proof.
    InterlockedAdd(s_dst[localAddress],encoded);
}
#endif

SvdResult forkSvd(SourceMatrix m) {
    float E=(m.c0x+m.c1y)*0.5, F=(m.c0x-m.c1y)*0.5;
    float G=(m.c0y+m.c1x)*0.5, H=(m.c0y-m.c1x)*0.5;
    float Q=sqrt(E*E+H*H), R=sqrt(F*F+G*G);
    float a1=F==0 && G==0 ? 0.0 : atan2(G,F);
    float a2=E==0 && H==0 ? 0.0 : atan2(H,E);
    SvdResult result={rotation((a2+a1)*0.5),float2(Q+R,Q-R),rotation((a2-a1)*0.5)};
    return result;
}

Particle constraint(Particle p
#ifdef PBMPM_ENGINE_ADAPTERS
    , uint row
#endif
) {
#ifdef PBMPM_ENGINE_ADAPTERS
    MaterialProperties coefficients = particleProperties(row);
#endif
    SourceMatrix D = p.deformationDisplacement;
    if (p.material == MaterialLiquid) {
        SourceMatrix deviatoric = scale(add(D, transposed(D)), -1.0);
        D = add(D, scale(deviatoric, PBMPM_COEFFICIENTS.liquidViscosity * 0.5));

        float alpha = 0.5 * (1.0 / p.liquidDensity - trace(D) - 1.0);
        D = add(D, scale(identity(), PBMPM_COEFFICIENTS.liquidRelaxation * alpha));
    } else if (p.material == MaterialElastic || p.material == MaterialVisco || p.material == MaterialSand
#ifdef PBMPM_ENGINE_ADAPTERS
        || p.material == MaterialCohesive || p.material == MaterialCompacting
#endif
    ) {
        SourceMatrix F = product(add(identity(), D), p.deformationGradient);
        SvdResult decomposition = forkSvd(F);
        bool sand = p.material == MaterialSand;
#ifdef PBMPM_ENGINE_ADAPTERS
        bool calibrated = p.material == MaterialCohesive || p.material == MaterialCompacting;
        if (calibrated) {
            // Constraint-target coupling in calibration space, not a stress
            // derivation. Freeze history AND its strength during this projection;
            // only integration may advance C/phi.
            float C = p.compaction;
            float phi = p.solidFraction;
            if (!calibratedReturn(decomposition.Sigma, C, phi, p.logJp, coefficients, false)) {
                recordError(8u, 0xffffffffu, 22u, row);
                return p;
            }
        }
#endif
        if (sand && p.logJp == 0.0) {
            decomposition.Sigma = clamp(decomposition.Sigma, float2(1.0), float2(1000.0));
        }

        float df = determinant(F);
        float cdf = clamp(abs(df), 0.1, sand ? 1.0 : 1000.0);
        SourceMatrix Q = scale(F, 1.0 / (sign(df) * sqrt(cdf)));
        SourceMatrix target = sand
#ifdef PBMPM_ENGINE_ADAPTERS
                                  || calibrated
#endif
                                  ? product(product(decomposition.U, diagonal(decomposition.Sigma)), decomposition.Vt)
                                  : product(decomposition.U, decomposition.Vt);
        target = add(scale(target, PBMPM_COEFFICIENTS.elasticityRatio), scale(Q, 1.0 - PBMPM_COEFFICIENTS.elasticityRatio));
        SourceMatrix difference = subtract(subtract(product(target, inverted(p.deformationGradient)), identity()), D);
        D = add(D, scale(difference, PBMPM_COEFFICIENTS.elasticRelaxation));

        if (sand) {
            SourceMatrix deviatoric = scale(add(D, transposed(D)), -1.0);
            D = add(D, scale(deviatoric, PBMPM_COEFFICIENTS.liquidViscosity * 0.5));
        }
    }

    p.deformationDisplacement = D;
    return p;
}

#ifdef PBMPM_ENGINE_ADAPTERS
// exp() keeps the sand projection's singular value finite and normal between
// these arguments: exp(88) = 1.65e38 < FLT_MAX and exp(-87) = 1.6e-38 > 0.
static const float kMinSandExponent = -87.0;
static const float kMaxSandExponent = 88.0;

// ------------------------------------------------------------------
// adapter-only calibration-space cap and shear returns
// ------------------------------------------------------------------

// H is the legacy sand reservoir. New-family admission requires H=0 and
// this return leaves it zero; C and phi are separate irreversible histories.
// No stress mapping or dissipation claim is implied by these projections.
bool calibratedReturn(inout float2 sigma, inout float C, inout float phi,
                      float H, MaterialProperties a, bool updateHistory = true) {
    if (!all(isfinite(sigma)) || any(sigma <= 0.0) || H != 0.0 ||
        !isfinite(C) || C < 0.0 || !isfinite(phi) ||
        phi < a.solidFraction0 || phi > 1.0-a.denseEpsilon) return false;

    float2 e = log(sigma);
    float trial = e.x + e.y + H;
    float2 dev = e - trial*0.5;
    float n = length(dev);
    float sine = sin(a.frictionAngle / 180.0 * 3.14159);
    float denominator = 3.0-sine;
    if (!isfinite(trial) || !isfinite(n) || !isfinite(denominator) || denominator <= 0.0) return false;

    float alpha = sqrt(2.0/3.0)*2.0*sine/denominator;
    float cohesion = a.cohesion;
    float cap = a.cap;
    float nextC = C;
    float nextPhi = phi;
    float returned = min(trial, a.tension);
    bool porous = a.family == uint(MaterialCompacting);
    bool dense = porous && phi >= 1.0-a.denseEpsilon;
    if (!porous) {
        cap += a.capHardening*C;
        if (!isfinite(cap) || cap <= 0.0) return false;

        returned = max(returned, -cap);
        if (updateHistory) nextC = max(C, -min(returned, 0.0));
        cohesion += a.cohesionHardening*nextC;
    } else {
        if (!dense) {
            float voidFraction = 1.0-phi;
            if (!isfinite(voidFraction) || voidFraction <= 0.0) return false;

            cap = a.cap/voidFraction;
            if (!isfinite(cap) || cap <= 0.0) return false;

            returned = max(returned, -cap);
            if (updateHistory) {
                float increment = a.compactionRate*max(0.0, returned-trial);
                if (!isfinite(increment)) return false;

                nextPhi = clamp(phi+increment, a.solidFraction0, 1.0-a.denseEpsilon);
            }
        }

        dense = nextPhi >= 1.0-a.denseEpsilon;
        float strengthPhi = dense ? 1.0-a.denseEpsilon : nextPhi;
        if (!isfinite(strengthPhi) || strengthPhi <= 0.0 || strengthPhi >= 1.0) return false;

        // pow on a positive base <=1 with nonnegative finite exponent cannot overflow.
        cohesion *= pow(strengthPhi, a.solidExponent);
        if (!dense) {
            float voidFraction = 1.0-nextPhi;
            if (!isfinite(voidFraction) || voidFraction <= 0.0) return false;

            cap = a.cap/voidFraction;
            if (!isfinite(cap) || cap <= 0.0) return false;
        }
    }

    float edge = alpha*max(-returned, 0.0)+cohesion;
    if (!isfinite(cohesion) || !isfinite(edge) || edge < 0.0) return false;

    if (porous && !dense && returned <= 0.0) {
        // The cap denominator is guarded above, before this division.
        float ratio = -returned/cap;
        if (!isfinite(ratio) || ratio < 0.0 || ratio > 1.0) return false;

        edge *= sqrt(max(0.0, 1.0-ratio*ratio));
    }

    if (n > edge) {
        if (!isfinite(n) || n <= 0.0) return false;

        dev *= edge/n;
    }

    float2 projected = dev + returned*0.5;
    if (!all(isfinite(projected)) || any(projected < kMinSandExponent) ||
        any(projected > kMaxSandExponent)) return false;

    sigma = exp(projected);
    C = nextC;
    phi = nextPhi;
    return true;
}
#endif
Particle integration(Particle p, uint row) {
#ifdef PBMPM_ENGINE_ADAPTERS
    MaterialProperties coefficients = particleProperties(row);
#endif
    if (p.material == MaterialLiquid) {
        p.liquidDensity *= trace(p.deformationDisplacement) + 1.0;
        p.liquidDensity = max(p.liquidDensity, 0.05);
    } else {
#ifdef PBMPM_ENGINE_ADAPTERS
        // Adapter-only operand snapshot: rejecting a derived operand leaves this
        // particle's step state unchanged. The source path keeps its arithmetic.
        SourceMatrix stepGradient = p.deformationGradient;
        float stepLogJp = p.logJp;
#endif
        p.deformationGradient = product(add(identity(), p.deformationDisplacement), p.deformationGradient);
        SvdResult decomposition = forkSvd(p.deformationGradient);
        bool calibrated = false;
#ifdef PBMPM_ENGINE_ADAPTERS
        calibrated = p.material == MaterialCohesive || p.material == MaterialCompacting;
        if (calibrated) {
            if (!calibratedReturn(decomposition.Sigma, p.compaction, p.solidFraction, p.logJp, coefficients)) {
                p.deformationGradient = stepGradient;
                recordError(8u, 0xffffffffu, 22u, row);
                return p;
            }
        }
#endif
        if (!calibrated) decomposition.Sigma = clamp(decomposition.Sigma, float2(0.1), float2(10000.0));
        if (p.material == MaterialSand) {
            float sinPhi = sin(PBMPM_COEFFICIENTS.frictionAngle / 180.0 * 3.14159);
            float alpha = sqrt(2.0 / 3.0) * 2.0 * sinPhi / (3.0 - sinPhi);
            float2 eDiag = log(max(abs(decomposition.Sigma), float2(1e-6)));
            float traceE = eDiag.x + eDiag.y + p.logJp;
            float2 eHat = eDiag - traceE / 2.0;
            float frobenius = length(eHat);
            if (traceE >= 0.0) {
                decomposition.Sigma = float2(1.0);
                p.logJp = 0.5 * traceE;
            } else {
                p.logJp = 0.0;
#ifdef PBMPM_ENGINE_ADAPTERS
                // Compatibility calibration, not a Lamé modulus ratio.
                float deltaGamma = frobenius + (coefficients.sandYieldRatio + 1.0) * traceE * alpha;
#else
                float deltaGamma = frobenius + (g_parameters.elasticityRatio + 1.0) * traceE * alpha;
#endif
#ifdef PBMPM_ENGINE_ADAPTERS
                // Reject NaN before the branch can silently bypass projection.
                if (!isfinite(deltaGamma)) {
                    p.deformationGradient = stepGradient;
                    p.logJp = stepLogJp;
                    recordError(8u, 0xffffffffu, 17u, row);
                    return p;
                }
#endif
                if (deltaGamma > 0.0) {
#ifdef PBMPM_ENGINE_ADAPTERS
                    // The projection divides by the deviatoric norm and then
                    // exponentiates that ratio. A huge but finite logJp makes
                    // length() overflow, and an out-of-domain exponent has no
                    // finite positive exp(); both are rejected here.
                    if (!isfinite(frobenius) || frobenius == 0.0) {
                        p.deformationGradient = stepGradient;
                        p.logJp = stepLogJp;
                        recordError(8u, 0xffffffffu, 17u, row);
                        return p;
                    }

                    float projectedX = eDiag.x - deltaGamma / frobenius * (eDiag.x - traceE * 0.5);
                    float projectedY = eDiag.y - deltaGamma / frobenius * (eDiag.y - traceE * 0.5);
                    if (!isfinite(projectedX) || !isfinite(projectedY) ||
                        projectedX < kMinSandExponent || projectedX > kMaxSandExponent ||
                        projectedY < kMinSandExponent || projectedY > kMaxSandExponent) {
                        p.deformationGradient = stepGradient;
                        p.logJp = stepLogJp;
                        recordError(8u, 0xffffffffu, 17u, row);
                        return p;
                    }
#endif
                    decomposition.Sigma = exp(eDiag - deltaGamma / frobenius * (eDiag - traceE * 0.5));
                }
            }
        } else if (p.material == MaterialVisco) {
            float yieldSurface = exp(1.0 - PBMPM_COEFFICIENTS.plasticity);
            float J = decomposition.Sigma.x * decomposition.Sigma.y;
#ifdef PBMPM_ENGINE_ADAPTERS
            // Profile admission already bounds yieldSurface; a nonfinite or
            // vanishing derived operand still has no usable plastic state.
            if (!isfinite(yieldSurface) || yieldSurface <= 0.0 || !isfinite(J) || J == 0.0) {
                p.deformationGradient = stepGradient;
                recordError(8u, 0xffffffffu, 16u, row);
                return p;
            }

            if (!isfinite(1.0 / yieldSurface)) {
                p.deformationGradient = stepGradient;
                recordError(8u, 0xffffffffu, 16u, row);
                return p;
            }
#endif
            decomposition.Sigma = clamp(decomposition.Sigma, float2(1.0 / yieldSurface), float2(yieldSurface));
            float newJ = decomposition.Sigma.x * decomposition.Sigma.y;
#ifdef PBMPM_ENGINE_ADAPTERS
            // The plastic step multiplies by sqrt(J / newJ). Profile admission
            // bounds yieldSurface, but the quotient itself can still overflow for
            // an admitted profile (for example plasticity 41 leaves newJ ~ 1.8e-35
            // while trial singular values give J ~ 1e8), so it is validated here.
            if (!isfinite(newJ) || newJ == 0.0) {
                p.deformationGradient = stepGradient;
                recordError(8u, 0xffffffffu, 16u, row);
                return p;
            }

            float plasticQuotient = J / newJ;
            if (!isfinite(plasticQuotient)) {
                p.deformationGradient = stepGradient;
                recordError(8u, 0xffffffffu, 16u, row);
                return p;
            }
#endif
            decomposition.Sigma *= sqrt(J / newJ);
        }

        p.deformationGradient = product(product(decomposition.U, diagonal(decomposition.Sigma)), decomposition.Vt);
    }

    float2 position=float2(p.positionX,p.positionY)+float2(p.displacementX,p.displacementY);
#ifdef PBMPM_ENGINE_ADAPTERS
    // Reject before any terrain/stencil index conversion; no stabilization.
    if (!all(isfinite(position))) {
        recordError(8u,0xffffffffu,0xffffffffu,row);
        return p;
    }
#endif
    float2 d=float2(p.displacementX,p.displacementY);
    d.y-=float(g_parameters.gridHeight)*g_parameters.gravityStrength*g_parameters.deltaTime*g_parameters.deltaTime;
#ifdef PBMPM_ENGINE_ADAPTERS
    // The source/no-force path retains its arithmetic. Acceleration enters only
    // integration (iteration1), after gather and before endpoint constraints.
    if (g_radial_force.acceleration != 0.0) {
        float2 offset = float2(p.positionX, p.positionY) - float2(g_radial_force.centerX, g_radial_force.centerY);
        float squaredDistance = dot(offset, offset);
        if (squaredDistance > 0.0 && squaredDistance < g_radial_force.radius * g_radial_force.radius) {
            float distance = sqrt(squaredDistance);
            float falloff = 1.0 - distance / g_radial_force.radius;
            float stepAcceleration = g_radial_force.acceleration * g_parameters.deltaTime * g_parameters.deltaTime;
            d += (offset / distance) * (falloff * falloff * stepAcceleration);
        }
    }
#endif

    for (uint i=0;i<g_parameters.shapeCount;++i) {
        Contact contact=collide(g_shapes[i],position);
        if (contact.collides) d-=contact.penetration*contact.normal;
    }

#ifdef PBMPM_ENGINE_ADAPTERS
    // Terrain transport/recovery is geometric and must not enter F/D/J or be
    // added to next-step motion. The no-adapter conformance path is unchanged.
    engineEndpointContacts(float2(p.positionX,p.positionY),float2(p.displacementX,p.displacementY),position,d,row);
#endif
    position=guardian(position,uint2(g_parameters.gridWidth,g_parameters.gridHeight),3.0);
#ifdef PBMPM_ENGINE_ADAPTERS
    terrainValidatePosition(position,row);
#endif
    p.positionX=position.x; p.positionY=position.y; p.displacementX=d.x; p.displacementY=d.y;
    return p;
}
