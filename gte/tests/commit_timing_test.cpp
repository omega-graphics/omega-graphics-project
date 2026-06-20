// GPU-Commit-Timing plan — P2 + Metal verification.
//
// Exercises the new whole-commit GPU-timing API end-to-end on a real device:
//   - GECommandQueue::commitToGPUAndWaitTimed()  (synchronous form)
//   - GECommandQueue::commitToGPU(GECommitCompletionHandler) (async form)
// against a small compute commit (the same "double every value" kernel the
// Metal ComputeTest uses), and prints the measured GPU time.
//
// Shared, backend-independent source: it only touches the public OmegaGTE API,
// so the same file verifies Vulkan / D3D12 once their P1 per-buffer timestamp
// writes land. On a backend whose per-buffer timing is not wired yet the timing
// assertions relax to ">= 0" (status + buffer count must still be correct).

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>

#include <cassert>
#include <cmath>
#include <future>
#include <iostream>

using namespace OmegaGTE;

namespace {

OmegaCommon::String computeShader = R"(

struct Data {
    float4 value;
};

buffer<Data> inputBuf : 0;
buffer<Data> outputBuf : 1;

[in inputBuf, out outputBuf]
compute(x=64,y=1,z=1)
void doubleValues(uint3 tid : GlobalThreadID){
    outputBuf[tid[0]].value = inputBuf[tid[0]].value * 2.0;
}

)";

constexpr unsigned elementCount = 64;

// Whether the default device advertises GPU timestamp support. When true we
// require a strictly-positive GPU duration; otherwise we only require the span
// to be non-negative (the API still reports status + buffer count).
bool deviceSupportsTimestamps() {
    auto devices = enumerateDevices();
    for(auto & device : devices) {
        if(device->features.hasFeature(GTEDEVICE_FEATURE_TIMESTAMP_QUERIES)) {
            std::cout << "Device '" << device->name
                      << "' supports timestamp queries; timestampPeriod="
                      << device->features.timestampPeriod << " ns/tick" << std::endl;
            return true;
        }
    }
    std::cout << "No device advertises GTEDEVICE_FEATURE_TIMESTAMP_QUERIES" << std::endl;
    return false;
}

// Build, bind and dispatch one compute command buffer onto `queue`, returning
// the submitted (but not yet committed) buffer. Leaves the batch enqueued so
// the caller can pick how to commit it.
void recordAndSubmit(SharedHandle<GEComputePipelineState> & pipeline,
                     SharedHandle<GEBuffer> & inputBuffer,
                     SharedHandle<GEBuffer> & outputBuffer,
                     SharedHandle<GECommandQueue> & queue) {
    auto cmdBuf = queue->getAvailableBuffer();
    GEComputePassDescriptor compDesc;
    cmdBuf->startComputePass(compDesc);
    cmdBuf->setComputePipelineState(pipeline);
    cmdBuf->bindResourceAtComputeShader(inputBuffer, 0);
    cmdBuf->bindResourceAtComputeShader(outputBuffer, 1);
    cmdBuf->dispatchThreads(elementCount, 1, 1);
    cmdBuf->finishComputePass();
    queue->submitCommandBuffer(cmdBuf);
}

bool checkOutputDoubled(SharedHandle<GEBuffer> & outputBuffer) {
    auto reader = GEBufferReader::Create();
    reader->setInputBuffer(outputBuffer);
    reader->setStructLayout({OMEGASL_FLOAT4});
    bool ok = true;
    for(unsigned i = 0; i < elementCount; i++) {
        reader->structBegin();
        auto v = FVec<4>::Create();
        reader->getFloat4(v);
        reader->structEnd();
        if(std::abs(v[0][0] - float(i) * 2.f)    > 0.001f ||
           std::abs(v[1][0] - float(i) * 20.f)   > 0.001f ||
           std::abs(v[2][0] - float(i) * 200.f)  > 0.001f ||
           std::abs(v[3][0] - float(i) * 2000.f) > 0.001f) {
            std::cerr << "FAIL at index " << i << std::endl;
            ok = false;
        }
    }
    return ok;
}

} // namespace

int main(int argc, const char * argv[]) {
    (void)argc;
    (void)argv;

    auto gte = InitWithDefaultDevice();
    std::cout << "GTE Initialized" << std::endl;

    const bool wantPositiveTiming = deviceSupportsTimestamps();

    auto compiledLib = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(computeShader)});
    auto funcLib = gte.graphicsEngine->loadShaderLibraryRuntime(compiledLib);

    ComputePipelineDescriptor pipelineDesc;
    pipelineDesc.computeFunc = funcLib->shaders["doubleValues"];
    assert(pipelineDesc.computeFunc && "Compute shader not found");
    auto computePipeline = gte.graphicsEngine->makeComputePipelineState(pipelineDesc);

    size_t structSize = omegaSLStructStride({OMEGASL_FLOAT4});
    BufferDescriptor inputDesc{BufferDescriptor::Upload, elementCount * structSize, structSize};
    auto inputBuffer = gte.graphicsEngine->makeBuffer(inputDesc);
    BufferDescriptor outputDesc{BufferDescriptor::Upload, elementCount * structSize, structSize};
    auto outputBuffer = gte.graphicsEngine->makeBuffer(outputDesc);

    auto writer = GEBufferWriter::Create();
    writer->setOutputBuffer(inputBuffer);
    for(unsigned i = 0; i < elementCount; i++) {
        auto v = FVec<4>::Create();
        v[0][0] = float(i);
        v[1][0] = float(i) * 10.f;
        v[2][0] = float(i) * 100.f;
        v[3][0] = float(i) * 1000.f;
        writer->structBegin();
        writer->writeFloat4(v);
        writer->structEnd();
        writer->sendToBuffer();
    }
    writer->flush();

    GECommandQueueDesc commandQueueDesc{};
    commandQueueDesc.maxBufferCount = 2;
    auto commandQueue = gte.graphicsEngine->makeCommandQueue(commandQueueDesc);

    bool pass = true;

    // ── 1. Synchronous form: commitToGPUAndWaitTimed() ──────────────────────
    {
        recordAndSubmit(computePipeline, inputBuffer, outputBuffer, commandQueue);
        GECommitCompletionInfo info = commandQueue->commitToGPUAndWaitTimed();

        std::cout << "[sync ] status="
                  << (info.status == GECommandBufferCompletionInfo::CompletionStatus::Completed ? "Completed" : "Error")
                  << " buffers=" << info.commandBufferCount
                  << " gpuDuration=" << (info.gpuDurationSec() * 1000.0) << " ms" << std::endl;

        if(info.status != GECommandBufferCompletionInfo::CompletionStatus::Completed) {
            std::cerr << "FAIL: sync commit status not Completed" << std::endl;
            pass = false;
        }
        if(info.commandBufferCount != 1) {
            std::cerr << "FAIL: sync commit reported " << info.commandBufferCount
                      << " buffers, expected 1" << std::endl;
            pass = false;
        }
        if(info.gpuDurationSec() < 0.0) {
            std::cerr << "FAIL: sync commit negative GPU duration" << std::endl;
            pass = false;
        }
        if(wantPositiveTiming && info.gpuDurationSec() <= 0.0) {
            std::cerr << "FAIL: timestamps supported but sync GPU duration was "
                      << info.gpuDurationSec() << std::endl;
            pass = false;
        }
        if(!checkOutputDoubled(outputBuffer)) {
            std::cerr << "FAIL: sync commit produced wrong compute output" << std::endl;
            pass = false;
        }
    }

    // ── 2. Asynchronous form: commitToGPU(handler) fires exactly once ───────
    {
        std::promise<GECommitCompletionInfo> promise;
        auto future = promise.get_future();
        std::atomic<int> fireCount{0};

        recordAndSubmit(computePipeline, inputBuffer, outputBuffer, commandQueue);
        commandQueue->commitToGPU([&promise, &fireCount](const GECommitCompletionInfo & info) {
            if(fireCount.fetch_add(1) == 0) {
                promise.set_value(info);
            }
        });

        GECommitCompletionInfo info = future.get(); // blocks until the GPU finishes
        std::cout << "[async] status="
                  << (info.status == GECommandBufferCompletionInfo::CompletionStatus::Completed ? "Completed" : "Error")
                  << " buffers=" << info.commandBufferCount
                  << " gpuDuration=" << (info.gpuDurationSec() * 1000.0) << " ms"
                  << " fireCount=" << fireCount.load() << std::endl;

        if(info.status != GECommandBufferCompletionInfo::CompletionStatus::Completed) {
            std::cerr << "FAIL: async commit status not Completed" << std::endl;
            pass = false;
        }
        if(info.commandBufferCount != 1) {
            std::cerr << "FAIL: async commit reported " << info.commandBufferCount
                      << " buffers, expected 1" << std::endl;
            pass = false;
        }
        if(info.gpuDurationSec() < 0.0) {
            std::cerr << "FAIL: async commit negative GPU duration" << std::endl;
            pass = false;
        }
        if(wantPositiveTiming && info.gpuDurationSec() <= 0.0) {
            std::cerr << "FAIL: timestamps supported but async GPU duration was "
                      << info.gpuDurationSec() << std::endl;
            pass = false;
        }
        // The handler must fire exactly once for the batch.
        if(fireCount.load() != 1) {
            std::cerr << "FAIL: async commit handler fired " << fireCount.load()
                      << " times, expected 1" << std::endl;
            pass = false;
        }
    }

    if(pass) {
        std::cout << "PASS: commit GPU timing verified." << std::endl;
    } else {
        std::cout << "FAIL: commit GPU timing checks failed." << std::endl;
    }

    Close(gte);
    return pass ? 0 : 1;
}
