// AQUA Phase 6 — particle-system implementation (§13.3). Split out of AQSpace.cpp
// so the particle pillar lives in its own translation unit while sharing the one
// AQSpace::Impl definition (AQSpaceImpl.h). This file (6b) lands the pool
// backing store, the deterministic free-list allocator, and the stable stream
// compaction that recycles dead slots. Emission (6c), collision + step wiring +
// the public API (6c/6d), and debug draw (6e) layer on top in later increments.
//
// The whole point of 6b is bookkeeping correctness, not dynamics (§2): a leak, a
// double-free, or a non-deterministic recycle has no visible dynamics tell, so
// the discipline here — DESCENDING free-list (smallest slot reused first) +
// STABLE in-place compaction — is what makes the CPU path match a double oracle
// and a future GPU path bit-for-bit on the census.

#include "AQSpaceImpl.h"
#include "AQParticleMath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// ---------------------------------------------------------------------------
// AQParticleSystem — pool lifecycle, allocation, compaction, guards.
// ---------------------------------------------------------------------------

void AQParticleSystem::reset(std::uint32_t cap) {
    capacity = cap;

    // resize(n, value): the two-arg form copy-fills, so it never needs FVec's
    // (private) default ctor — the same reason the rest of AQUA seeds FVec
    // vectors from Create().
    const FVec<3> zero = FVec<3>::Create();
    positions.assign(cap, zero);
    velocities.assign(cap, zero);
    accels.assign(cap, zero);
    invMass.assign(cap, 0.f);
    lifetime.assign(cap, 0.0);
    radius.assign(cap, 0.f);
    flags.assign(cap, AQParticleDead);

    // Free-list holds every slot, DESCENDING (front = largest, back = smallest),
    // so pop_back() during allocate() hands out slot 0 first, then 1, 2, ...
    freeList.resize(cap);
    for (std::uint32_t i = 0; i < cap; ++i) freeList[i] = cap - 1u - i;

    liveCount = 0;
}

std::uint32_t AQParticleSystem::allocate(std::uint32_t k,
                                         OmegaCommon::Vector<std::uint32_t>& outSlots) {
    const std::uint32_t avail = freeCount();
    const std::uint32_t n = (k < avail) ? k : avail;   // near-exhaustion caps here (§9)
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t slot = freeList.back();     // smallest free index
        freeList.pop_back();
        flags[slot] = AQParticleAlive;
        outSlots.push_back(slot);
        ++liveCount;
    }
    return n;
}

void AQParticleSystem::kill(std::uint32_t slot) {
    if (slot >= capacity) return;
    if ((flags[slot] & AQParticleAlive) == 0u) return;  // already dead — no double count
    flags[slot] = AQParticleDead;
    --liveCount;
    // Slot is NOT returned to the free-list here: compaction reclaims it, which
    // keeps this frame's free-list (already handed to emission) stable.
}

void AQParticleSystem::compact() {
    // Stable in-place stream compaction: survivors slide down into the prefix in
    // their existing order; the vacated tail becomes the new free-list.
    std::uint32_t writeIdx = 0;
    for (std::uint32_t readIdx = 0; readIdx < capacity; ++readIdx) {
        if ((flags[readIdx] & AQParticleAlive) == 0u) continue;
        if (writeIdx != readIdx) {
            positions[writeIdx]  = positions[readIdx];
            velocities[writeIdx] = velocities[readIdx];
            accels[writeIdx]     = accels[readIdx];
            invMass[writeIdx]    = invMass[readIdx];
            lifetime[writeIdx]   = lifetime[readIdx];
            radius[writeIdx]     = radius[readIdx];
            flags[writeIdx]      = AQParticleAlive;
        }
        ++writeIdx;
    }

    // Mark the vacated tail dead and rebuild the free-list over it (descending,
    // so the smallest freed slot is again the back / first to be reused).
    for (std::uint32_t i = writeIdx; i < capacity; ++i) flags[i] = AQParticleDead;
    freeList.clear();
    for (std::uint32_t i = capacity; i-- > writeIdx; ) freeList.push_back(i);

    liveCount = writeIdx;   // packed prefix length == census (§7)
}

// ---------------------------------------------------------------------------
// AQParticleSystem — per-sub-step passes (§13.3 6c).
// ---------------------------------------------------------------------------

void AQParticleSystem::emit(float dt) {
    // Keep the per-emitter bookkeeping parallel to `emitters` (emitters may have
    // been added since the last frame). Resize preserves existing carry/ordinal.
    if (emitterCarry.size()   != emitters.size()) emitterCarry.resize(emitters.size(), 0.0);
    if (emitterOrdinal.size() != emitters.size()) emitterOrdinal.resize(emitters.size(), 0u);

    OmegaCommon::Vector<std::uint32_t> slots;
    for (std::size_t e = 0; e < emitters.size(); ++e) {
        const AQEmitter &em = emitters[e];
        if (!em.enabled) continue;

        const std::uint32_t count = AQemitCount(em.rate, dt, emitterCarry[e]);
        if (count == 0) continue;

        slots.clear();
        const std::uint32_t got = allocate(count, slots);   // near-exhaustion caps here (§9)
        for (std::uint32_t i = 0; i < got; ++i) {
            // Seed from the emitter seed + a path-independent ordinal so the same
            // logical particle always draws the same attribute stream. Only
            // emitted particles consume an ordinal (saturation-dropped ones do
            // not), which keeps the sequence stable across frames.
            const std::uint64_t ord = emitterOrdinal[e]++;
            AQParticleRng rng(AQmixSeed(em.seed, ord));

            const std::uint32_t s = slots[i];
            positions[s]  = AQsampleEmitPosition<float>(em, rng);
            velocities[s] = AQsampleEmitVelocity<float>(em, rng);
            accels[s]     = AQvec3(0.f, 0.f, 0.f);
            // lifetime in double (same draw as the oracle) — see AQSpaceImpl.h.
            lifetime[s]   = AQsampleLifetime<double>(em, rng);
            invMass[s]    = (em.mass > 0.f) ? (1.f / em.mass) : 0.f;
            radius[s]     = em.radius;
            // flags[s] is already AQParticleAlive (set by allocate()).
        }
    }
}

void AQParticleSystem::accumulateAndIntegrate(float dt) {
    for (std::uint32_t s = 0; s < capacity; ++s) {
        if ((flags[s] & AQParticleAlive) == 0u) continue;
        AQVec3<float> a = AQvec3(0.f, 0.f, 0.f);
        for (const AQForceField &f : fields) a = a + AQevalField<float>(f, positions[s], velocities[s]);
        accels[s] = a;
        AQintegrateSemiImplicit<float>(positions[s], velocities[s], a, dt);
    }
}

void AQParticleSystem::age(float dt) {
    for (std::uint32_t s = 0; s < capacity; ++s) {
        if ((flags[s] & AQParticleAlive) == 0u) continue;
        lifetime[s] -= static_cast<double>(dt);
        if (lifetime[s] <= 0.0) kill(s);
    }
}

void AQParticleSystem::collide(const OmegaCommon::Vector<AQParticleCollider>& colliders,
                               const OmegaGTE::FVec<3>* hullVerts, std::size_t hullVertCount) {
    for (std::uint32_t s = 0; s < capacity; ++s) {
        if ((flags[s] & AQParticleAlive) == 0u) continue;
        const float r = radius[s];
        for (const AQParticleCollider& c : colliders) {
            const AQShapeSample sd =
                AQshapeSignedDistance(c.shape, positions[s], c.xform, hullVerts, hullVertCount);
            if (sd.distance >= r) continue;                     // no contact
            // Exact analytic push-out along the surface normal (§5) — a plane's
            // closed-form distance leaves no tunnel.
            positions[s] = positions[s] + sd.normal * (r - sd.distance);
            // Reflect/damp only the INTO-surface velocity component.
            const float vn = OmegaGTE::dot(velocities[s], sd.normal);
            if (vn < 0.f) velocities[s] = velocities[s] - sd.normal * ((1.f + c.restitution) * vn);
        }
    }
}

bool AQParticleSystem::anyNonFinite() const {
    for (std::uint32_t s = 0; s < capacity; ++s) {
        if ((flags[s] & AQParticleAlive) == 0u) continue;
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(positions[s][c][0]))  return true;
            if (!std::isfinite(velocities[s][c][0])) return true;
        }
    }
    return false;
}

bool AQParticleSystem::partitionOK() const {
    // Every slot must be ALIVE, on the free-list, or dead-pending — never two of
    // those. A free slot marked ALIVE, or a slot appearing twice on the
    // free-list, is a double-free; an ALIVE count that disagrees with liveCount
    // is a miscount/leak. This is the census net §9 relies on.
    OmegaCommon::Vector<std::uint8_t> onFree(capacity, 0u);
    for (std::uint32_t f : freeList) {
        if (f >= capacity)               return false;   // out of range
        if (flags[f] & AQParticleAlive)  return false;   // free slot marked alive
        if (onFree[f])                   return false;   // duplicate => double free
        onFree[f] = 1u;
    }
    std::uint32_t aliveCount = 0;
    for (std::uint32_t s = 0; s < capacity; ++s) {
        if ((flags[s] & AQParticleAlive) == 0u) continue;
        if (onFree[s]) return false;                     // alive AND on free-list
        ++aliveCount;
    }
    return aliveCount == liveCount;
}

// ---------------------------------------------------------------------------
// AQSpace::Impl — particle-system table resolution.
// ---------------------------------------------------------------------------

AQParticleSystem *AQSpace::Impl::particleSystemAt(std::uint64_t id) {
    if (id == 0) return nullptr;
    for (auto &sys : particleSystems) {
        if (sys && sys->id == id) return sys.get();
    }
    return nullptr;
}

const AQParticleSystem *AQSpace::Impl::particleSystemAt(std::uint64_t id) const {
    if (id == 0) return nullptr;
    for (const auto &sys : particleSystems) {
        if (sys && sys->id == id) return sys.get();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// AQSpace — public particle API (§10). Handles are opaque ids resolved through
// the pimpl each call; an unknown/stale handle is a safe no-op / zero.
// ---------------------------------------------------------------------------

AQParticleSystemHandle AQSpace::createParticleSystem(std::uint32_t capacity) {
    auto sys = std::make_unique<AQParticleSystem>();
    sys->id = impl->nextParticleSystemId++;
    sys->reset(capacity);
    AQParticleSystemHandle h;
    h.id = sys->id;
    impl->particleSystems.push_back(std::move(sys));
    return h;
}

void AQSpace::destroyParticleSystem(AQParticleSystemHandle system) {
    auto &vec = impl->particleSystems;
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (vec[i] && vec[i]->id == system.id) { vec.erase(vec.begin() + i); return; }
    }
}

void AQSpace::addEmitter(AQParticleSystemHandle system, const AQEmitter &emitter) {
    if (auto *s = impl->particleSystemAt(system.id)) s->emitters.push_back(emitter);
}

void AQSpace::setEmitterEnabled(AQParticleSystemHandle system, std::uint32_t idx, bool on) {
    if (auto *s = impl->particleSystemAt(system.id))
        if (idx < s->emitters.size()) s->emitters[idx].enabled = on ? 1u : 0u;
}

void AQSpace::addForceField(AQParticleSystemHandle system, const AQForceField &field) {
    if (auto *s = impl->particleSystemAt(system.id)) s->fields.push_back(field);
}

void AQSpace::setForceFieldEnabled(AQParticleSystemHandle system, std::uint32_t idx, bool on) {
    if (auto *s = impl->particleSystemAt(system.id))
        if (idx < s->fields.size()) s->fields[idx].enabled = on ? 1u : 0u;
}

void AQSpace::setParticleCollisionEnabled(AQParticleSystemHandle system, bool on) {
    if (auto *s = impl->particleSystemAt(system.id)) s->collisionEnabled = on ? 1u : 0u;
}

std::uint32_t AQSpace::readParticleState(AQParticleSystemHandle system,
                                         OmegaGTE::FVec<3> *outPositions,
                                         OmegaGTE::FVec<3> *outVelocities,
                                         float *outLifetimes,
                                         std::uint32_t *outFlags,
                                         std::uint32_t maxCount) const {
    const AQParticleSystem *s = impl->particleSystemAt(system.id);
    if (!s) return 0;
    // After the end-of-frame compaction the live set is packed into [0, liveCount).
    const std::uint32_t n = (s->liveCount < maxCount) ? s->liveCount : maxCount;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (outPositions)  outPositions[i]  = s->positions[i];
        if (outVelocities) outVelocities[i] = s->velocities[i];
        if (outLifetimes)  outLifetimes[i]  = static_cast<float>(s->lifetime[i]);
        if (outFlags)      outFlags[i]      = s->flags[i];
    }
    return n;
}

std::uint32_t AQSpace::liveParticleCount(AQParticleSystemHandle system) const {
    const AQParticleSystem *s = impl->particleSystemAt(system.id);
    return s ? s->liveCount : 0u;
}

// ---------------------------------------------------------------------------
// AQSpace — particle stepping (private; driven by AQContext::advance).
// ---------------------------------------------------------------------------

void AQSpace::particlesEmit(float simDt) {
    if (impl->particleSystems.empty()) return;

    // Rebuild the static-collider snapshot once per advance. Uses the PUBLIC body
    // accessors (AQRigidBody::Impl is not in this TU) and the same pose-origin +
    // orientation transform convention AQshapeAABB is called with elsewhere.
    impl->particleColliders.clear();
    for (const auto &body : impl->bodies) {
        if (!body) continue;
        const AQShape *sp = impl->shapeAt(body->shape());
        if (!sp) continue;
        AQParticleCollider c;
        c.shape       = *sp;
        c.xform.p     = body->position();
        c.xform.q     = body->orientation();
        c.restitution = body->restitution();
        impl->particleColliders.push_back(c);
    }

    for (auto &sys : impl->particleSystems) if (sys) sys->emit(simDt);
}

void AQSpace::particlesSubstep(float dt) {
    for (auto &sys : impl->particleSystems) {
        if (!sys) continue;
        sys->accumulateAndIntegrate(dt);
        if (sys->collisionEnabled)
            sys->collide(impl->particleColliders, impl->hullVerts.data(), impl->hullVerts.size());
        sys->age(dt);
    }
}

void AQSpace::particlesCompact() {
    for (auto &sys : impl->particleSystems) if (sys) sys->compact();

    // Debug emission (§9) onto the existing drainable bus — only when a particle
    // flag is set (zero-cost otherwise). Runs after compaction, so it draws the
    // packed live set.
    const std::uint32_t dflags = impl->debugFlags;
    if ((dflags & (AQDebugParticle | AQDebugForceField)) == 0u) return;

    for (auto &sys : impl->particleSystems) {
        if (!sys) continue;

        if (dflags & AQDebugParticle) {
            // One short velocity vector per live particle (0.05 s of motion).
            for (std::uint32_t p = 0; p < sys->liveCount; ++p) {
                AQDebugLine ln;
                ln.a = sys->positions[p];
                ln.b = sys->positions[p] + sys->velocities[p] * 0.05f;
                ln.rgba[0] = 0.3f; ln.rgba[1] = 0.8f; ln.rgba[2] = 1.0f; ln.rgba[3] = 1.0f;
                impl->debugLines.push_back(ln);
            }
        }

        if (dflags & AQDebugForceField) {
            for (const AQForceField &f : sys->fields) {
                if (!f.enabled) continue;
                // Direction/axis indicator anchored at the field position.
                AQDebugLine dir;
                dir.a = f.position;
                dir.b = f.position + f.axis;
                dir.rgba[0] = 1.0f; dir.rgba[1] = 0.6f; dir.rgba[2] = 0.2f; dir.rgba[3] = 1.0f;
                impl->debugLines.push_back(dir);

                // Localized fields (point/vortex) also draw an influence-radius
                // cross so the on-call engineer sees the region (§9).
                if ((f.kind == AQFieldPoint || f.kind == AQFieldVortex) && f.radiusOfInfluence > 0.f) {
                    const float r = f.radiusOfInfluence;
                    const OmegaGTE::FVec<3> ax[3] = { AQvec3(r, 0.f, 0.f), AQvec3(0.f, r, 0.f), AQvec3(0.f, 0.f, r) };
                    for (int k = 0; k < 3; ++k) {
                        AQDebugLine cross;
                        cross.a = f.position - ax[k];
                        cross.b = f.position + ax[k];
                        cross.rgba[0] = 1.0f; cross.rgba[1] = 0.4f; cross.rgba[2] = 0.1f; cross.rgba[3] = 1.0f;
                        impl->debugLines.push_back(cross);
                    }
                }
            }
        }
    }
}
