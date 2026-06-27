// AQUA Phase 5b — SoA body mirror + the precompiled-kernel toolchain.
//
// Two sections, mirroring the Phase 5 "CPU first, then GPU" posture:
//   * CPU (always runs): AQBodySoA gather/scatter round-trips AoS AQBodyState
//     through the struct-of-arrays mirror with full field fidelity.
//   * GPU (device-guarded): AQComputeBackend loads the precompiled
//     AQKernels.omegasllib and runs its capability probe (selfTest), proving the
//     precompile -> load -> pipeline -> buffer -> dispatch -> readback toolchain
//     end to end. Skips cleanly where no GTE device exists (headless CI).
//
// This is an INTERNAL test: it includes AQUA's src/ headers (AQBodySoA.h,
// AQComputeBackend.h) directly and links the AQUA library, which exports the
// symbols (default visibility). The kernel-lib path arrives as a compile
// definition (AQUA_KERNELS_LIB_PATH) pointing at the build-tree lib.

#include "AQBodySoA.h"
#include "AQComputeBackend.h"

#include <aqua/AQIntegrator.h>
#include <omegaGTE/GTEDevice.h>      // Init / Close / enumerateDevices / GTE
#include <omegaGTE/GE.h>            // makeCommandQueue
#include <omegaGTE/GECommandQueue.h> // GECommandQueueDesc

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// Build an AQBodyState<float> with a distinct value in every mirrored field, so
// a dropped/swapped field in gather or scatter shows up as a mismatch.
AQBodyState<float> makeState(float seed) {
    AQBodyState<float> s;
    s.position[0][0] = seed + 1.f;  s.position[1][0] = seed + 2.f;  s.position[2][0] = seed + 3.f;
    s.velocity[0][0] = seed + 4.f;  s.velocity[1][0] = seed + 5.f;  s.velocity[2][0] = seed + 6.f;
    s.orientation.x = seed + 7.f;   s.orientation.y = seed + 8.f;
    s.orientation.z = seed + 9.f;   s.orientation.w = seed + 10.f;
    s.angularVelBody[0][0] = seed + 11.f; s.angularVelBody[1][0] = seed + 12.f; s.angularVelBody[2][0] = seed + 13.f;
    s.invMass = seed + 14.f;
    s.invInertiaBody[0][0] = seed + 15.f; s.invInertiaBody[1][0] = seed + 16.f; s.invInertiaBody[2][0] = seed + 17.f;
    s.forceAccum[0][0] = seed + 18.f;  s.forceAccum[1][0] = seed + 19.f;  s.forceAccum[2][0] = seed + 20.f;
    s.torqueAccum[0][0] = seed + 21.f; s.torqueAccum[1][0] = seed + 22.f; s.torqueAccum[2][0] = seed + 23.f;
    s.linearDamping = seed + 24.f;  s.angularDamping = seed + 25.f;
    s.gravityScale = seed + 26.f;   s.maxAngularSpeed = seed + 27.f;
    s.comOffset[0][0] = seed + 28.f; s.comOffset[1][0] = seed + 29.f; s.comOffset[2][0] = seed + 30.f;
    s.activation = AQActivationState::Sleeping;
    return s;
}

bool statesEqual(const AQBodyState<float>& a, const AQBodyState<float>& b) {
    auto v3 = [](const AQVec3<float>& x, const AQVec3<float>& y) {
        return x[0][0] == y[0][0] && x[1][0] == y[1][0] && x[2][0] == y[2][0];
    };
    return v3(a.position, b.position) && v3(a.velocity, b.velocity) &&
           a.orientation.x == b.orientation.x && a.orientation.y == b.orientation.y &&
           a.orientation.z == b.orientation.z && a.orientation.w == b.orientation.w &&
           v3(a.angularVelBody, b.angularVelBody) &&
           a.invMass == b.invMass && v3(a.invInertiaBody, b.invInertiaBody) &&
           v3(a.forceAccum, b.forceAccum) && v3(a.torqueAccum, b.torqueAccum) &&
           a.linearDamping == b.linearDamping && a.angularDamping == b.angularDamping &&
           a.gravityScale == b.gravityScale && a.maxAngularSpeed == b.maxAngularSpeed &&
           v3(a.comOffset, b.comOffset) && a.activation == b.activation;
}

} // namespace

int main() {
    std::printf("== AQUA Phase 5b: SoA mirror + precompiled-kernel toolchain ==\n");

    // ---- CPU: AQBodySoA gather/scatter round-trip ----
    {
        const std::size_t N = 3;
        AQBodyState<float> in[N] = { makeState(0.f), makeState(100.f), makeState(200.f) };

        AQBodySoA soa;
        soa.gatherFrom(in, N);
        check(soa.size() == N, "gatherFrom sizes the SoA to the body count");
        check(soa.posX[1] == in[1].position[0][0] && soa.velZ[2] == in[2].velocity[2][0] &&
              soa.activation[0] == static_cast<std::uint8_t>(AQActivationState::Sleeping),
              "gatherFrom packs fields into the right array slots");

        AQBodyState<float> out[N];
        soa.scatterTo(out, N);
        bool allEqual = true;
        for (std::size_t i = 0; i < N; ++i) allEqual = allEqual && statesEqual(in[i], out[i]);
        check(allEqual, "gather -> scatter round-trips every field with full fidelity");

        // A mutation in the SoA reaches the AoS on scatter (the GPU-writeback path).
        soa.velX[0] = 1234.5f;
        soa.scatterTo(out, N);
        check(out[0].velocity[0][0] == 1234.5f, "SoA mutation scatters back to AoS");
    }

    // ---- GPU: precompiled-kernel toolchain probe (device-guarded) ----
    {
        OmegaCommon::Vector<SharedHandle<OmegaGTE::GTEDevice>> devices =
            OmegaGTE::enumerateDevices();
        if (devices.empty()) {
            std::printf("[SKIP] no GTE device on this host — kernel toolchain probe skipped\n");
        } else {
            SharedHandle<OmegaGTE::GTEDevice> dev = devices[0];
            OmegaGTE::GTE gte = OmegaGTE::Init(dev);
            check(static_cast<bool>(gte.graphicsEngine), "GTE initialized a graphics engine");

            OmegaGTE::GECommandQueueDesc queueDesc;
            queueDesc.maxBufferCount = 1;
            auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);

            auto backend = AQComputeBackend::TryCreate(gte.graphicsEngine, queue);
            check(backend != nullptr, "AQComputeBackend::TryCreate succeeds with a real engine");

            const bool loaded = backend->loadKernelLibrary(OmegaCommon::String(AQUA_KERNELS_LIB_PATH));
            check(loaded, "loadKernelLibrary loads the precompiled AQKernels.omegasllib");

            if (loaded) {
                check(backend->selfTest(),
                      "selfTest: GPU built+ran AQProbeDouble and doubled the buffer");
            }

            OmegaGTE::Close(gte);
        }
    }

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
    } else {
        std::printf("%d FAILURE(S)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
