/// Triangulation-Engine-Completion-Plan Phase 3.1 — backend-independent unit
/// test for `TETriangulationResult::TEMesh::buildIndexed()`. Pure CPU: the
/// dedup pass only touches already-populated `vertexPolygons`, no GPU device
/// or triangulation context needed (mirrors sampler_validation_test.cpp).

#include <omegaGTE/TE.h>
#include <omegaGTE/GTEMath.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace OmegaGTE;

namespace {

using Vertex = TETriangulationResult::TEMesh::Vertex;
/// NB: not `Polygon` — on the D3D12 (TARGET_DIRECTX) build `<windows.h>` /
/// `<wingdi.h>` (pulled in transitively via omegaGTE headers) defines a global
/// `Polygon(...)` GDI function, so an unqualified `Polygon` alias here is
/// ambiguous and fails to compile. Name it `MeshPolygon` to sidestep the clash.
using MeshPolygon = TETriangulationResult::TEMesh::Polygon;
using AttachmentData = TETriangulationResult::AttachmentData;

FVec<3> makeNormal(float x, float y, float z) {
    auto v = FVec<3>::Create();
    v[0][0] = x; v[1][0] = y; v[2][0] = z;
    return v;
}

Vertex makeVertex(float x, float y, float z, const FVec<4> &color, const FVec<3> &normal) {
    Vertex v;
    v.pt = GPoint3D{x, y, z};
    v.attachment = AttachmentData{color, FVec<2>::Create(), FVec<3>::Create(), normal};
    return v;
}

Vertex makePlainVertex(float x, float y, float z) {
    Vertex v;
    v.pt = GPoint3D{x, y, z};
    return v;
}

}  // namespace

int main() {
    const FVec<4> white = makeColor(1.f, 1.f, 1.f, 1.f);
    const FVec<3> up = makeNormal(0.f, 0.f, 1.f);
    const FVec<3> down = makeNormal(0.f, 0.f, -1.f);

    // --- Smooth quad: two triangles sharing an edge, identical color/normal
    //     at every shared corner. Expect 4 unique vertices, 6 indices,
    //     reused vertices reference the same index in first-seen order. ---
    {
        TETriangulationResult::TEMesh mesh;
        Vertex a = makeVertex(0.f, 0.f, 0.f, white, up);
        Vertex b = makeVertex(1.f, 0.f, 0.f, white, up);
        Vertex c = makeVertex(1.f, 1.f, 0.f, white, up);
        Vertex d = makeVertex(0.f, 1.f, 0.f, white, up);

        MeshPolygon tri1; tri1.a = a; tri1.b = b; tri1.c = c;
        MeshPolygon tri2; tri2.a = a; tri2.b = c; tri2.c = d;
        mesh.vertexPolygons = {tri1, tri2};

        mesh.buildIndexed();
        assert(mesh.indexedData.has_value());
        auto &idx = *mesh.indexedData;
        assert(idx.vertices.size() == 4);
        assert(idx.indices.size() == 6);
        std::vector<uint32_t> expected = {0, 1, 2, 0, 2, 3};
        assert(idx.indices == expected);
        assert(idx.vertices[0].pt.x == 0.f && idx.vertices[0].pt.y == 0.f);
        assert(idx.vertices[3].pt.x == 0.f && idx.vertices[3].pt.y == 1.f);
    }

    // --- Hard edge: the same four positions, but each triangle carries its
    //     own flat normal. Corners at shared positions (A, C) must NOT
    //     collapse across triangles — normals differ. Expect 6 vertices,
    //     one per corner, no sharing at all. ---
    {
        TETriangulationResult::TEMesh mesh;
        MeshPolygon tri1;
        tri1.a = makeVertex(0.f, 0.f, 0.f, white, up);
        tri1.b = makeVertex(1.f, 0.f, 0.f, white, up);
        tri1.c = makeVertex(1.f, 1.f, 0.f, white, up);
        MeshPolygon tri2;
        tri2.a = makeVertex(0.f, 0.f, 0.f, white, down);
        tri2.b = makeVertex(1.f, 1.f, 0.f, white, down);
        tri2.c = makeVertex(0.f, 1.f, 0.f, white, down);
        mesh.vertexPolygons = {tri1, tri2};

        mesh.buildIndexed();
        auto &idx = *mesh.indexedData;
        assert(idx.vertices.size() == 6);
        std::vector<uint32_t> expected = {0, 1, 2, 3, 4, 5};
        assert(idx.indices == expected);
    }

    // --- No attachment data at all: dedup falls back to position only. ---
    {
        TETriangulationResult::TEMesh mesh;
        MeshPolygon tri1;
        tri1.a = makePlainVertex(0.f, 0.f, 0.f);
        tri1.b = makePlainVertex(1.f, 0.f, 0.f);
        tri1.c = makePlainVertex(1.f, 1.f, 0.f);
        MeshPolygon tri2;
        tri2.a = makePlainVertex(0.f, 0.f, 0.f);
        tri2.b = makePlainVertex(1.f, 1.f, 0.f);
        tri2.c = makePlainVertex(0.f, 1.f, 0.f);
        mesh.vertexPolygons = {tri1, tri2};

        mesh.buildIndexed();
        auto &idx = *mesh.indexedData;
        assert(idx.vertices.size() == 4);
        std::vector<uint32_t> expected = {0, 1, 2, 0, 2, 3};
        assert(idx.indices == expected);
    }

    // --- Position epsilon: a corner perturbed by far less than the default
    //     1e-6 grid collapses into its neighbor; one perturbed well beyond
    //     it stays distinct. ---
    {
        TETriangulationResult::TEMesh mesh;
        MeshPolygon tri1;
        tri1.a = makePlainVertex(0.f, 0.f, 0.f);
        tri1.b = makePlainVertex(1.f, 0.f, 0.f);
        tri1.c = makePlainVertex(0.5f, 1.f, 0.f);
        MeshPolygon tri2;
        tri2.a = makePlainVertex(0.f + 5e-8f, 0.f, 0.f);   // within epsilon of tri1.a
        tri2.b = makePlainVertex(1.f + 1e-3f, 0.f, 0.f);   // well outside epsilon of tri1.b
        tri2.c = makePlainVertex(0.5f, 1.f, 0.f);           // exact match to tri1.c
        mesh.vertexPolygons = {tri1, tri2};

        mesh.buildIndexed();
        auto &idx = *mesh.indexedData;
        // tri1.a/tri2.a collapse, tri1.c/tri2.c collapse, tri2.b stays distinct.
        assert(idx.vertices.size() == 4);
        std::vector<uint32_t> expected = {0, 1, 2, 0, 3, 2};
        assert(idx.indices == expected);
    }

    // --- Empty mesh: buildIndexed() still populates indexedData (as empty),
    //     it does not leave it unset. ---
    {
        TETriangulationResult::TEMesh mesh;
        mesh.buildIndexed();
        assert(mesh.indexedData.has_value());
        assert(mesh.indexedData->vertices.empty());
        assert(mesh.indexedData->indices.empty());
    }

    std::puts("te_index_buffer_test: OK");
    return 0;
}
