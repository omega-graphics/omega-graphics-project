// AQUA Phase 7 — XPBD constraint-projection core (§13 7b/7c/7d). Split out of
// AQSpace.cpp so the deformable pillar lives in its own translation unit while
// sharing the one AQSpace::Impl definition (AQSpaceImpl.h) — the same split
// AQSpaceParticles.cpp made for Phase 6.
//
// Three responsibilities land here:
//   * storage + authoring — persistent particle SoA, typed distance-constraint
//     array, the public AQSpace authoring surface;
//   * deterministic graph coloring — greedy smallest-free-color over the
//     constraint graph (two constraints conflict iff they share a particle),
//     visited in authoring order so the coloring is a pure function of what
//     the caller authored (stable-cross-path by construction); the color-
//     sorted mirror is the contiguous-per-color layout the GPU port reads
//     coalesced, and the CPU solve walks it in the SAME fixed order;
//   * the solve — per engine sub-step, n XPBD slices of h = dt/n, each slice
//     predict → (iterations ×) colored projection → derive-velocity, with the
//     loud explosion guard (brief §6). The scalar math lives in AQXPBDMath.h,
//     shared verbatim with the double reference oracle in the rope test.

#include "AQSpaceImpl.h"
#include "AQXPBDMath.h"
#include "AQParticleCollision.h"  // Phase 7g — the scalar-generic point-vs-shape SDF
#include "AQComputeBackend.h"   // Phase 7f — the live GPU XPBD path

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>

// ---------------------------------------------------------------------------
// AQXPBDBody — storage, authoring, coloring, and the slice loop.
// ---------------------------------------------------------------------------

std::uint32_t AQXPBDBody::addParticles(const OmegaGTE::FVec<3>* pos, const float* w,
                                       std::uint32_t count) {
    const std::uint32_t first = static_cast<std::uint32_t>(positions.size());
    const FVec<3> zero = FVec<3>::Create();
    for (std::uint32_t i = 0; i < count; ++i) {
        positions.push_back(pos[i]);
        prevPositions.push_back(pos[i]);
        velocities.push_back(zero);
        // Null invMass means "all unit mass"; negatives are a malformed pin —
        // clamp to 0 (pinned) rather than letting a negative mass explode.
        const float wi = w ? w[i] : 1.f;
        invMass.push_back(wi > 0.f ? wi : 0.f);
    }
    gpuUploadNeeded = true;   // 7f: the device particle SoA must be re-seeded
    return first;
}

std::uint32_t AQXPBDBody::addDistance(std::uint32_t a, std::uint32_t b,
                                      float restLength, float compliance) {
    AQDistanceConstraint c;
    c.a = a;
    c.b = b;
    c.restLength = restLength;
    c.compliance = compliance;
    c.lambda = 0.f;
    c.color = 0;                       // assigned by recolor()
    const std::uint32_t idx = static_cast<std::uint32_t>(distance.size());
    distance.push_back(c);
    colorsDirty = true;
    return idx;
}

void AQXPBDBody::recolor() {
    const std::uint32_t nC = static_cast<std::uint32_t>(distance.size());
    const std::uint32_t nP = static_cast<std::uint32_t>(positions.size());

    // Per-particle incidence lists: which constraints touch particle p. Built
    // once per topology change; the coloring below only ever consults already-
    // colored constraints, so authoring order fully determines the result.
    OmegaCommon::Vector<OmegaCommon::Vector<std::uint32_t>> incident(nP);
    for (std::uint32_t ci = 0; ci < nC; ++ci) {
        incident[distance[ci].a].push_back(ci);
        incident[distance[ci].b].push_back(ci);
    }

    constexpr std::uint32_t kUncolored = 0xFFFFFFFFu;
    OmegaCommon::Vector<std::uint32_t> colorOf(nC, kUncolored);
    std::uint32_t colorCount = 0;

    OmegaCommon::Vector<std::uint8_t> used;
    for (std::uint32_t ci = 0; ci < nC; ++ci) {
        // Colors already taken by any constraint sharing particle a or b.
        used.assign(colorCount + 1u, 0u);
        const std::uint32_t ends[2] = { distance[ci].a, distance[ci].b };
        for (std::uint32_t e = 0; e < 2; ++e) {
            for (std::uint32_t cj : incident[ends[e]]) {
                const std::uint32_t col = colorOf[cj];
                if (col != kUncolored) used[col] = 1u;
            }
        }
        std::uint32_t chosen = 0;
        while (chosen < colorCount && used[chosen]) ++chosen;
        colorOf[ci] = chosen;
        distance[ci].color = chosen;
        if (chosen == colorCount) ++colorCount;
    }

    // Color-sorted mirror in (color, authoring index) total order — ties are
    // impossible by construction, so the layout is deterministic. λ resets to
    // 0 in the fresh mirror (it is per-slice solver state anyway).
    distanceSorted.clear();
    sortedAuthoring.clear();
    batches.clear();
    for (std::uint32_t col = 0; col < colorCount; ++col) {
        AQConstraintBatch batch;
        batch.type = AQConstraintDistance;
        batch.color = col;
        batch.firstConstraint = static_cast<std::uint32_t>(distanceSorted.size());
        for (std::uint32_t ci = 0; ci < nC; ++ci) {
            if (colorOf[ci] != col) continue;
            AQDistanceConstraint c = distance[ci];
            c.lambda = 0.f;
            distanceSorted.push_back(c);
            sortedAuthoring.push_back(ci);
        }
        batch.constraintCount =
            static_cast<std::uint32_t>(distanceSorted.size()) - batch.firstConstraint;
        batches.push_back(batch);
    }

    buildLongRange();     // §13.7 7h #1 — recompute geodesic attachments (or clear)
    colorsDirty = false;
}

// §13.7 7h #1 — long-range attachments (Kim/Chentanez/Müller 2012). Multi-source
// BFS from every pinned particle over the distance-constraint graph, relaxing on
// the accumulated rest-length distance, gives each dynamic particle its nearest
// pin and the geodesic max distance it may hang from that pin. A pure function of
// the authored topology (rest lengths + pins), so it is deterministic and rebuilt
// only here (on topology change), never per frame. Cleared when the feature is off
// or the body has no pins.
void AQXPBDBody::buildLongRange() {
    lra.clear();
    if (!longRangeAttach) return;

    const std::uint32_t nP = static_cast<std::uint32_t>(positions.size());
    if (nP == 0) return;

    // Incidence: the neighbors of each particle and the rest length of the edge.
    OmegaCommon::Vector<OmegaCommon::Vector<std::pair<std::uint32_t, float>>> adj(nP);
    for (const AQDistanceConstraint& c : distance) {
        adj[c.a].push_back({c.b, c.restLength});
        adj[c.b].push_back({c.a, c.restLength});
    }

    // Dijkstra-style relaxation on a nonnegative (rest-length) metric. The graph
    // is tiny (a rope/cloth), so a simple settle loop over a dense best[] is both
    // deterministic and adequate — no priority queue needed.
    constexpr float kInf = std::numeric_limits<float>::infinity();
    OmegaCommon::Vector<float>         best(nP, kInf);
    OmegaCommon::Vector<std::uint32_t> nearestPin(nP, 0u);
    OmegaCommon::Vector<std::uint8_t>  isPin(nP, 0u);
    bool anyPin = false;
    for (std::uint32_t i = 0; i < nP; ++i) {
        if (invMass[i] == 0.f) {           // pinned seed
            best[i] = 0.f;
            nearestPin[i] = i;
            isPin[i] = 1u;
            anyPin = true;
        }
    }
    if (!anyPin) return;                    // nothing to attach to

    // Relax until no distance improves. Constraints touch few particles, so this
    // converges in O(diameter) passes; the total-order tie-break (smaller pin
    // index wins an equal distance) keeps the result path-independent.
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::uint32_t u = 0; u < nP; ++u) {
            if (best[u] == kInf) continue;
            for (const auto& e : adj[u]) {
                const std::uint32_t v = e.first;
                const float nd = best[u] + e.second;
                if (nd < best[v] - 1e-9f ||
                    (std::abs(nd - best[v]) <= 1e-9f && nearestPin[u] < nearestPin[v])) {
                    best[v] = nd;
                    nearestPin[v] = nearestPin[u];
                    changed = true;
                }
            }
        }
    }

    // Emit one attachment per dynamic, pin-reachable particle (skip pins and
    // unreachable islands). maxDist is the geodesic rest-length sum to the pin.
    for (std::uint32_t i = 0; i < nP; ++i) {
        if (isPin[i] || best[i] == kInf) continue;
        AQLongRangeAttachment a;
        a.p = i;
        a.pin = nearestPin[i];
        a.maxDist = best[i];
        lra.push_back(a);
    }
}

// §13.7 7h #2 — keep positions/prevPositions as offsets from `body.origin` so the
// solve runs on small, precise values far from the world origin (the solver2d XPBD
// precision failure). A pure storage transform: the world position (origin+offset)
// and every difference (x−x_prev, xa−xb) are invariant under it, so with the flag
// OFF (origin stays 0) the path is byte-identical to pre-7h. Runs ONCE per advance
// (before the slice loop), so the whole loop shares one constant offset frame.
static void AQxpbdManageOrigin(AQXPBDBody& body, bool on, std::uint32_t nP) {
    constexpr float kRebaseThreshold = 512.f;  // re-center when offsets drift past this
    auto offsetCentroid = [&]() -> FVec<3> {
        double cx = 0.0, cy = 0.0, cz = 0.0;   // double sum ⇒ a precise anchor
        for (std::uint32_t i = 0; i < nP; ++i) {
            cx += body.positions[i][0][0];
            cy += body.positions[i][1][0];
            cz += body.positions[i][2][0];
        }
        const float inv = nP ? (1.f / static_cast<float>(nP)) : 0.f;
        return AQvec3(static_cast<float>(cx) * inv,
                      static_cast<float>(cy) * inv,
                      static_cast<float>(cz) * inv);
    };

    if (on && !body.originActive) {
        const FVec<3> c = offsetCentroid();    // positions are world here
        body.origin = c;
        for (std::uint32_t i = 0; i < nP; ++i) {
            body.positions[i]     = body.positions[i]     - c;
            body.prevPositions[i] = body.prevPositions[i] - c;
        }
        body.originActive = true;
    } else if (on && body.originActive) {
        const FVec<3> delta = offsetCentroid();
        if (std::sqrt(OmegaGTE::dot(delta, delta)) > kRebaseThreshold) {
            body.origin = body.origin + delta;
            for (std::uint32_t i = 0; i < nP; ++i) {
                body.positions[i]     = body.positions[i]     - delta;
                body.prevPositions[i] = body.prevPositions[i] - delta;
            }
        }
    } else if (!on && body.originActive) {
        for (std::uint32_t i = 0; i < nP; ++i) {  // fold origin back → world storage
            body.positions[i]     = body.positions[i]     + body.origin;
            body.prevPositions[i] = body.prevPositions[i] + body.origin;
        }
        body.origin = FVec<3>::Create();
        body.originActive = false;
    }
}

// §13.6 7g — resolve one XPBD body's particles against the rigid colliders for a
// single slice of size h (engine sub-step dt, for the penetration bias). World
// position = origin + offset (§13.7 7h #2 storage); the SDF query, the impulse,
// and the reaction all live in world space.
//
// This is a VELOCITY-LEVEL contact, not a raw position push-out: the normal
// impulse cancels the approaching relative velocity plus a capped Baumgarte
// penetration-recovery bias — so it is bounded by the approach speed, never by
// (penetration / h) which explodes when a fast body is caught after sinking a
// whole frame. On the particle side the impulse is realized as a POSITION shift
// (wp·λ·h·n) so the derive step (÷h) reproduces exactly the intended velocity
// change — staying inside the position-based framework. On the body side it is a
// real impulse. `colliders` is a MUTABLE per-body copy whose contact velocity is
// evolved as reactions land, so λ self-limits across the sub-step's slices (the
// body pose is frozen for the sub-step, so without this every slice would re-
// apply the full stop). TWO-WAY + momentum-conserving; static/kinematic
// colliders (dynamic == false) reproduce the Phase 6 one-way response.
static void AQxpbdCollide(AQXPBDBody& body,
                          OmegaCommon::Vector<AQXPBDCollider>& colliders,
                          float h) {
    constexpr float kBeta = 0.2f;         // split-impulse position-recovery fraction
    constexpr float kSlop = 0.001f;       // allowed penetration (anti-jitter)
    const float invH = 1.f / h;
    const std::uint32_t nP = static_cast<std::uint32_t>(body.positions.size());
    const float radius = body.particleRadius;
    const float mu = body.friction;
    const FVec<3> zero = FVec<3>::Create();

    for (std::uint32_t i = 0; i < nP; ++i) {
        const float wp = body.invMass[i];      // pinned (0) may still stop a body
        for (AQXPBDCollider& c : colliders) {
            const FVec<3> world = body.origin + body.positions[i];
            const AQShapeSampleT<float> sd =
                AQshapeSignedDistanceGeneric<float>(c.shape, world, c.xform);
            const float pen = radius - sd.distance;           // >0 ⇒ penetrating
            if (!(pen > 0.f)) continue;                       // outside (or NaN) → skip
            const FVec<3> n = sd.normal;                      // unit outward normal
            const FVec<3> contact = world - n * sd.distance;  // closest surface point

            // Effective inverse masses: the point particle (wp) and, for a dynamic
            // collider, the Catto-2005 body term wb = 1/m + (r×n)·I⁻¹(r×n).
            FVec<3> r = zero;
            float wb = 0.f;
            if (c.dynamic) {
                r = contact - c.com;
                const FVec<3> rxn = OmegaGTE::cross(r, n);
                wb = c.invMassBody + OmegaGTE::dot(rxn, c.worldInvInertia * rxn);
            }
            const float wsum = wp + wb;
            if (wsum <= 0.f) continue;                        // both effectively fixed

            // Current relative velocity at the contact (particle implied velocity
            // (x−x_prev)/h minus the body contact velocity vel + ω×r).
            const FVec<3> vp = (body.positions[i] - body.prevPositions[i]) * invH;
            const FVec<3> vb = c.dynamic ? (c.vel + OmegaGTE::cross(c.omega, r)) : zero;
            const FVec<3> vrel = vp - vb;
            const float vrelN = OmegaGTE::dot(vrel, n);

            // Split-impulse normal response (§13.6). VELOCITY: a pure inelastic
            // stop — drive vrelN to 0, never to a separating bias, so a resting
            // body is neither bounced off the surface nor left to creep through
            // it. POSITION: a separate pseudo-recovery (below) holds penetration.
            float lambdaN = -vrelN / wsum;
            if (lambdaN < 0.f) lambdaN = 0.f;                 // push apart only

            // Position-recovery magnitude: the penetration beyond the slop, taken
            // down by kBeta per sub-step. On the particle it is a real shift (its
            // inelastic push-out share); on the body it is a pseudo-position push
            // (no velocity ⇒ no energy injection).
            const float posCorr = kBeta * (pen > kSlop ? (pen - kSlop) : 0.f);
            const float pShare = wp / wsum;

            // Particle side: velocity stop (shift → derive) + its push-out share.
            if (wp > 0.f)
                body.positions[i] = body.positions[i]
                                  + n * (wp * lambdaN * h + pShare * posCorr);
            // Body side: velocity impulse −λ·n (evolve the snapshot + record) and
            // its pseudo-position share along −n (recorded; xpbdSubstep applies it
            // as a per-body MAX so a bed of contacts does not over-lift the body).
            if (c.dynamic) {
                const FVec<3> J = n * (-lambdaN);
                c.vel   = c.vel   + J * c.invMassBody;
                c.omega = c.omega + c.worldInvInertia * OmegaGTE::cross(r, J);
                AQXPBDReaction rec;
                rec.bodyIndex     = c.bodyIndex;
                rec.impulse       = J;
                rec.point         = contact;
                rec.posCorrection = n * (-(wb / wsum) * posCorr);
                body.reactionsThisStep.push_back(rec);
            }

            // Coulomb friction (opt-in μ>0): a tangential impulse that removes the
            // tangential relative velocity, clamped to μ·λN. Same realize-as-shift
            // on the particle, impulse on the body — momentum-conserving.
            if (mu > 0.f && lambdaN > 0.f) {
                const FVec<3> vt = vrel - n * vrelN;
                const float vtLen = std::sqrt(OmegaGTE::dot(vt, vt));
                if (vtLen > 1e-9f) {
                    const FVec<3> t = vt * (1.f / vtLen);
                    float lambdaT = vtLen / wsum;
                    const float maxT = mu * lambdaN;
                    if (lambdaT > maxT) lambdaT = maxT;
                    if (wp > 0.f) body.positions[i] = body.positions[i] - t * (wp * lambdaT * h);
                    if (c.dynamic) {
                        const FVec<3> Jt = t * lambdaT;   // drags the body along the slip
                        c.vel   = c.vel   + Jt * c.invMassBody;
                        c.omega = c.omega + c.worldInvInertia * OmegaGTE::cross(r, Jt);
                        AQXPBDReaction rec;
                        rec.bodyIndex = c.bodyIndex;
                        rec.impulse   = Jt;
                        rec.point     = contact;
                        body.reactionsThisStep.push_back(rec);
                    }
                }
            }
        }
    }
}

void AQXPBDBody::advance(float dt, const AQXPBDParams& params, const FVec<3>& gravity,
                         const OmegaCommon::Vector<AQXPBDCollider>& colliders) {
    if (positions.empty()) return;
    if (colorsDirty) recolor();

    reactionsThisStep.clear();          // §13.6 — fresh reaction buffer per sub-step

    const std::uint32_t nP = static_cast<std::uint32_t>(positions.size());

    // §13.7 7h #2 — origin frame for this advance (no-op when the flag is off).
    AQxpbdManageOrigin(*this, params.originRelative, nP);

    const std::uint32_t nSlices = params.substeps > 0u ? params.substeps : 1u;
    const std::uint32_t nIters  = params.iterations > 0u ? params.iterations : 1u;
    const float h = dt / static_cast<float>(nSlices);
    const float damping = params.velocityDamping;
    const float maxMove = params.explosionThreshold;
    const FVec<3> zero = FVec<3>::Create();
    const bool doCollide = collisionEnabled && !colliders.empty();
    // A mutable per-body copy: its contact velocity evolves across the slices so
    // the velocity-level impulse self-limits (§13.6). One copy per advance shared
    // by every slice, so a body slowed in slice 0 is seen slowed in slice 1.
    OmegaCommon::Vector<AQXPBDCollider> localColliders;
    if (doCollide) localColliders = colliders;

    for (std::uint32_t s = 0; s < nSlices; ++s) {
        // 1. predict — unconstrained integrate. Pinned lanes still refresh
        //    x_prev so a later unpin never derives a velocity from stale
        //    history; their velocity reads 0 by definition.
        for (std::uint32_t i = 0; i < nP; ++i) {
            if (invMass[i] > 0.f) {
                AQxpbdPredict(positions[i], prevPositions[i], velocities[i], gravity, h);
            } else {
                prevPositions[i] = positions[i];
            }
        }

        // 2. project — λ resets each slice, accumulates across the slice's
        //    iterations. Colors run serially; within a color the visit order
        //    is the fixed sorted order (bitwise-within-path determinism; on
        //    the GPU each color is one conflict-free parallel dispatch).
        for (auto& c : distanceSorted) c.lambda = 0.f;
        for (std::uint32_t iter = 0; iter < nIters; ++iter) {
            for (const auto& batch : batches) {
                const std::uint32_t end = batch.firstConstraint + batch.constraintCount;
                for (std::uint32_t k = batch.firstConstraint; k < end; ++k) {
                    AQDistanceConstraint& c = distanceSorted[k];
                    const bool tripped = AQxpbdProjectDistance(
                        positions[c.a], positions[c.b],
                        invMass[c.a], invMass[c.b],
                        c.restLength, c.compliance,
                        h, maxMove, c.lambda);
                    if (tripped) {
                        ++guardTrips;
                        trippedThisFrame.push_back(k);
                        if (!guardWarned) {
                            std::cerr << "AQUA::AQSpace[XPBD]: explosion guard tripped on body "
                                      << id << " — distance constraint #" << sortedAuthoring[k]
                                      << " (color " << c.color << ", particles " << c.a
                                      << "," << c.b << "): per-slice correction clamped to "
                                      << maxMove << " world units. The solve is diverging "
                                      << "(over-stiff rig, extreme mass ratio, or too-coarse "
                                      << "dt); further warnings for this body suppressed.\n";
                            guardWarned = true;
                        }
                    }
                }
            }
        }

        // 2b. §13.7 7h #1 — long-range attachment clamp. After the distance
        //     projection, so it arrests the residual over-stretch a 1-iteration
        //     solve leaves on a long pinned chain. The pin is fixed, so only the
        //     particle moves, and no two attachments share a dynamic particle ⇒
        //     order-independent (no coloring needed). Unilateral: only when the
        //     particle hangs beyond its geodesic reach.
        if (longRangeAttach) {
            for (const AQLongRangeAttachment& a : lra) {
                if (invMass[a.p] == 0.f) continue;
                const FVec<3> d = positions[a.p] - positions[a.pin];
                const float len = std::sqrt(OmegaGTE::dot(d, d));
                if (len > a.maxDist && len > 1e-12f) {
                    positions[a.p] = positions[a.pin] + d * (a.maxDist / len);
                }
            }
        }

        // 2c. §13.6 7g — rigid contact coupling (two-way). After constraints +
        //     LRA so the contact is the slice's final position correction; the
        //     derive below then reads the inelastic post-contact velocity.
        if (doCollide) AQxpbdCollide(*this, localColliders, h);

        // 3. derive — velocities from the net position change.
        for (std::uint32_t i = 0; i < nP; ++i) {
            if (invMass[i] > 0.f) {
                AQxpbdDeriveVelocity(positions[i], prevPositions[i], velocities[i],
                                     h, damping);
            } else {
                velocities[i] = zero;
            }
        }
    }
}

bool AQXPBDBody::anyNonFinite() const {
    for (std::size_t i = 0; i < positions.size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(positions[i][c][0]))  return true;
            if (!std::isfinite(velocities[i][c][0])) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Phase 7f — the host↔device readback sync point (the 6h idiom). Pulls the
// body's positions/velocities from the resident pool into the host mirror.
// No-op on the CPU path (never dirty) or when no backend is attached.
// ---------------------------------------------------------------------------

static void AQrefreshHostXPBDState(AQXPBDBody &body, AQComputeBackend *gpu) {
    if (!body.gpuDirty || gpu == nullptr) return;
    const std::size_t n = body.positions.size();
    if (n > 0) {
        OmegaCommon::Vector<float> px, py, pz, vx, vy, vz;
        if (!gpu->downloadXPBDParticles(body.id, px, py, pz, vx, vy, vz, n)) {
            std::cerr << "AQUA::AQSpace[XPBD]: GPU state readback failed for body "
                      << body.id << " — host mirror stays stale.\n";
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            body.positions[i]  = AQvec3(px[i], py[i], pz[i]);
            body.velocities[i] = AQvec3(vx[i], vy[i], vz[i]);
        }
    }
    body.gpuDirty = false;
}

// ---------------------------------------------------------------------------
// AQSpace::Impl — handle resolution (the particle-system idiom).
// ---------------------------------------------------------------------------

AQXPBDBody* AQSpace::Impl::xpbdBodyAt(std::uint64_t id) {
    if (id == 0) return nullptr;
    for (auto& body : xpbdBodies) {
        if (body && body->id == id) return body.get();
    }
    return nullptr;
}

const AQXPBDBody* AQSpace::Impl::xpbdBodyAt(std::uint64_t id) const {
    if (id == 0) return nullptr;
    for (const auto& body : xpbdBodies) {
        if (body && body->id == id) return body.get();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// AQSpace — public authoring/readback surface (§10).
// ---------------------------------------------------------------------------

AQXPBDBodyHandle AQSpace::createXPBDBody(const AQXPBDBodyDesc& desc) {
    AQXPBDBodyHandle handle;                       // id 0 = invalid
    if (desc.positions == nullptr || desc.count == 0) return handle;

    auto body = std::make_unique<AQXPBDBody>();
    body->id = impl->nextXPBDBodyId++;
    body->addParticles(desc.positions, desc.invMass, desc.count);

    handle.id = body->id;
    impl->xpbdBodies.push_back(std::move(body));
    return handle;
}

void AQSpace::destroyXPBDBody(AQXPBDBodyHandle body) {
    for (auto it = impl->xpbdBodies.begin(); it != impl->xpbdBodies.end(); ++it) {
        if (*it && (*it)->id == body.id) {
            if (impl->gpuBackend && (*it)->gpuResident) {
                impl->gpuBackend->xpbdReleasePool(body.id);
            }
            impl->xpbdBodies.erase(it);
            return;
        }
    }
}

AQConstraintHandle AQSpace::addDistanceConstraint(AQXPBDBodyHandle body,
                                                  std::uint32_t a, std::uint32_t b,
                                                  float restLength, float compliance) {
    AQConstraintHandle handle;                     // body 0 = invalid
    AQXPBDBody* bp = impl->xpbdBodyAt(body.id);
    if (!bp) return handle;
    const std::uint32_t nP = static_cast<std::uint32_t>(bp->positions.size());
    if (a >= nP || b >= nP || a == b) return handle;

    if (restLength < 0.f) {
        const FVec<3> d = bp->positions[a] - bp->positions[b];
        restLength = std::sqrt(OmegaGTE::dot(d, d));
    }
    if (compliance < 0.f) compliance = 0.f;

    handle.body = body.id;
    handle.type = AQConstraintDistance;
    handle.index = bp->addDistance(a, b, restLength, compliance);
    return handle;
}

std::uint32_t AQSpace::addRope(AQXPBDBodyHandle body,
                               const std::uint32_t* particles, std::uint32_t count,
                               float compliance) {
    AQXPBDBody* bp = impl->xpbdBodyAt(body.id);
    if (!bp || particles == nullptr || count < 2) return 0;
    const std::uint32_t nP = static_cast<std::uint32_t>(bp->positions.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        if (particles[i] >= nP) return 0;          // validate BEFORE authoring any
    }
    if (compliance < 0.f) compliance = 0.f;

    for (std::uint32_t i = 0; i + 1 < count; ++i) {
        const std::uint32_t a = particles[i], b = particles[i + 1];
        const FVec<3> d = bp->positions[a] - bp->positions[b];
        bp->addDistance(a, b, std::sqrt(OmegaGTE::dot(d, d)), compliance);
    }
    return count - 1;
}

void AQSpace::setXPBDParams(const AQXPBDParams& params) {
    AQXPBDParams p = params;
    if (p.substeps == 0u) p.substeps = 1u;
    if (p.iterations == 0u) p.iterations = 1u;
    if (p.velocityDamping < 0.f) p.velocityDamping = 0.f;
    if (p.velocityDamping >= 1.f) p.velocityDamping = 0.999f;
    if (!(p.explosionThreshold > 0.f)) p.explosionThreshold = 10.f;
    impl->xpbdParams = p;
}

AQXPBDParams AQSpace::xpbdParams() const { return impl->xpbdParams; }

std::uint32_t AQSpace::readXPBDState(AQXPBDBodyHandle body,
                                     OmegaGTE::FVec<3>* outPositions,
                                     OmegaGTE::FVec<3>* outVelocities,
                                     std::uint32_t maxCount) const {
    // unique_ptr does not propagate constness to the pointee — the lazy GPU
    // readback (a cache refresh, logically const) is reachable from here.
    AQXPBDBody* bp = impl->xpbdBodyAt(body.id);
    if (!bp) return 0;
    AQrefreshHostXPBDState(*bp, impl->gpuBackend);
    const std::uint32_t n =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(bp->positions.size()), maxCount);
    for (std::uint32_t i = 0; i < n; ++i) {
        // §13.7 7h #2 — positions are offsets from the body origin (origin 0 ⇒
        // world, the default); reconstruct the world position at the boundary.
        if (outPositions)  outPositions[i]  = bp->origin + bp->positions[i];
        if (outVelocities) outVelocities[i] = bp->velocities[i];
    }
    return n;
}

std::uint32_t AQSpace::readXPBDConstraints(AQXPBDBodyHandle body,
                                           AQDistanceConstraint* outConstraints,
                                           std::uint32_t maxConstraints,
                                           OmegaCommon::Vector<AQConstraintBatch>* outBatches) const {
    // unique_ptr does not propagate constness to the pointee, so the lazy
    // recolor (a cache refresh, logically const) is reachable from here.
    AQXPBDBody* bp = impl->xpbdBodyAt(body.id);
    if (!bp) return 0;
    if (bp->colorsDirty) bp->recolor();

    const std::uint32_t n = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(bp->distanceSorted.size()), maxConstraints);
    if (outConstraints) {
        for (std::uint32_t i = 0; i < n; ++i) outConstraints[i] = bp->distanceSorted[i];
    }
    if (outBatches) *outBatches = bp->batches;
    return n;
}

std::uint32_t AQSpace::xpbdGuardTrips(AQXPBDBodyHandle body) const {
    const AQXPBDBody* bp = impl->xpbdBodyAt(body.id);
    return bp ? bp->guardTrips : 0u;
}

void AQSpace::setXPBDCollisionEnabled(AQXPBDBodyHandle body, bool on,
                                      float particleRadius, float friction) {
    AQXPBDBody* bp = impl->xpbdBodyAt(body.id);
    if (!bp) return;
    bp->collisionEnabled = on;
    bp->particleRadius   = particleRadius > 0.f ? particleRadius : 0.f;
    bp->friction         = friction > 0.f ? friction : 0.f;
}

void AQSpace::setXPBDLongRangeAttachment(AQXPBDBodyHandle body, bool on) {
    AQXPBDBody* bp = impl->xpbdBodyAt(body.id);
    if (!bp) return;
    if (bp->longRangeAttach == on) return;
    bp->longRangeAttach = on;
    bp->colorsDirty = true;    // buildLongRange runs inside recolor()
}

// ---------------------------------------------------------------------------
// Step wiring + debug bus (driven by AQContext::advance).
// ---------------------------------------------------------------------------

void AQSpace::xpbdSubstep(float dt, bool gpuActive) {
    if (impl->xpbdBodies.empty()) return;

    // §13.6 7g — gather the rigid-collider snapshot once per sub-step if any XPBD
    // body couples to it. Uses the PUBLIC AQRigidBody accessors (the particle-path
    // convention), extended with the mass / world inverse inertia / body index the
    // TWO-WAY reaction needs. Read here (post-stepInternal) so poses are current.
    bool anyCollide = false;
    for (auto& body : impl->xpbdBodies)
        if (body && body->collisionEnabled) { anyCollide = true; break; }

    OmegaCommon::Vector<AQXPBDCollider> colliders;
    if (anyCollide) {
        for (std::uint32_t bi = 0; bi < impl->bodies.size(); ++bi) {
            const auto& rb = impl->bodies[bi];
            if (!rb) continue;
            const AQShape* sp = impl->shapeAt(rb->shape());
            if (!sp) continue;
            AQXPBDCollider c;
            c.shape          = *sp;
            c.xform.p        = rb->position();
            c.xform.q        = rb->orientation();
            c.vel            = rb->velocity();
            c.omega          = rb->angularVelocity();
            c.restitution    = rb->restitution();
            c.bodyIndex      = bi;
            const float m    = rb->mass();
            c.dynamic        = (rb->type() == AQBodyType::Dynamic) && (m > 0.f);
            c.invMassBody    = c.dynamic ? (1.f / m) : 0.f;
            c.worldInvInertia = rb->worldInverseInertia();
            // COM ≈ pose origin (exact when comOffset is 0, the default — used only
            // for the wb estimate; the reaction goes through applyImpulseAtPoint,
            // which applies the true COM offset).
            c.com            = rb->position();
            colliders.push_back(c);
        }
    }

    for (auto& body : impl->xpbdBodies) {
        if (!body) continue;
        // On the GPU path, only collision-enabled bodies solve on the CPU here
        // (their coupling needs CPU-resident rigid state); the rest are batched by
        // xpbdGpuFrame. On the CPU path every body runs here. (§13.6.)
        if (gpuActive && !body->collisionEnabled) continue;

        // Path-switch guard (GPU → CPU mid-run): never advance a stale host
        // mirror — re-base it on the device state first.
        AQrefreshHostXPBDState(*body, impl->gpuBackend);
        body->advance(dt, impl->xpbdParams, impl->gravity, colliders);

        // Drain the two-way reactions onto the rigid bodies. Velocity impulses go
        // through the SAME path the PGS sweep uses (COM-offset correct) and sum
        // naturally; the split-impulse position corrections are taken as a per-body
        // MAX (a bed of contacts all want the same lift — summing would launch it).
        // A non-negligible impulse wakes a sleeping dynamic body so cloth landing
        // on it is felt.
        std::unordered_map<std::uint32_t, FVec<3>> maxPosCorr;
        for (const AQXPBDReaction& r : body->reactionsThisStep) {
            if (r.bodyIndex >= impl->bodies.size()) continue;
            auto& rb = impl->bodies[r.bodyIndex];
            if (!rb) continue;
            rb->applyImpulseAtPoint(r.impulse, r.point);
            if (rb->type() == AQBodyType::Dynamic &&
                rb->activation() == AQActivationState::Sleeping &&
                OmegaGTE::dot(r.impulse, r.impulse) > 1e-12f) {
                rb->wakeUp();
            }
            auto it = maxPosCorr.find(r.bodyIndex);
            if (it == maxPosCorr.end()) {
                maxPosCorr.emplace(r.bodyIndex, r.posCorrection);
            } else if (OmegaGTE::dot(r.posCorrection, r.posCorrection) >
                       OmegaGTE::dot(it->second, it->second)) {
                it->second = r.posCorrection;
            }
        }
        for (const auto& kv : maxPosCorr) {
            auto& rb = impl->bodies[kv.first];
            if (rb && rb->type() == AQBodyType::Dynamic)
                rb->setPosition(rb->position() + kv.second);
        }
        body->reactionsThisStep.clear();
    }
}

// Phase 7f — the live GPU frame: encode every body's whole advance-frame
// (nSub engine sub-steps × params.substeps slices) in one command buffer per
// body, uploading state/topology only when authoring changed it. Failures are
// loud and skip the body (§9 guard doctrine — frozen is observable).
void AQSpace::xpbdGpuFrame(AQComputeBackend* backend, float dt, int substeps) {
    impl->gpuBackend = backend;
    if (!backend || substeps <= 0) return;

    for (auto& bodyH : impl->xpbdBodies) {
        if (!bodyH) continue;
        // §13.6 7g — collision-enabled bodies solve on the CPU (xpbdSubstep) so
        // their two-way coupling can read rigid poses and write rigid impulses;
        // skip them here so the device batch handles only the non-coupled bodies.
        if (bodyH->collisionEnabled) continue;
        AQXPBDBody& body = *bodyH;
        const std::size_t n = body.positions.size();
        if (n == 0) continue;

        const bool needUpload = !body.gpuResident || body.colorsDirty || body.gpuUploadNeeded;
        if (needUpload) {
            // A topology change re-seeds the device from the host: pull the
            // live device state down first so authoring mid-simulation never
            // rewinds particles to stale host positions.
            AQrefreshHostXPBDState(body, backend);
            if (body.colorsDirty) body.recolor();

            OmegaCommon::Vector<float> px(n), py(n), pz(n), im(n), vx(n), vy(n), vz(n);
            for (std::size_t i = 0; i < n; ++i) {
                px[i] = body.positions[i][0][0];
                py[i] = body.positions[i][1][0];
                pz[i] = body.positions[i][2][0];
                im[i] = body.invMass[i];
                vx[i] = body.velocities[i][0][0];
                vy[i] = body.velocities[i][1][0];
                vz[i] = body.velocities[i][2][0];
            }
            OmegaCommon::Vector<AQComputeBackend::XPBDConstraintIn> sorted;
            for (const AQDistanceConstraint& c : body.distanceSorted) {
                AQComputeBackend::XPBDConstraintIn in;
                in.a = c.a;
                in.b = c.b;
                in.restLength = c.restLength;
                in.compliance = c.compliance;
                in.color = c.color;
                sorted.push_back(in);
            }
            OmegaCommon::Vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
            for (const AQConstraintBatch& batch : body.batches) {
                ranges.push_back({batch.firstConstraint, batch.constraintCount});
            }
            if (!backend->uploadXPBDParticles(body.id, px, py, pz, im, vx, vy, vz) ||
                !backend->uploadXPBDConstraints(body.id, sorted, ranges)) {
                std::cerr << "AQUA::AQSpace[XPBD]: GPU upload failed for body "
                          << body.id << " — frame skipped.\n";
                continue;
            }
            body.gpuResident = true;
            body.gpuUploadNeeded = false;
            // Fresh upload zeroes the device trip counters — resync the delta
            // baseline so xpbdFrameEnd doesn't misread the reset as trips.
            body.gpuTripsPrev.assign(body.distanceSorted.size(), 0u);
        }

        const float g[3] = {impl->gravity[0][0], impl->gravity[1][0], impl->gravity[2][0]};
        if (!backend->encodeXPBDAdvance(body.id, dt, g,
                                        impl->xpbdParams.substeps,
                                        impl->xpbdParams.iterations,
                                        impl->xpbdParams.velocityDamping,
                                        impl->xpbdParams.explosionThreshold,
                                        n, static_cast<std::uint32_t>(substeps))) {
            std::cerr << "AQUA::AQSpace[XPBD]: GPU frame encode failed for body "
                      << body.id << " — device state did not advance.\n";
            continue;
        }
        body.gpuDirty = true;
    }
}

void AQSpace::xpbdFrameEnd() {
    const bool drawStrain = (impl->debugFlags & AQDebugConstraint) != 0u;
    const bool drawColors = (impl->debugFlags & AQDebugConstraintColor) != 0u;

    // 8-entry palette for AQDebugConstraintColor — distinct hues so adjacent
    // color batches are visually separable (they cycle past 8).
    static const float kPalette[8][3] = {
        {1.f, 0.2f, 0.2f}, {0.2f, 1.f, 0.2f}, {0.2f, 0.4f, 1.f}, {1.f, 1.f, 0.2f},
        {1.f, 0.2f, 1.f}, {0.2f, 1.f, 1.f}, {1.f, 0.6f, 0.2f}, {0.7f, 0.7f, 0.7f},
    };

    for (auto& body : impl->xpbdBodies) {
        if (!body) continue;

        // 7f live path — fold the device's CUMULATIVE guard-trip counters
        // into the host bookkeeping as this-frame deltas: guardTrips advances
        // exactly as the CPU path's would, the tripped sorted-slots feed the
        // flat-red debug grading below, and the loud once-per-body report
        // fires from here (the projection runs on the device, so the CPU
        // path's in-loop stderr never sees these trips).
        if (body->gpuResident && impl->gpuBackend) {
            OmegaCommon::Vector<std::uint32_t> trips;
            if (impl->gpuBackend->downloadXPBDTrips(body->id, trips)) {
                if (body->gpuTripsPrev.size() != trips.size()) {
                    body->gpuTripsPrev.assign(trips.size(), 0u);
                }
                for (std::uint32_t k = 0; k < trips.size(); ++k) {
                    const std::uint32_t delta = trips[k] - body->gpuTripsPrev[k];
                    if (delta > 0) {
                        body->guardTrips += delta;
                        body->trippedThisFrame.push_back(k);
                    }
                }
                body->gpuTripsPrev = trips;
                if (body->guardTrips > 0 && !body->guardWarned) {
                    std::cerr << "AQUA::AQSpace[XPBD]: explosion guard tripped on body "
                              << body->id << " (GPU path) — per-slice corrections clamped; "
                              << "the solve is diverging (over-stiff rig, extreme mass "
                              << "ratio, or too-coarse dt); further warnings for this "
                              << "body suppressed.\n";
                    body->guardWarned = true;
                }
            }
        }

        if (drawStrain || drawColors) {
            AQrefreshHostXPBDState(*body, impl->gpuBackend);
            // Dedup this frame's guard trips for O(log n) membership below.
            auto& tripped = body->trippedThisFrame;
            std::sort(tripped.begin(), tripped.end());
            tripped.erase(std::unique(tripped.begin(), tripped.end()), tripped.end());

            for (std::uint32_t k = 0; k < body->distanceSorted.size(); ++k) {
                const AQDistanceConstraint& c = body->distanceSorted[k];
                AQDebugLine line;
                line.a = body->positions[c.a];
                line.b = body->positions[c.b];

                if (drawColors) {
                    const float* rgb = kPalette[c.color % 8u];
                    line.rgba[0] = rgb[0]; line.rgba[1] = rgb[1];
                    line.rgba[2] = rgb[2]; line.rgba[3] = 1.f;
                    impl->debugLines.push_back(line);
                }
                if (drawStrain) {
                    const bool wasTripped = std::binary_search(
                        tripped.begin(), tripped.end(), k);
                    const FVec<3> d = line.a - line.b;
                    const float len = std::sqrt(OmegaGTE::dot(d, d));
                    const float rest = (c.restLength > 1e-12f) ? c.restLength : 1e-12f;
                    // Tension grades green → red, compression green → blue,
                    // full grade at |strain| = 10%; a guard-tripped constraint
                    // is flat red regardless (it IS the diagnostic).
                    const float strain = (len - rest) / rest;
                    float t = strain / 0.1f;
                    if (t > 1.f) t = 1.f;
                    if (t < -1.f) t = -1.f;
                    if (wasTripped) {
                        line.rgba[0] = 1.f; line.rgba[1] = 0.f; line.rgba[2] = 0.f;
                    } else if (t >= 0.f) {
                        line.rgba[0] = t; line.rgba[1] = 1.f - t; line.rgba[2] = 0.f;
                    } else {
                        line.rgba[0] = 0.f; line.rgba[1] = 1.f + t; line.rgba[2] = -t;
                    }
                    line.rgba[3] = 1.f;
                    impl->debugLines.push_back(line);
                }
            }
        }

        body->trippedThisFrame.clear();            // frame boundary — always reset
    }
}
