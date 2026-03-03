#include <OmegaGTE.h>
#include <omegaGTE/GTEShader.h>

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <iostream>
#include <cassert>

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

int main(int argc, const char * argv[]){
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

    size_t structSize = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT4});
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

    auto commandQueue = gte.graphicsEngine->makeCommandQueue(1);
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

    if(allCorrect){
        std::cout << "PASS: All " << elementCount << " elements doubled correctly." << std::endl;
    } else {
        std::cout << "FAIL: Some elements were incorrect." << std::endl;
    }

    OmegaGTE::Close(gte);
    return allCorrect ? 0 : 1;
}
