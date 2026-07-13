#include "MeshParser.h"

// Single-header impl. Only this TU defines CGLTF_IMPLEMENTATION.
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

// ufbx ships as ufbx.h + ufbx.c; the implementation TU (ufbx.c) is added to
// the build sources in gte/CMakeLists.txt, so only the header is included here.
#include "ufbx.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

_NAMESPACE_BEGIN_

namespace MeshParser {

namespace {

// ─── helpers ────────────────────────────────────────────────────────

std::string lowerExt(const std::string &path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

std::string parentDir(const std::string &path) {
    auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return "";
    return path.substr(0, slash + 1);
}

void appendVertex(std::vector<float> &out,
                  uint32_t attrs,
                  const float pos[3],
                  const float uv[3],
                  const float n[3],
                  const float c[4]) {
    if (attrs & GEMeshAttrPosition) {
        out.push_back(pos[0]); out.push_back(pos[1]); out.push_back(pos[2]);
    }
    if (attrs & GEMeshAttrUV2) {
        out.push_back(uv[0]); out.push_back(uv[1]);
    }
    if (attrs & GEMeshAttrUV3) {
        out.push_back(uv[0]); out.push_back(uv[1]); out.push_back(uv[2]);
    }
    if (attrs & GEMeshAttrNormal) {
        out.push_back(n[0]); out.push_back(n[1]); out.push_back(n[2]);
    }
    if (attrs & GEMeshAttrColor) {
        out.push_back(c[0]); out.push_back(c[1]); out.push_back(c[2]); out.push_back(c[3]);
    }
}

// ─── glTF (cgltf) ───────────────────────────────────────────────────

const cgltf_attribute *findAttr(const cgltf_primitive *prim,
                                cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < prim->attributes_count; ++i) {
        if (prim->attributes[i].type == type) return &prim->attributes[i];
    }
    return nullptr;
}

bool parseGltf(const std::string &path,
               const GEMeshDescriptor &desc,
               ParsedMesh &out) {
    cgltf_options opt = {};
    cgltf_data *data = nullptr;
    if (cgltf_parse_file(&opt, path.c_str(), &data) != cgltf_result_success) {
        std::cerr << "[MeshParser] cgltf_parse_file failed: " << path << std::endl;
        return false;
    }
    if (cgltf_load_buffers(&opt, data, path.c_str()) != cgltf_result_success) {
        std::cerr << "[MeshParser] cgltf_load_buffers failed: " << path << std::endl;
        cgltf_free(data);
        return false;
    }

    const std::string baseDir = parentDir(path);
    const uint32_t attrs = desc.attributes;
    bool warnedMissing = false;

    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh *mesh = &data->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh->primitives_count; ++pi) {
            const cgltf_primitive *prim = &mesh->primitives[pi];
            if (prim->type != cgltf_primitive_type_triangles) {
                std::cerr << "[MeshParser] skipping non-triangle primitive in " << path << std::endl;
                continue;
            }

            const cgltf_attribute *aPos = findAttr(prim, cgltf_attribute_type_position);
            const cgltf_attribute *aUV  = findAttr(prim, cgltf_attribute_type_texcoord);
            const cgltf_attribute *aN   = findAttr(prim, cgltf_attribute_type_normal);
            const cgltf_attribute *aC   = findAttr(prim, cgltf_attribute_type_color);
            if (aPos == nullptr) continue;

            if (!warnedMissing &&
                (((attrs & (GEMeshAttrUV2 | GEMeshAttrUV3)) && !aUV) ||
                 ((attrs & GEMeshAttrNormal) && !aN) ||
                 ((attrs & GEMeshAttrColor) && !aC))) {
                std::cerr << "[MeshParser] glTF primitive missing some requested "
                             "attributes; zero-fill in effect." << std::endl;
                warnedMissing = true;
            }

            // Resolve base-color texture (first one wins).
            if (out.baseColorTexturePath.empty() && prim->material) {
                const cgltf_texture_view *tv = nullptr;
                if (prim->material->has_pbr_metallic_roughness) {
                    tv = &prim->material->pbr_metallic_roughness.base_color_texture;
                }
                if (tv && tv->texture && tv->texture->image && tv->texture->image->uri) {
                    out.baseColorTexturePath = baseDir + tv->texture->image->uri;
                }
            }

            auto readVertex = [&](cgltf_size idx,
                                  float pos[3], float uv[3], float n[3], float c[4]) {
                pos[0] = pos[1] = pos[2] = 0.f;
                uv[0] = uv[1] = uv[2] = 0.f;
                n[0] = n[1] = n[2] = 0.f;
                c[0] = c[1] = c[2] = c[3] = 1.f;
                if (aPos) cgltf_accessor_read_float(aPos->data, idx, pos, 3);
                if (aUV)  cgltf_accessor_read_float(aUV->data,  idx, uv,  2);
                if (aN)   cgltf_accessor_read_float(aN->data,   idx, n,   3);
                if (aC)   cgltf_accessor_read_float(aC->data,   idx, c,   4);
            };

            const cgltf_size triCount = prim->indices ? prim->indices->count
                                                      : (aPos->data ? aPos->data->count : 0);
            for (cgltf_size i = 0; i < triCount; ++i) {
                cgltf_size vidx = i;
                if (prim->indices) {
                    vidx = cgltf_accessor_read_index(prim->indices, i);
                }
                float pos[3], uv[3], n[3], c[4];
                readVertex(vidx, pos, uv, n, c);
                appendVertex(out.packed, attrs, pos, uv, n, c);
            }
        }
    }

    cgltf_free(data);
    return !out.packed.empty();
}

// ─── FBX (ufbx) ─────────────────────────────────────────────────────

// FBX vertices are emitted in the file's raw coordinate space / units — no
// axis or unit conversion (matching the glTF / OBJ paths), so the default
// `ufbx_load_opts` are used. Faces are triangulated per-face via
// `ufbx_triangulate_face`; node instance transforms are not baked in (the
// glTF path likewise ignores node transforms in v1). Missing requested
// attributes are zero-filled, consistent with the other parsers.
bool parseFbx(const std::string &path,
              const GEMeshDescriptor &desc,
              ParsedMesh &out) {
    ufbx_load_opts opts = {};
    ufbx_error error;
    ufbx_scene *scene = ufbx_load_file(path.c_str(), &opts, &error);
    if (scene == nullptr) {
        char errBuf[512];
        ufbx_format_error(errBuf, sizeof(errBuf), &error);
        std::cerr << "[MeshParser] ufbx_load_file failed: " << path << "\n"
                  << errBuf << std::endl;
        return false;
    }

    const std::string baseDir = parentDir(path);
    const uint32_t attrs = desc.attributes;
    bool warnedMissing = false;

    // Resolve the first base-color texture in the scene. ufbx maps legacy FBX
    // phong materials into the `pbr` view, so `pbr.base_color` covers both
    // modern and classic materials; fall back to `fbx.diffuse_color` just in
    // case. The relative filename is joined to the source dir (mirroring the
    // glTF uri handling); the recorded absolute filename is the fallback.
    for (size_t i = 0; i < scene->materials.count && out.baseColorTexturePath.empty(); ++i) {
        const ufbx_material *mat = scene->materials.data[i];
        const ufbx_texture *tex = mat->pbr.base_color.texture;
        if (tex == nullptr) tex = mat->fbx.diffuse_color.texture;
        if (tex == nullptr) continue;
        if (tex->relative_filename.length > 0) {
            out.baseColorTexturePath =
                baseDir + std::string(tex->relative_filename.data, tex->relative_filename.length);
        } else if (tex->filename.length > 0) {
            out.baseColorTexturePath =
                std::string(tex->filename.data, tex->filename.length);
        }
    }

    for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
        const ufbx_mesh *mesh = scene->meshes.data[mi];

        const bool hasUV = mesh->vertex_uv.exists;
        const bool hasN  = mesh->vertex_normal.exists;
        const bool hasC  = mesh->vertex_color.exists;
        if (!warnedMissing &&
            (((attrs & (GEMeshAttrUV2 | GEMeshAttrUV3)) && !hasUV) ||
             ((attrs & GEMeshAttrNormal) && !hasN) ||
             ((attrs & GEMeshAttrColor) && !hasC))) {
            std::cerr << "[MeshParser] FBX mesh missing some requested "
                         "attributes; zero-fill in effect." << std::endl;
            warnedMissing = true;
        }

        // Scratch buffer for triangulated corner indices: at most
        // `max_face_triangles * 3` per face.
        std::vector<uint32_t> triIndices(mesh->max_face_triangles * 3);

        for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
            const ufbx_face face = mesh->faces.data[fi];
            if (face.num_indices < 3) continue;  // degenerate / point / line
            const uint32_t numTris =
                ufbx_triangulate_face(triIndices.data(), triIndices.size(), mesh, face);

            for (uint32_t ti = 0; ti < numTris * 3; ++ti) {
                const uint32_t index = triIndices[ti];  // corner index
                float pos[3] = {0, 0, 0};
                float uv[3]  = {0, 0, 0};
                float n[3]   = {0, 0, 0};
                float c[4]   = {1, 1, 1, 1};

                const ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
                pos[0] = (float)p.x; pos[1] = (float)p.y; pos[2] = (float)p.z;
                if (hasUV) {
                    const ufbx_vec2 t = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
                    uv[0] = (float)t.x; uv[1] = (float)t.y;
                }
                if (hasN) {
                    const ufbx_vec3 nn = ufbx_get_vertex_vec3(&mesh->vertex_normal, index);
                    n[0] = (float)nn.x; n[1] = (float)nn.y; n[2] = (float)nn.z;
                }
                if (hasC) {
                    const ufbx_vec4 cc = ufbx_get_vertex_vec4(&mesh->vertex_color, index);
                    c[0] = (float)cc.x; c[1] = (float)cc.y; c[2] = (float)cc.z; c[3] = (float)cc.w;
                }
                appendVertex(out.packed, attrs, pos, uv, n, c);
            }
        }
    }

    ufbx_free_scene(scene);
    return !out.packed.empty();
}

// ─── Wavefront OBJ (inline) ─────────────────────────────────────────

// Minimal OBJ parser: handles v / vt / vn / f (triangle and triangulated
// quad fans), mtllib, usemtl, and map_Kd in the .mtl file. Per-face
// material is tracked only well enough to surface the first base-color
// texture; multi-material OBJs collapse into one mesh in v1, matching
// the Metal backend's behavior.
struct OBJState {
    std::vector<float> positions;   // x,y,z triplets
    std::vector<float> texcoords;   // u,v pairs
    std::vector<float> normals;     // x,y,z triplets
    std::unordered_map<std::string, std::string> mtlBaseColor; // name → tex path
    std::string activeMaterial;
};

void parseMtlFile(const std::string &path, OBJState &st) {
    std::ifstream in(path);
    if (!in) return;
    std::string line, current;
    const std::string baseDir = parentDir(path);
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tok; ls >> tok;
        if (tok == "newmtl") {
            ls >> current;
        } else if (tok == "map_Kd" && !current.empty()) {
            std::string rest;
            std::getline(ls, rest);
            // Strip leading whitespace.
            size_t start = rest.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            std::string texPath = rest.substr(start);
            // map_Kd may have options (-blendu, -clamp, etc.); take last token.
            auto sp = texPath.find_last_of(" \t");
            if (sp != std::string::npos) texPath = texPath.substr(sp + 1);
            st.mtlBaseColor[current] = baseDir + texPath;
        }
    }
}

// Parse a single "v/vt/vn" face vertex token. Missing components → 0.
void parseFaceVertex(const std::string &tok, int &vi, int &ti, int &ni) {
    vi = ti = ni = 0;
    int field = 0;
    std::string buf;
    auto flush = [&]() {
        if (buf.empty()) { ++field; return; }
        int v = std::atoi(buf.c_str());
        if (field == 0) vi = v;
        else if (field == 1) ti = v;
        else if (field == 2) ni = v;
        ++field;
        buf.clear();
    };
    for (char c : tok) {
        if (c == '/') flush();
        else buf.push_back(c);
    }
    flush();
}

// 1-based, signed → 0-based positive index. Negative indices count from
// the end of the array (OBJ spec).
int resolveIdx(int signed1, size_t arraySize) {
    if (signed1 > 0) return signed1 - 1;
    if (signed1 < 0) return (int)arraySize + signed1;
    return -1;
}

bool parseObj(const std::string &path,
              const GEMeshDescriptor &desc,
              ParsedMesh &out) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "[MeshParser] cannot open OBJ: " << path << std::endl;
        return false;
    }

    OBJState st;
    const std::string baseDir = parentDir(path);
    const uint32_t attrs = desc.attributes;
    bool warnedMissing = false;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string tok; ls >> tok;
        if (tok == "v") {
            float x, y, z; ls >> x >> y >> z;
            st.positions.push_back(x); st.positions.push_back(y); st.positions.push_back(z);
        } else if (tok == "vt") {
            float u = 0, v = 0; ls >> u >> v;
            st.texcoords.push_back(u); st.texcoords.push_back(v);
        } else if (tok == "vn") {
            float x, y, z; ls >> x >> y >> z;
            st.normals.push_back(x); st.normals.push_back(y); st.normals.push_back(z);
        } else if (tok == "mtllib") {
            std::string mtl; ls >> mtl;
            parseMtlFile(baseDir + mtl, st);
        } else if (tok == "usemtl") {
            ls >> st.activeMaterial;
            if (out.baseColorTexturePath.empty()) {
                auto it = st.mtlBaseColor.find(st.activeMaterial);
                if (it != st.mtlBaseColor.end()) {
                    out.baseColorTexturePath = it->second;
                }
            }
        } else if (tok == "f") {
            // Read all face vertices, then fan-triangulate.
            std::vector<std::string> verts;
            std::string s;
            while (ls >> s) verts.push_back(s);
            if (verts.size() < 3) continue;
            for (size_t i = 1; i + 1 < verts.size(); ++i) {
                std::string face[3] = { verts[0], verts[i], verts[i + 1] };
                for (int k = 0; k < 3; ++k) {
                    int vi, ti, ni;
                    parseFaceVertex(face[k], vi, ti, ni);
                    int p = resolveIdx(vi, st.positions.size() / 3);
                    int t = resolveIdx(ti, st.texcoords.size() / 2);
                    int nn = resolveIdx(ni, st.normals.size() / 3);
                    float pos[3] = {0, 0, 0};
                    float uv[3]  = {0, 0, 0};
                    float nv[3]  = {0, 0, 0};
                    float c[4]   = {1, 1, 1, 1};
                    if (p >= 0 && p * 3 + 2 < (int)st.positions.size()) {
                        pos[0] = st.positions[p * 3 + 0];
                        pos[1] = st.positions[p * 3 + 1];
                        pos[2] = st.positions[p * 3 + 2];
                    }
                    if (t >= 0 && t * 2 + 1 < (int)st.texcoords.size()) {
                        uv[0] = st.texcoords[t * 2 + 0];
                        uv[1] = st.texcoords[t * 2 + 1];
                    } else if ((attrs & (GEMeshAttrUV2 | GEMeshAttrUV3)) && !warnedMissing) {
                        std::cerr << "[MeshParser] OBJ vertex missing texcoord; "
                                     "zero-fill." << std::endl;
                        warnedMissing = true;
                    }
                    if (nn >= 0 && nn * 3 + 2 < (int)st.normals.size()) {
                        nv[0] = st.normals[nn * 3 + 0];
                        nv[1] = st.normals[nn * 3 + 1];
                        nv[2] = st.normals[nn * 3 + 2];
                    } else if ((attrs & GEMeshAttrNormal) && !warnedMissing) {
                        std::cerr << "[MeshParser] OBJ vertex missing normal; "
                                     "zero-fill." << std::endl;
                        warnedMissing = true;
                    }
                    appendVertex(out.packed, attrs, pos, uv, nv, c);
                }
            }
        }
    }
    return !out.packed.empty();
}

}  // namespace

bool parseMesh(const std::string &path,
               const GEMeshDescriptor &desc,
               ParsedMesh &out) {
    if ((desc.attributes & GEMeshAttrPosition) == 0) {
        std::cerr << "[MeshParser] desired descriptor must include Position." << std::endl;
        return false;
    }
    if (desc.indexType != GEMeshIndexType::None) {
        std::cerr << "[MeshParser] indexed output not supported in Phase 3 v1." << std::endl;
        return false;
    }
    out.packed.clear();
    out.baseColorTexturePath.clear();

    // The format parsers below append each vertex's components back to back, so
    // they produce the TIGHT layout. The GPU reads a `buffer<T>` at its
    // backend's own stride (std430 pads a lone float3 to 16B on Vulkan), so the
    // stream is re-laid into that layout once, at the end, before anyone
    // uploads it. Parse in tight units; publish in GPU units.
    const size_t tightStride = geMeshTightStrideFor(desc.attributes);
    out.stride = geMeshStrideFor(desc.attributes);
    if (out.stride == 0 || tightStride == 0) {
        std::cerr << "[MeshParser] empty vertex layout." << std::endl;
        return false;
    }

    const std::string ext = lowerExt(path);
    bool ok = false;
    if (ext == "gltf" || ext == "glb") {
        ok = parseGltf(path, desc, out);
    } else if (ext == "obj") {
        ok = parseObj(path, desc, out);
    } else if (ext == "fbx") {
        ok = parseFbx(path, desc, out);
    } else {
        std::cerr << "[MeshParser] unsupported extension: ." << ext
                  << " (path=" << path << ")" << std::endl;
        return false;
    }

    if (!ok || out.packed.empty()) {
        std::cerr << "[MeshParser] no triangles produced from " << path << std::endl;
        return false;
    }
    // Count in the layout the parsers actually wrote, THEN convert. Counting
    // against out.stride here would divide tight bytes by the padded stride and
    // silently drop a quarter of a Position-only mesh.
    out.vertexCount = (unsigned)((out.packed.size() * sizeof(float)) / tightStride);
    out.packed = geMeshRepackToGPULayout(out.packed, desc.attributes, out.vertexCount);
    return true;
}

}  // namespace MeshParser

_NAMESPACE_END_
