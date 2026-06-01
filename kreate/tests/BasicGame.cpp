#include <kreate/App.h>
#include <kreate/Mesh.h>
#include <kreate/Object.h>
#include <kreate/Pipeline.h>
#include <kreate/Scene.h>
#include <array>
#include <chrono>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;

// One CPU vertex in the Phase 1 Position+Color contract — laid out in
// the exact order `MeshDesc::attributes = Position | Color` declares.
// Tightly packed (28 bytes); the Mesh factory unpacks it into the GPU's
// std430-padded layout.
struct CpuVertex {
    float x, y, z;
    float r, g, b, a;
};

// Build the 36 vertices of a unit cube (6 faces * 2 triangles * 3
// verts), each face flat-shaded with a distinct color so rotation is
// visible. No index buffer — keeps Phase 1 plumbing minimal.
std::array<CpuVertex, 36> makeCubeVertices() {
    constexpr float s = 0.5f;
    const std::array<float, 4> red    = {1.f, 0.2f, 0.2f, 1.f};
    const std::array<float, 4> green  = {0.2f, 1.f, 0.2f, 1.f};
    const std::array<float, 4> blue   = {0.2f, 0.2f, 1.f, 1.f};
    const std::array<float, 4> yellow = {1.f, 1.f, 0.2f, 1.f};
    const std::array<float, 4> cyan   = {0.2f, 1.f, 1.f, 1.f};
    const std::array<float, 4> purple = {1.f, 0.2f, 1.f, 1.f};

    auto v = [](float x, float y, float z,
                const std::array<float, 4> &c) {
        return CpuVertex{x, y, z, c[0], c[1], c[2], c[3]};
    };

    return {{
        // +Z face (red)
        v(-s, -s,  s, red), v( s, -s,  s, red), v( s,  s,  s, red),
        v(-s, -s,  s, red), v( s,  s,  s, red), v(-s,  s,  s, red),
        // -Z face (green)
        v( s, -s, -s, green), v(-s, -s, -s, green), v(-s,  s, -s, green),
        v( s, -s, -s, green), v(-s,  s, -s, green), v( s,  s, -s, green),
        // +X face (blue)
        v( s, -s,  s, blue), v( s, -s, -s, blue), v( s,  s, -s, blue),
        v( s, -s,  s, blue), v( s,  s, -s, blue), v( s,  s,  s, blue),
        // -X face (yellow)
        v(-s, -s, -s, yellow), v(-s, -s,  s, yellow), v(-s,  s,  s, yellow),
        v(-s, -s, -s, yellow), v(-s,  s,  s, yellow), v(-s,  s, -s, yellow),
        // +Y face (cyan)
        v(-s,  s,  s, cyan), v( s,  s,  s, cyan), v( s,  s, -s, cyan),
        v(-s,  s,  s, cyan), v( s,  s, -s, cyan), v(-s,  s, -s, cyan),
        // -Y face (purple)
        v(-s, -s, -s, purple), v( s, -s, -s, purple), v( s, -s,  s, purple),
        v(-s, -s, -s, purple), v( s, -s,  s, purple), v(-s, -s,  s, purple),
    }};
}

} // namespace

class BasicGame : public Kreate::App {
    std::shared_ptr<Kreate::Scene> scene;
    std::shared_ptr<Kreate::Object> cube;
    std::chrono::steady_clock::time_point startTime;

public:
    BasicGame() : App({{.title = "KREATE - BasicGame", .width = 1280, .height = 720}}) {}

    void onInit() override {
        Kreate::PipelineDesc pdesc{};
        // CullMode::None for Phase 1 — depth buffering arrives in
        // Engine-Roadmap Phase 7. Without a depth attachment, back-face
        // culling alone is the only thing keeping the cube from looking
        // inside-out; defaulting to None makes Phase 1 robust to either
        // winding convention. The triangles overdraw each other every
        // frame, but the spin still reads clearly.
        pdesc.cullMode         = Kreate::CullMode::None;
        pdesc.fillMode         = Kreate::FillMode::Solid;
        pdesc.enableDepth      = false;
        pdesc.vertexFunction   = "vertexFunc";
        pdesc.fragmentFunction = "fragFunc";
        auto pipeline = createPipeline("Phase1Basic.omegasl", pdesc);

        Kreate::MeshDesc mdesc{};
        mdesc.attributes  = Kreate::VertexAttribute::Position | Kreate::VertexAttribute::Color;
        mdesc.topology    = Kreate::MeshTopology::Triangle;
        mdesc.indexFormat = Kreate::IndexFormat::None;

        auto verts = makeCubeVertices();
        auto mesh  = createMesh(mdesc,
                                verts.data(),
                                verts.size() * sizeof(CpuVertex),
                                static_cast<unsigned>(verts.size()));

        cube = Kreate::Object::create(pipeline, mesh);
        cube->setName("Cube");

        scene = Kreate::Scene::create();
        scene->setClearColor({0.1f, 0.1f, 0.15f, 1.f});
        scene->setProjectionMatrix(
            Kreate::Mat4::perspective(60.f * kPi / 180.f, 1280.f / 720.f, 0.1f, 100.f));
        scene->setViewMatrix(
            Kreate::Mat4::lookAt({0, 2, 5}, {0, 0, 0}, {0, 1, 0}));
        scene->add(cube);

        startTime = std::chrono::steady_clock::now();
    }

    void onFrame() override {
        const auto now = std::chrono::steady_clock::now();
        const float t = std::chrono::duration<float>(now - startTime).count();

        // Spin around the Y axis at ~60 degrees per second, plus a
        // slower wobble on X so the cube reads as a 3D object.
        Kreate::Mat4 rotY = Kreate::Mat4::rotation(t * (kPi / 3.f), {0, 1, 0});
        Kreate::Mat4 rotX = Kreate::Mat4::rotation(t * (kPi / 7.f), {1, 0, 0});
        cube->setTransform(rotY * rotX);

        scene->render(*this);
    }
};

std::unique_ptr<Kreate::App> Kreate::CreateApp() {
    return std::make_unique<BasicGame>();
}
