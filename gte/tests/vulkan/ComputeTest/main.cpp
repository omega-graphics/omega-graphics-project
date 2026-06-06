#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>

#include <iostream>
#include <cassert>
#include <cmath>

/// Both compute shaders compiled in a single pass. The §12.1/§12.2
/// matrix round-trip shares the input/output buffer bindings (slots 0
/// and 1) with the doubleValues shader; their structs are distinct
/// types under unique names so the OmegaSL compiler treats them as
/// independent declarations.
OmegaCommon::String computeShader = R"(

struct Data {
    float4 value;
};

struct MatrixData {
    float4x4 m44;
    float3x3 m33;
    float3x2 m32;
};

buffer<Data> inputBuf : 0;
buffer<Data> outputBuf : 1;

buffer<MatrixData> matInputBuf : 0;
buffer<MatrixData> matOutputBuf : 1;

[in inputBuf, out outputBuf]
compute(x=64,y=1,z=1)
void doubleValues(uint3 tid : GlobalThreadID){
    outputBuf[tid[0]].value = inputBuf[tid[0]].value * 2.0;
}

[in matInputBuf, out matOutputBuf]
compute(x=1, y=1, z=1)
void copyMatrices(uint3 tid : GlobalThreadID){
    matOutputBuf[0].m44 = matInputBuf[0].m44;
    matOutputBuf[0].m33 = matInputBuf[0].m33;
    matOutputBuf[0].m32 = matInputBuf[0].m32;
}

)";

static bool runMatrixRoundtrip(OmegaGTE::GTE &gte,
                               SharedHandle<OmegaGTE::GTEShaderLibrary> &funcLib){
    using namespace OmegaGTE;
    std::cout << "\n--- §12.1/§12.2 matrix round-trip ---" << std::endl;

    ComputePipelineDescriptor pipelineDesc;
    pipelineDesc.computeFunc = funcLib->shaders["copyMatrices"];
    assert(pipelineDesc.computeFunc && "copyMatrices not found");
    auto pipeline = gte.graphicsEngine->makeComputePipelineState(pipelineDesc);

    /// Match the shader's struct field order. The stride helper accounts
    /// for the std430 column padding on Cx3 (m33's columns occupy 16
    /// bytes each instead of 12).
    auto layout = OmegaCommon::Vector<omegasl_data_type>{
        OMEGASL_FLOAT4x4, OMEGASL_FLOAT3x3, OMEGASL_FLOAT3x2,
    };
    size_t structSize = omegaSLStructStride(layout);

    BufferDescriptor inDesc{BufferDescriptor::Upload,  structSize, structSize};
    BufferDescriptor outDesc{BufferDescriptor::Upload, structSize, structSize};
    auto inBuf = gte.graphicsEngine->makeBuffer(inDesc);
    auto outBuf = gte.graphicsEngine->makeBuffer(outDesc);

    /// Build matrices with non-symmetric, non-trivial values so a stray
    /// transpose / wrong-column read produces a different element on
    /// readback. `m[c][r] = c * 100 + r * 10 + 1` makes every element
    /// uniquely identifiable.
    auto m44 = FMatrix<4,4>::Create();
    for(unsigned c = 0; c < 4; ++c)
        for(unsigned r = 0; r < 4; ++r)
            m44[c][r] = float(c * 100 + r * 10 + 1);
    auto m33 = FMatrix<3,3>::Create();
    for(unsigned c = 0; c < 3; ++c)
        for(unsigned r = 0; r < 3; ++r)
            m33[c][r] = float(c * 100 + r * 10 + 7);
    auto m32 = FMatrix<3,2>::Create();
    for(unsigned c = 0; c < 3; ++c)
        for(unsigned r = 0; r < 2; ++r)
            m32[c][r] = float(c * 100 + r * 10 + 3);

    auto writer = GEBufferWriter::Create();
    writer->setOutputBuffer(inBuf);
    writer->structBegin();
    writer->writeFloat4x4(m44);
    writer->writeFloat3x3(m33);
    writer->writeFloat3x2(m32);
    writer->structEnd();
    writer->sendToBuffer();
    writer->flush();

    OmegaGTE::GECommandQueueDesc queueDesc{};
    queueDesc.maxBufferCount = 1;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);
    auto cmd = queue->getAvailableBuffer();
    GEComputePassDescriptor pass;
    cmd->startComputePass(pass);
    cmd->setComputePipelineState(pipeline);
    cmd->bindResourceAtComputeShader(inBuf, 0);
    cmd->bindResourceAtComputeShader(outBuf, 1);
    cmd->dispatchThreads(1, 1, 1);
    cmd->finishComputePass();
    queue->submitCommandBuffer(cmd);
    queue->commitToGPUAndWait();

    auto reader = GEBufferReader::Create();
    reader->setInputBuffer(outBuf);
    reader->setStructLayout(layout);
    reader->structBegin();
    auto m44_back = FMatrix<4,4>::Create();
    auto m33_back = FMatrix<3,3>::Create();
    auto m32_back = FMatrix<3,2>::Create();
    reader->getFloat4x4(m44_back);
    reader->getFloat3x3(m33_back);
    reader->getFloat3x2(m32_back);
    reader->structEnd();
    reader->reset();

    bool ok = true;
    auto check = [&](const char *name, unsigned C, unsigned R, auto &orig, auto &back){
        for(unsigned c = 0; c < C; ++c){
            for(unsigned r = 0; r < R; ++r){
                if(std::abs(orig[c][r] - back[c][r]) > 0.0001f){
                    std::cerr << "FAIL " << name << "[" << c << "][" << r << "]: "
                              << "wrote " << orig[c][r] << " read " << back[c][r] << std::endl;
                    ok = false;
                }
            }
        }
    };
    check("m44", 4, 4, m44, m44_back);
    check("m33", 3, 3, m33, m33_back);
    check("m32", 3, 2, m32, m32_back);

    if(ok){
        std::cout << "PASS: matrix round-trip preserves every element." << std::endl;
    } else {
        std::cout << "FAIL: matrix round-trip mismatch." << std::endl;
    }
    return ok;
}

int main(int argc, char *argv[]){
    auto gte = OmegaGTE::InitWithDefaultDevice();
    std::cout << "GTE Initialized" << std::endl;

    auto compiledLib = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(computeShader)});
    auto funcLib = gte.graphicsEngine->loadShaderLibraryRuntime(compiledLib);

    std::cout << "Shader library loaded, shader count: " << funcLib->shaders.size() << std::endl;

    OmegaGTE::ComputePipelineDescriptor pipelineDesc;
    pipelineDesc.computeFunc = funcLib->shaders["doubleValues"];
    assert(pipelineDesc.computeFunc && "Compute shader not found");

    auto computePipeline = gte.graphicsEngine->makeComputePipelineState(pipelineDesc);
    std::cout << "Compute pipeline created" << std::endl;

    size_t structSize = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4});
    constexpr unsigned elementCount = 64;

    OmegaGTE::BufferDescriptor inputDesc{OmegaGTE::BufferDescriptor::Upload, elementCount * structSize, structSize};
    auto inputBuffer = gte.graphicsEngine->makeBuffer(inputDesc);

    OmegaGTE::BufferDescriptor outputDesc{OmegaGTE::BufferDescriptor::Upload, elementCount * structSize, structSize};
    auto outputBuffer = gte.graphicsEngine->makeBuffer(outputDesc);

    auto writer = OmegaGTE::GEBufferWriter::Create();
    writer->setOutputBuffer(inputBuffer);
    for(unsigned i = 0; i < elementCount; i++){
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
    std::cout << "Input buffer written" << std::endl;

    OmegaGTE::GECommandQueueDesc commandQueueDesc{};
    commandQueueDesc.maxBufferCount = 1;
    auto commandQueue = gte.graphicsEngine->makeCommandQueue(commandQueueDesc);
    auto cmdBuf = commandQueue->getAvailableBuffer();

    OmegaGTE::GEComputePassDescriptor compDesc;
    cmdBuf->startComputePass(compDesc);
    cmdBuf->setComputePipelineState(computePipeline);
    cmdBuf->bindResourceAtComputeShader(inputBuffer, 0);
    cmdBuf->bindResourceAtComputeShader(outputBuffer, 1);
    cmdBuf->dispatchThreads(elementCount, 1, 1);
    cmdBuf->finishComputePass();

    commandQueue->submitCommandBuffer(cmdBuf);
    commandQueue->commitToGPUAndWait();
    std::cout << "Compute dispatch completed" << std::endl;

    auto reader = OmegaGTE::GEBufferReader::Create();
    reader->setInputBuffer(outputBuffer);
    reader->setStructLayout({OMEGASL_FLOAT4});

    bool allCorrect = true;
    for(unsigned i = 0; i < elementCount; i++){
        reader->structBegin();
        auto v = OmegaGTE::FVec<4>::Create();
        reader->getFloat4(v);
        reader->structEnd();

        float expected0 = float(i) * 2.f;
        float expected1 = float(i) * 20.f;
        float expected2 = float(i) * 200.f;
        float expected3 = float(i) * 2000.f;

        if(std::abs(v[0][0] - expected0) > 0.001f ||
           std::abs(v[1][0] - expected1) > 0.001f ||
           std::abs(v[2][0] - expected2) > 0.001f ||
           std::abs(v[3][0] - expected3) > 0.001f){
            std::cerr << "FAIL at index " << i << ": got ("
                      << v[0][0] << ", " << v[1][0] << ", " << v[2][0] << ", " << v[3][0]
                      << ") expected ("
                      << expected0 << ", " << expected1 << ", " << expected2 << ", " << expected3 << ")" << std::endl;
            allCorrect = false;
        }
    }

    /// Symmetric with the writer's `flush()` — without this, the
    /// VMA allocation backing `outputBuffer` is destroyed while still
    /// mapped (asserts in debug builds of vk_mem_alloc).
    reader->reset();

    if(allCorrect){
        std::cout << "PASS: All " << elementCount << " elements doubled correctly." << std::endl;
    } else {
        std::cout << "FAIL: Some elements were incorrect." << std::endl;
    }

    bool matrixOk = runMatrixRoundtrip(gte, funcLib);

    OmegaGTE::Close(gte);
    return (allCorrect && matrixOk) ? 0 : 1;
}
