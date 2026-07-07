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

    colorsDirty = false;
}

void AQXPBDBody::advance(float dt, const AQXPBDParams& params, const FVec<3>& gravity) {
    if (positions.empty()) return;
    if (colorsDirty) recolor();

    const std::uint32_t nSlices = params.substeps > 0u ? params.substeps : 1u;
    const std::uint32_t nIters  = params.iterations > 0u ? params.iterations : 1u;
    const float h = dt / static_cast<float>(nSlices);
    const float damping = params.velocityDamping;
    const float maxMove = params.explosionThreshold;
    const std::uint32_t nP = static_cast<std::uint32_t>(positions.size());
    const FVec<3> zero = FVec<3>::Create();

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
    const AQXPBDBody* bp = impl->xpbdBodyAt(body.id);
    if (!bp) return 0;
    const std::uint32_t n =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(bp->positions.size()), maxCount);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (outPositions)  outPositions[i]  = bp->positions[i];
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

// ---------------------------------------------------------------------------
// Step wiring + debug bus (driven by AQContext::advance).
// ---------------------------------------------------------------------------

void AQSpace::xpbdSubstep(float dt) {
    if (impl->xpbdBodies.empty()) return;
    for (auto& body : impl->xpbdBodies) {
        if (body) body->advance(dt, impl->xpbdParams, impl->gravity);
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

        if (drawStrain || drawColors) {
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
