/// OmegaSL §5.6 — atomic-operation GPU integration test.
/// Backend-independent: uses only the OmegaGTE public API + runtime OmegaSL
/// compilation, so the same source builds and runs on Metal, Vulkan, and
/// D3D12 (mirrors bitfield_ops_test.cpp). Headless — one compute dispatch then
/// a buffer readback.
///
/// The point of this test (beyond "it compiles") is to prove the atomics are
/// actually atomic on real hardware, and that load / store / exchange round-
/// trip a value correctly. A non-atomic `counter += 1` from 256 racing threads
/// would read back < 256; an atomic one reads back exactly 256.
///   counter  — every thread atomic_add(1)            -> 256
///   maxId    — every thread atomic_max(tid)          -> 255
///   orBits   — every thread atomic_or(1 << (tid&31)) -> 0xFFFFFFFF (all bits)
/// Thread 0 additionally exercises store / exchange / and / load on a scratch
/// slot and the signed-atomic path (these run on a single thread, so the
/// results are deterministic):
///   scratch  — store 100, exchange->7, and 6           -> 6
///   rExch    — exchange's returned original value       -> 100
///   rLoad    — atomic_load(scratch) after the `and`     -> 6
///   sBalance — signed: store -3, add 5                  -> 2
///
/// Atomic memory is declared with the new `atomic_uint` / `atomic_int` types;
/// on the CPU side the slots are plain uint/int bytes (the atomicity is a
/// shader-source concern), so the readback uses the ordinary getUint/getInt.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GTEShader.h>
#include "GTETestEntryPoint.h"

#include <iostream>

using namespace OmegaGTE;

namespace {

OmegaCommon::String kShaders = R"(

struct Data {
    atomic_uint counter;   // all threads atomic_add(1)  -> N
    atomic_uint maxId;     // all threads atomic_max(tid) -> N-1
    atomic_uint orBits;    // all threads atomic_or(bit)  -> 0xFFFFFFFF
    atomic_uint scratch;   // thread 0: store/exchange/and/load
    uint        rExch;     // thread 0: exchange's original value
    uint        rLoad;     // thread 0: atomic_load(scratch) after the `and`
    atomic_int  sBalance;  // thread 0: signed store + add
    atomic_uint casVal;    // thread 0: compare_exchange target
    uint        rCasHit;   // thread 0: CAS that succeeds -> original (5)
    uint        rCasMiss;  // thread 0: CAS that fails    -> original (42)
    atomic_uint wcas;      // ALL threads: weak-CAS increment loop -> N
};

buffer<Data> data : 0;

[inout data]
compute(x=64,y=1,z=1)
void atomicOps(uint3 tid : GlobalThreadID){
    // Contended accumulation across the whole grid.
    atomic_add(data[0].counter, 1u);
    atomic_max(data[0].maxId, tid[0]);
    atomic_or(data[0].orBits, 1u << (tid[0] & 31u));

    // Contended weak-CAS increment loop: every thread bumps `wcas` by 1 via the
    // canonical retry loop. Under real contention the CAS genuinely fails and
    // retries (exercising the in-place `expected` update); the final value must
    // equal the thread count exactly. The weak CAS is a loop-body *statement*
    // (the portable form — not a bare while-condition; see atomic_ops.omegasl).
    uint expected = atomic_load(data[0].wcas);
    bool done = false;
    while(!done){
        done = atomic_compare_exchange_weak(data[0].wcas, expected, expected + 1u);
    }

    // Single-thread, race-free sequence: load / store / exchange + signed + CAS.
    if(tid[0] == 0u){
        atomic_store(data[0].scratch, 100u);
        uint old = atomic_exchange(data[0].scratch, 7u);   // old == 100
        data[0].rExch = old;
        atomic_and(data[0].scratch, 6u);                   // 7 & 6 == 6
        data[0].rLoad = atomic_load(data[0].scratch);      // 6

        atomic_store(data[0].sBalance, -3);
        atomic_add(data[0].sBalance, 5);                   // 2

        // Strong CAS: first matches (5 -> 42, returns 5); second mismatches
        // (current 42 != 5, no store, returns 42). casVal ends at 42.
        atomic_store(data[0].casVal, 5u);
        data[0].rCasHit  = atomic_compare_exchange(data[0].casVal, 5u, 42u);
        data[0].rCasMiss = atomic_compare_exchange(data[0].casVal, 5u, 99u);
    }
}

)";

constexpr unsigned kThreads = 256;   // 4 threadgroups of 64

bool &failFlag() { static bool f = false; return f; }

void expectU(const char *name, unsigned got, unsigned want) {
    if (got != want) {
        std::cerr << "  FAIL " << name << " = " << got << " expected " << want << "\n";
        failFlag() = true;
    }
}
void expectI(const char *name, int got, int want) {
    if (got != want) {
        std::cerr << "  FAIL " << name << " = " << got << " expected " << want << "\n";
        failFlag() = true;
    }
}

}  // namespace

GTE_TEST_ENTRY_POINT {
    (void)argc;
    (void)argv;
    auto gte = OmegaGTE::InitWithDefaultDevice();

    auto compiled = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(kShaders)});
    auto lib = gte.graphicsEngine->loadShaderLibraryRuntime(compiled);

    ComputePipelineDescriptor pd{};
    pd.computeFunc = lib->shaders["atomicOps"];
    if (!pd.computeFunc) {
        std::cerr << "atomicOps shader not found\n";
        OmegaGTE::Close(gte);
        return 1;
    }
    auto pipeline = gte.graphicsEngine->makeComputePipelineState(pd);

    // The atomic slots are plain uint/int bytes on the CPU side.
    const auto layout = OmegaCommon::Vector<omegasl_data_type>{
        OMEGASL_UINT, OMEGASL_UINT, OMEGASL_UINT, OMEGASL_UINT,
        OMEGASL_UINT, OMEGASL_UINT, OMEGASL_INT,
        OMEGASL_UINT, OMEGASL_UINT, OMEGASL_UINT, OMEGASL_UINT};
    const size_t size = omegaSLStructStride(layout);

    auto buf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, size, size});

    // Zero-initialize every slot before the dispatch.
    auto writer = GEBufferWriter::Create();
    writer->setOutputBuffer(buf);
    writer->structBegin();
    unsigned zeroU = 0u; int zeroI = 0;
    writer->writeUint(zeroU);  // counter
    writer->writeUint(zeroU);  // maxId
    writer->writeUint(zeroU);  // orBits
    writer->writeUint(zeroU);  // scratch
    writer->writeUint(zeroU);  // rExch
    writer->writeUint(zeroU);  // rLoad
    writer->writeInt(zeroI);   // sBalance
    writer->writeUint(zeroU);  // casVal
    writer->writeUint(zeroU);  // rCasHit
    writer->writeUint(zeroU);  // rCasMiss
    writer->writeUint(zeroU);  // wcas
    writer->structEnd();
    writer->sendToBuffer();
    writer->flush();

    OmegaGTE::GECommandQueueDesc queueDesc{};
    queueDesc.maxBufferCount = 1;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);
    auto cmd = queue->getAvailableBuffer();
    GEComputePassDescriptor pass{};
    cmd->startComputePass(pass);
    cmd->setComputePipelineState(pipeline);
    cmd->bindResourceAtComputeShader(buf, 0);
    cmd->dispatchThreads(kThreads, 1, 1);   // 256 total threads
    cmd->finishComputePass();
    queue->submitCommandBuffer(cmd);
    queue->commitToGPUAndWait();

    auto reader = GEBufferReader::Create();
    reader->setInputBuffer(buf);
    reader->setStructLayout(layout);
    reader->structBegin();
    unsigned counter = 0, maxId = 0, orBits = 0, scratch = 0, rExch = 0, rLoad = 0;
    int sBalance = 0;
    unsigned casVal = 0, rCasHit = 0, rCasMiss = 0, wcas = 0;
    reader->getUint(counter);
    reader->getUint(maxId);
    reader->getUint(orBits);
    reader->getUint(scratch);
    reader->getUint(rExch);
    reader->getUint(rLoad);
    reader->getInt(sBalance);
    reader->getUint(casVal);
    reader->getUint(rCasHit);
    reader->getUint(rCasMiss);
    reader->getUint(wcas);
    reader->structEnd();
    reader->reset();

    expectU("counter (atomic_add x256)", counter, kThreads);
    expectU("maxId (atomic_max)",        maxId, kThreads - 1);
    expectU("orBits (atomic_or)",        orBits, 0xFFFFFFFFu);
    expectU("scratch (store/exch/and)",  scratch, 6u);
    expectU("rExch (exchange original)", rExch, 100u);
    expectU("rLoad (atomic_load)",       rLoad, 6u);
    expectI("sBalance (signed add)",     sBalance, 2);
    expectU("casVal (CAS final)",        casVal, 42u);
    expectU("rCasHit (CAS success orig)",rCasHit, 5u);
    expectU("rCasMiss (CAS fail orig)",  rCasMiss, 42u);
    expectU("wcas (weak-CAS loop x256)", wcas, kThreads);

    OmegaGTE::Close(gte);

    const bool ok = !failFlag();
    std::cout << (ok ? "PASS: atomic ops" : "FAIL: atomic ops") << "\n";
    return ok ? 0 : 1;
}
