#include "AQComputeBackend.h"

// Phase 5b: AQUA now links OmegaGTE and defines the target-platform macro
// (aqua/CMakeLists.txt), so the GPU headers compile here. The Metal-specific
// Objective-C bits in GE.h are `#ifdef __OBJC__`-guarded, so this stays a .cpp —
// every call below is the public C++ GTE surface (no Metal types).
#include <omegaGTE/GE.h>             // OmegaGraphicsEngine, GEBuffer, BufferDescriptor
#include <omegaGTE/GEPipeline.h>     // ComputePipelineDescriptor, GEComputePipelineState
#include <omegaGTE/GERenderTarget.h> // GECommandBuffer compute pass
#include <omegaGTE/GECommandQueue.h> // GECommandQueue, GEComputePassDescriptor
#include <omegaGTE/GTEShader.h>      // GTEShaderLibrary, GEBufferWriter/Reader, omegaSLStructStride
#include <omegaGTE/GTEMath.h>        // FVec<4>
#include <omegasl.h>                 // OMEGASL_FLOAT4
#include <omega-common/fs.h>         // OmegaCommon::FS::Path

#include <cmath>
#include <iostream>
#include <utility>

AQComputeBackend::AQComputeBackend(SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
                                   SharedHandle<OmegaGTE::GECommandQueue> queue)
    : gpuEngine(std::move(engine)), cmdQueue(std::move(queue)) {}

std::unique_ptr<AQComputeBackend> AQComputeBackend::TryCreate(
    SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
    SharedHandle<OmegaGTE::GECommandQueue> queue) {
    // No engine ⇒ CPU-only context: there is no device to dispatch kernels to.
    if (!engine) {
        return nullptr;
    }

    // Phase 5a/5b: hold the engine + queue. `gpuUsable` stays false until the
    // full GPU step pipeline is ported (5f/5g); 5b proves the toolchain via
    // loadKernelLibrary + selfTest but does not yet route the step to the GPU.
    return std::unique_ptr<AQComputeBackend>(
        new AQComputeBackend(std::move(engine), std::move(queue)));
}

bool AQComputeBackend::loadKernelLibrary(const OmegaCommon::String& path) {
    if (!gpuEngine) {
        return false;
    }
    kernelLib = gpuEngine->loadShaderLibrary(OmegaCommon::FS::Path(path));
    return static_cast<bool>(kernelLib);
}

bool AQComputeBackend::selfTest() {
    if (!gpuEngine || !cmdQueue) {
        std::cerr << "AQComputeBackend::selfTest: engine or command queue is null\n";
        return false;
    }
    if (!kernelLib) {
        std::cerr << "AQComputeBackend::selfTest: kernel library not loaded "
                     "(call loadKernelLibrary first)\n";
        return false;
    }

    auto it = kernelLib->shaders.find("AQProbeDouble");
    if (it == kernelLib->shaders.end() || !it->second) {
        std::cerr << "AQComputeBackend::selfTest: AQProbeDouble kernel not found "
                     "in the loaded library\n";
        return false;
    }

    OmegaGTE::ComputePipelineDescriptor pipelineDesc;
    pipelineDesc.name = "AQProbeDouble";
    pipelineDesc.computeFunc = it->second;
    auto pipeline = gpuEngine->makeComputePipelineState(pipelineDesc);
    if (!pipeline) {
        std::cerr << "AQComputeBackend::selfTest: failed to build the compute pipeline\n";
        return false;
    }

    constexpr unsigned kElementCount = 64;
    const size_t structSize = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4});

    OmegaGTE::BufferDescriptor inDesc{OmegaGTE::BufferDescriptor::Upload,
                                      kElementCount * structSize, structSize};
    OmegaGTE::BufferDescriptor outDesc{OmegaGTE::BufferDescriptor::Upload,
                                       kElementCount * structSize, structSize};
    auto inBuffer = gpuEngine->makeBuffer(inDesc);
    auto outBuffer = gpuEngine->makeBuffer(outDesc);
    if (!inBuffer || !outBuffer) {
        std::cerr << "AQComputeBackend::selfTest: failed to allocate probe buffers\n";
        return false;
    }

    // Fill the input with (i, 10i, 100i, 1000i).
    auto writer = OmegaGTE::GEBufferWriter::Create();
    writer->setOutputBuffer(inBuffer);
    for (unsigned i = 0; i < kElementCount; ++i) {
        auto v = OmegaGTE::FVec<4>::Create();
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

    auto cmdBuf = cmdQueue->getAvailableBuffer();
    OmegaGTE::GEComputePassDescriptor passDesc;
    cmdBuf->startComputePass(passDesc);
    cmdBuf->setComputePipelineState(pipeline);
    cmdBuf->bindResourceAtComputeShader(inBuffer, 0);
    cmdBuf->bindResourceAtComputeShader(outBuffer, 1);
    cmdBuf->dispatchThreads(kElementCount, 1, 1);
    cmdBuf->finishComputePass();
    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();

    auto reader = OmegaGTE::GEBufferReader::Create();
    reader->setInputBuffer(outBuffer);
    reader->setStructLayout({OMEGASL_FLOAT4});
    for (unsigned i = 0; i < kElementCount; ++i) {
        reader->structBegin();
        auto v = OmegaGTE::FVec<4>::Create();
        reader->getFloat4(v);
        reader->structEnd();
        if (std::fabs(v[0][0] - float(i) * 2.f) > 1e-3f ||
            std::fabs(v[1][0] - float(i) * 20.f) > 1e-3f ||
            std::fabs(v[2][0] - float(i) * 200.f) > 1e-3f ||
            std::fabs(v[3][0] - float(i) * 2000.f) > 1e-3f) {
            std::cerr << "AQComputeBackend::selfTest: GPU output mismatch at " << i << "\n";
            return false;
        }
    }
    return true;
}
