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
#include "AQComputeBackend.h"   // Phase 6h — the live GPU particle path

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>

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
    deathCountdown.assign(cap, 0u);
    lifetime.assign(cap, 0.f);
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
            positions[writeIdx]      = positions[readIdx];
            velocities[writeIdx]     = velocities[readIdx];
            accels[writeIdx]         = accels[readIdx];
            invMass[writeIdx]        = invMass[readIdx];
            deathCountdown[writeIdx] = deathCountdown[readIdx];
            lifetime[writeIdx]       = lifetime[readIdx];
            radius[writeIdx]         = radius[readIdx];
            flags[writeIdx]          = AQParticleAlive;
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

void AQParticleSystem::emit(float dt, float substepDt) {
    // Keep the per-emitter bookkeeping parallel to `emitters` (emitters may have
    // been added since the last frame). Resize preserves existing carry/ordinal.
    if (emitterCarry.size()   != emitters.size()) emitterCarry.resize(emitters.size(), 0.0);
    if (emitterOrdinal.size() != emitters.size()) emitterOrdinal.resize(emitters.size(), 0u);

    // 6h: the pool was compacted at the last frame end, so this frame's
    // allocations are exactly the tail [liveCount, liveCount + emitted) — the
    // window the GPU staging upload reads back out of the host arrays.
    emitBase = liveCount;
    emitDeathStage.clear();

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
            // Lifetime sampled in double (same draw on every path), then
            // frozen into the integer death schedule — see AQSpaceImpl.h.
            const double lifeD = AQsampleLifetime<double>(em, rng);
            deathCountdown[s] = AQdeathCountdown(lifeD, substepDt);
            emitDeathStage.push_back(deathCountdown[s]);  // 6h: pre-age copy
            lifetime[s]   = static_cast<float>(lifeD);   // display only
            invMass[s]    = (em.mass > 0.f) ? (1.f / em.mass) : 0.f;
            radius[s]     = em.radius;
            // flags[s] is already AQParticleAlive (set by allocate()).
        }
    }
    emittedThisFrame = liveCount - emitBase;
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
        lifetime[s] -= dt;                       // display only, never a death test
        if (deathCountdown[s] <= 1u) {
            deathCountdown[s] = 0u;
            kill(s);
        } else {
            --deathCountdown[s];
        }
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
// Phase 6h — the host↔device readback sync point (§14.1). Pulls the packed
// live prefix's positions/velocities from the resident pool into the host
// mirror. No-op on the CPU path (never dirty) or when no backend is attached.
// ---------------------------------------------------------------------------

static void AQrefreshHostParticleState(AQParticleSystem &sys, AQComputeBackend *gpu) {
    if (!sys.gpuDirty || gpu == nullptr) return;
    const std::uint32_t n = sys.liveCount;
    if (n > 0) {
        OmegaCommon::Vector<float> px(n), py(n), pz(n), vx(n), vy(n), vz(n);
        if (!gpu->particlesDownloadState(sys.id, px.data(), py.data(), pz.data(),
                                         vx.data(), vy.data(), vz.data(), n)) {
            std::cerr << "AQUA::AQSpace[particles]: GPU state readback failed for "
                         "system " << sys.id << " — host mirror stays stale.\n";
            return;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            sys.positions[i]  = AQvec3(px[i], py[i], pz[i]);
            sys.velocities[i] = AQvec3(vx[i], vy[i], vz[i]);
        }
    }
    sys.gpuDirty = false;
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
        if (vec[i] && vec[i]->id == system.id) {
            if (impl->gpuBackend && vec[i]->gpuResident) {
                impl->gpuBackend->particlesReleasePool(system.id);
            }
            vec.erase(vec.begin() + i);
            return;
        }
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
    // unique_ptr does not propagate constness to the pointee: the lazy GPU
    // readback (a cache refresh, logically const) is reachable from here —
    // the same idiom readXPBDConstraints uses for its lazy recolor.
    AQParticleSystem *s = impl->particleSystemAt(system.id);
    if (!s) return 0;
    AQrefreshHostParticleState(*s, impl->gpuBackend);
    // After the end-of-frame compaction the live set is packed into [0, liveCount).
    const std::uint32_t n = (s->liveCount < maxCount) ? s->liveCount : maxCount;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (outPositions)  outPositions[i]  = s->positions[i];
        if (outVelocities) outVelocities[i] = s->velocities[i];
        if (outLifetimes)  outLifetimes[i]  = s->lifetime[i];
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

void AQSpace::particlesEmit(float simDt, float substepDt) {
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

    for (auto &sys : impl->particleSystems) if (sys) sys->emit(simDt, substepDt);
}

void AQSpace::particlesSubstep(float dt) {
    for (auto &sys : impl->particleSystems) {
        if (!sys) continue;
        // Path-switch guard (GPU → CPU mid-run): the CPU float step must not
        // integrate a stale host mirror — re-base it on the device state.
        AQrefreshHostParticleState(*sys, impl->gpuBackend);
        sys->accumulateAndIntegrate(dt);
        if (sys->collisionEnabled)
            sys->collide(impl->particleColliders, impl->hullVerts.data(), impl->hullVerts.size());
        sys->age(dt);
    }
}

// Phase 6h — the GPU path's per-sub-step HOST work: only the deterministic
// integer bookkeeping (death countdown + display lifetime). The float physics
// (integrate/collide) runs on the device in particlesGpuFrame's encode.
void AQSpace::particlesAgeSubstep(float dt) {
    for (auto &sys : impl->particleSystems) {
        if (sys) sys->age(dt);
    }
}

// Phase 6h — encode one advance-frame of device work per system: staged-
// emission inject → substeps × (integrate [+ collide] + age) → scan/scatter
// compaction, one command buffer, one sync (§14.3). Runs AFTER the sub-step
// loop (the host bookkeeping is then final, so newLiveCount is exact) and
// BEFORE the host-side compaction mirrors the same permutation. Failures are
// loud and skip the system — a frozen system is observable; a silent one is
// not (§9 guard doctrine).
void AQSpace::particlesGpuFrame(AQComputeBackend *backend, float dt, int substeps) {
    impl->gpuBackend = backend;
    if (!backend || substeps <= 0) return;

    for (auto &sysH : impl->particleSystems) {
        if (!sysH) continue;
        AQParticleSystem &sys = *sysH;
        const std::uint32_t span = sys.emitBase + sys.emittedThisFrame;
        if (span == 0 && !sys.gpuResident) continue;   // nothing has ever lived

        if (!sys.gpuResident) {
            if (!backend->particlesEnsurePool(sys.id, sys.capacity)) {
                std::cerr << "AQUA::AQSpace[particles]: GPU pool creation failed for "
                             "system " << sys.id << " — frame skipped.\n";
                continue;
            }
            sys.gpuResident = true;
        }

        // Stage this frame's emission slice out of the host arrays (positions
        // and velocities are still the at-emission values — the host never
        // integrates on this path; countdowns come from the pre-age copy).
        const std::uint32_t k = sys.emittedThisFrame;
        bool ok = true;
        if (k > 0) {
            OmegaCommon::Vector<float> px(k), py(k), pz(k), rad(k);
            OmegaCommon::Vector<float> vx(k), vy(k), vz(k), im(k);
            for (std::uint32_t i = 0; i < k; ++i) {
                const std::uint32_t s = sys.emitBase + i;
                px[i] = sys.positions[s][0][0];
                py[i] = sys.positions[s][1][0];
                pz[i] = sys.positions[s][2][0];
                rad[i] = sys.radius[s];
                vx[i] = sys.velocities[s][0][0];
                vy[i] = sys.velocities[s][1][0];
                vz[i] = sys.velocities[s][2][0];
                im[i] = sys.invMass[s];
            }
            ok = backend->particlesUploadStaging(sys.id, px.data(), py.data(), pz.data(),
                                                 rad.data(), vx.data(), vy.data(), vz.data(),
                                                 im.data(), sys.emitDeathStage.data(), k);
        }
        ok = ok && backend->particlesUploadFields(sys.id, sys.fields);

        // Static-collider snapshot → the flattened GPU records (6i). Combined
        // world transform (body ∘ shape-local) with the CPU's float math;
        // hulls are filtered (their SDF is +inf on the CPU path too); plane
        // normals pre-normalized exactly as AQshapeSignedDistanceGeneric does.
        std::uint32_t colliderCount = 0;
        if (ok && sys.collisionEnabled && !impl->particleColliders.empty()) {
            OmegaCommon::Vector<AQComputeBackend::ParticleColliderIn> cols;
            for (const AQParticleCollider &c : impl->particleColliders) {
                if (c.shape.type == AQShapeType::ConvexHull) continue;
                AQTransform<float> local;
                local.p = AQvec3(c.shape.lpx, c.shape.lpy, c.shape.lpz);
                local.q = OmegaGTE::Quaternion<float>{c.shape.lqx, c.shape.lqy,
                                                      c.shape.lqz, c.shape.lqw};
                const AQTransform<float> xf = c.xform * local;
                AQComputeBackend::ParticleColliderIn in;
                in.shapeType = static_cast<std::uint32_t>(c.shape.type);
                in.px = xf.p[0][0]; in.py = xf.p[1][0]; in.pz = xf.p[2][0];
                in.qx = xf.q.x; in.qy = xf.q.y; in.qz = xf.q.z; in.qw = xf.q.w;
                in.restitution = c.restitution;
                switch (c.shape.type) {
                case AQShapeType::Sphere:
                    in.params[0] = c.shape.sphere.radius;
                    break;
                case AQShapeType::Box:
                    in.params[0] = c.shape.box.hx;
                    in.params[1] = c.shape.box.hy;
                    in.params[2] = c.shape.box.hz;
                    break;
                case AQShapeType::Capsule:
                    in.params[0] = c.shape.capsule.radius;
                    in.params[1] = c.shape.capsule.halfHeight;
                    break;
                case AQShapeType::Plane: {
                    float nx = c.shape.plane.nx, ny = c.shape.plane.ny, nz = c.shape.plane.nz;
                    float offset = c.shape.plane.offset;
                    const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (nlen > 1e-12f) { nx /= nlen; ny /= nlen; nz /= nlen; offset /= nlen; }
                    in.params[0] = nx; in.params[1] = ny; in.params[2] = nz;
                    in.params[3] = offset;
                    break;
                }
                case AQShapeType::ConvexHull:
                    break;   // filtered above
                }
                cols.push_back(in);
            }
            colliderCount = static_cast<std::uint32_t>(cols.size());
            ok = backend->particlesUploadColliders(sys.id, cols);
        }

        if (ok) {
            AQComputeBackend::ParticleFrameDesc desc;
            desc.systemId = sys.id;
            desc.dt = dt;
            desc.substeps = static_cast<std::uint32_t>(substeps);
            desc.activeSpan = span;
            desc.injectStart = sys.emitBase;
            desc.injectCount = k;
            desc.newLiveCount = sys.liveCount;      // host census, post-age
            desc.fieldCount = static_cast<std::uint32_t>(sys.fields.size());
            desc.colliderCount = colliderCount;
            ok = backend->particlesEncodeFrame(desc);
        }
        if (!ok) {
            std::cerr << "AQUA::AQSpace[particles]: GPU frame encode failed for "
                         "system " << sys.id << " — device state did not advance.\n";
            continue;
        }
        sys.gpuDirty = true;
    }
}

void AQSpace::detachCompute() {
    impl->gpuBackend = nullptr;
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
        // GPU path: the velocity vectors below draw host positions — pull the
        // packed prefix down first (debug drain is a §14.1 sync point).
        AQrefreshHostParticleState(*sys, impl->gpuBackend);

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
