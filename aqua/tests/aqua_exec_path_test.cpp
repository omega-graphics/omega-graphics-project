// AQUA Phase 5a — execution-path selection + the factory split.
//
// Phase 5 gives AQContext a GPU compute path with the CPU path retained as the
// runtime-selected fallback (Physics-Roadmap §3 principle 3). 5a lands the
// groundwork: the mandatory-engine `Create(engine, queue)` factory, the
// engine-less `CreateCPUOnly()` factory for pure-CPU use, and `AQExecPath` /
// `setExecutionPath` / `executionPath`. No kernels are ported yet, so every
// device resolves to the CPU path — this test asserts exactly that.
//
// Two sections: the CPU-only assertions always run (no GTE device needed); the
// mandatory-engine section is guarded by `enumerateDevices()` and skips cleanly
// on a host with no usable device (headless CI). This is the "CPU test first,
// then the GPU test" posture the Phase 5 suite follows — the GPU section grows
// real GPU assertions as kernels land in 5c+.

#include <aqua/AQContext.h>
#include <aqua/AQSpace.h>
#include <aqua/AQMath.h>

#include <omegaGTE/GTEDevice.h>   // Init / Close / enumerateDevices / GTE

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

} // namespace

int main() {
    std::printf("== AQUA Phase 5a: execution-path selection ==\n");

    // ---- CPU-only context (no engine): always the CPU path ----
    {
        auto ctx = AQContext::CreateCPUOnly();
        check(ctx->executionPath() == AQExecPath::CPU,
              "CreateCPUOnly resolves to the CPU path");

        // Auto on an engine-less context stays CPU (no usable backend exists).
        ctx->setExecutionPath(AQExecPath::Auto);
        check(ctx->executionPath() == AQExecPath::CPU,
              "Auto with no engine resolves to CPU");

        // Forcing GPU with no engine warns (to stderr) and falls back to CPU —
        // a misuse degrades, it does not crash.
        ctx->setExecutionPath(AQExecPath::GPU);
        check(ctx->executionPath() == AQExecPath::CPU,
              "GPU requested without an engine falls back to CPU");

        // Forcing CPU is honored.
        ctx->setExecutionPath(AQExecPath::CPU);
        check(ctx->executionPath() == AQExecPath::CPU, "CPU is honored");

        // A CPU-only context still simulates: a dynamic body falls under the
        // default gravity. Proves CreateCPUOnly produces a working sim (no queue
        // is touched on the CPU path).
        auto space = ctx->createSpace();
        AQBodyDesc d;
        d.mass = 1.f;
        d.position = AQvec3(0.f, 10.f, 0.f);
        auto body = space->addBody(d);
        const float y0 = body->position()[1][0];
        for (int i = 0; i < 60; ++i) ctx->advance(1.f / 60.f);
        check(body->position()[1][0] < y0 - 1.f,
              "CreateCPUOnly context steps the simulation (body fell)");
    }

    // ---- Mandatory-engine path, guarded by device availability ----
    {
        OmegaCommon::Vector<SharedHandle<OmegaGTE::GTEDevice>> devices =
            OmegaGTE::enumerateDevices();
        if (devices.empty()) {
            std::printf("[SKIP] no GTE device on this host — mandatory-engine "
                        "construction test skipped\n");
        } else {
            SharedHandle<OmegaGTE::GTEDevice> dev = devices[0];
            OmegaGTE::GTE gte = OmegaGTE::Init(dev);
            check(static_cast<bool>(gte.graphicsEngine),
                  "GTE initialized a graphics engine");

            // The mandatory-engine factory. A null queue is fine for 5a: with no
            // kernels ported the backend reports unusable, so the resolved path
            // is CPU regardless. (5c flips this to GPU once the kernels + the
            // compute-pipeline capability probe land — update this expectation
            // then.)
            auto ctx = AQContext::Create(gte.graphicsEngine,
                                         SharedHandle<OmegaGTE::GECommandQueue>());
            check(ctx->executionPath() == AQExecPath::CPU,
                  "Phase 5a: engine present but no kernels yet -> CPU path");

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
