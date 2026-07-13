#include "ShaderArchive.h"

#include <cstring>
#include <istream>
#include <ostream>
#include <string>

namespace omegasl {

namespace {

/// Defensive caps for parsing untrusted archive files — mirror the historical
/// limits in `GE.cpp` so a malformed or hostile file fails loudly instead of
/// allocating wildly off an attacker-controlled length.
constexpr std::size_t kMaxLibraryNameBytes    = 4096u;
constexpr unsigned    kMaxEntryCount          = 4096u;
constexpr std::size_t kMaxShaderNameBytes     = 4096u;
constexpr std::size_t kMaxShaderBytecodeBytes = 256u * 1024u * 1024u;
constexpr unsigned    kMaxLayoutCount         = 1024u;
constexpr unsigned    kMaxVertexParamCount    = 256u;

template <typename T>
bool readValue(std::istream &in, T &value) {
    in.read(reinterpret_cast<char *>(&value), sizeof(T));
    return static_cast<bool>(in);
}

bool readBytes(std::istream &in, void *data, std::size_t size) {
    if (size == 0) {
        return true;
    }
    in.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

template <typename T>
void writeValue(std::ostream &out, const T &value) {
    out.write(reinterpret_cast<const char *>(&value), sizeof(T));
}

} // namespace

// --- owning-storage helpers ---------------------------------------------------

char *OmegaSLShaderArchive::internName(const char *src, std::size_t len) {
    auto buf = std::make_unique<char[]>(len + 1);
    if (len > 0 && src != nullptr) {
        std::memcpy(buf.get(), src, len);
    }
    buf[len] = '\0';
    char *ptr = buf.get();
    nameStore_.push_back(std::move(buf));
    return ptr;
}

unsigned char *OmegaSLShaderArchive::internData(const void *src, std::size_t len) {
    if (len == 0) {
        return nullptr;
    }
    auto buf = std::make_unique<unsigned char[]>(len);
    if (src != nullptr) {
        std::memcpy(buf.get(), src, len);
    }
    unsigned char *ptr = buf.get();
    dataStore_.push_back(std::move(buf));
    return ptr;
}

omegasl_shader_layout_desc *OmegaSLShaderArchive::allocLayout(std::size_t count) {
    if (count == 0) {
        return nullptr;
    }
    auto buf = std::make_unique<omegasl_shader_layout_desc[]>(count); // value-inits to zero
    omegasl_shader_layout_desc *ptr = buf.get();
    layoutStore_.push_back(std::move(buf));
    return ptr;
}

omegasl_vertex_shader_param_desc *OmegaSLShaderArchive::allocParams(std::size_t count) {
    if (count == 0) {
        return nullptr;
    }
    auto buf = std::make_unique<omegasl_vertex_shader_param_desc[]>(count); // value-inits to zero
    omegasl_vertex_shader_param_desc *ptr = buf.get();
    paramStore_.push_back(std::move(buf));
    return ptr;
}

// --- read ---------------------------------------------------------------------

bool ReadShaderArchive(std::istream &in, OmegaSLShaderArchive &out, std::string &err) {
    err.clear();
    if (!in.good()) {
        err = "input stream is not readable";
        return false;
    }

    /// Container prefix (Phase 0): magic, format version, backend id, reserved.
    char magic[OMEGASLLIB_MAGIC_LEN] = {};
    if (!readBytes(in, magic, OMEGASLLIB_MAGIC_LEN)) {
        err = "could not read container magic";
        return false;
    }
    if (std::memcmp(magic, OMEGASLLIB_MAGIC, OMEGASLLIB_MAGIC_LEN) != 0) {
        err = "not an OmegaSL library (bad magic; expected \"" OMEGASLLIB_MAGIC "\")";
        return false;
    }
    if (!readValue(in, out.formatVersion)) {
        err = "could not read container format version";
        return false;
    }
    if (out.formatVersion != OMEGASLLIB_FORMAT_VERSION) {
        err = "unsupported container format version " + std::to_string(out.formatVersion) +
              " (this build understands version " +
              std::to_string(static_cast<unsigned>(OMEGASLLIB_FORMAT_VERSION)) +
              "); recompile the library with a matching omegaslc";
        return false;
    }
    std::uint8_t reserved[3] = {};
    if (!readValue(in, out.backendId) || !readBytes(in, reserved, sizeof(reserved))) {
        err = "could not read container header (backend id / reserved bytes)";
        return false;
    }

    /// Library name.
    std::size_t nameLen = 0;
    if (!readValue(in, nameLen)) {
        err = "could not read library name length";
        return false;
    }
    if (nameLen > kMaxLibraryNameBytes) {
        err = "library name length " + std::to_string(nameLen) + " exceeds supported limit";
        return false;
    }
    out.name.resize(nameLen);
    if (!readBytes(in, out.name.data(), nameLen)) {
        err = "could not read library name";
        return false;
    }

    /// Entry table.
    unsigned entryCount = 0;
    if (!readValue(in, entryCount)) {
        err = "could not read shader entry count";
        return false;
    }
    if (entryCount > kMaxEntryCount) {
        err = "shader entry count " + std::to_string(entryCount) + " exceeds supported limit";
        return false;
    }

    out.shaders.reserve(entryCount);
    for (unsigned i = 0; i < entryCount; ++i) {
        const std::string at = " for entry " + std::to_string(i);
        omegasl_shader rec{};

        if (!readValue(in, rec.type)) {
            err = "could not read shader type" + at;
            return false;
        }

        std::size_t shaderNameLen = 0;
        if (!readValue(in, shaderNameLen)) {
            err = "could not read shader name length" + at;
            return false;
        }
        if (shaderNameLen == 0 || shaderNameLen > kMaxShaderNameBytes) {
            err = "invalid shader name length " + std::to_string(shaderNameLen) + at;
            return false;
        }
        std::string nameTmp(shaderNameLen, '\0');
        if (!readBytes(in, nameTmp.data(), shaderNameLen)) {
            err = "could not read shader name" + at;
            return false;
        }
        rec.name = out.internName(nameTmp.data(), shaderNameLen);

        std::size_t dataSize = 0;
        if (!readValue(in, dataSize)) {
            err = "could not read shader bytecode size" + at;
            return false;
        }
        if (dataSize > kMaxShaderBytecodeBytes) {
            err = "invalid shader bytecode size " + std::to_string(dataSize) + at;
            return false;
        }
        rec.dataSize = dataSize;
        rec.data = out.internData(nullptr, dataSize); // allocate owned storage (nullptr if 0)
        if (dataSize > 0 && !readBytes(in, rec.data, dataSize)) {
            err = "could not read shader bytecode" + at;
            return false;
        }
        const bool isStub = (dataSize == 0);

        unsigned nLayout = 0;
        if (!readValue(in, nLayout)) {
            err = "could not read resource layout count" + at;
            return false;
        }
        if (nLayout > kMaxLayoutCount) {
            err = "resource layout count " + std::to_string(nLayout) + " exceeds supported limit" + at;
            return false;
        }
        rec.nLayout = nLayout;
        rec.pLayout = out.allocLayout(nLayout);
        for (unsigned l = 0; l < nLayout; ++l) {
            if (!readValue(in, rec.pLayout[l])) {
                err = "could not read layout entry " + std::to_string(l) + at;
                return false;
            }
        }

        /// Stage-specific decoration travels only with non-stub records, in the
        /// exact order the writer emits it.
        if (rec.type == OMEGASL_SHADER_VERTEX && !isStub) {
            if (!readValue(in, rec.vertexShaderInputDesc.useVertexID) ||
                !readValue(in, rec.vertexShaderInputDesc.nParam)) {
                err = "could not read vertex input descriptor" + at;
                return false;
            }
            const unsigned nParam = rec.vertexShaderInputDesc.nParam;
            if (nParam > kMaxVertexParamCount) {
                err = "vertex input parameter count " + std::to_string(nParam) + " exceeds supported limit" + at;
                return false;
            }
            rec.vertexShaderInputDesc.pParams = out.allocParams(nParam);
            for (unsigned p = 0; p < nParam; ++p) {
                auto &param = rec.vertexShaderInputDesc.pParams[p];
                std::size_t paramNameLen = 0;
                if (!readValue(in, paramNameLen)) {
                    err = "could not read vertex parameter name length" + at;
                    return false;
                }
                if (paramNameLen == 0 || paramNameLen > kMaxShaderNameBytes) {
                    err = "invalid vertex parameter name length " + std::to_string(paramNameLen) + at;
                    return false;
                }
                std::string paramNameTmp(paramNameLen, '\0');
                if (!readBytes(in, paramNameTmp.data(), paramNameLen)) {
                    err = "could not read vertex parameter name" + at;
                    return false;
                }
                param.name = out.internName(paramNameTmp.data(), paramNameLen);
                if (!readValue(in, param.type) || !readValue(in, param.offset)) {
                    err = "could not read vertex parameter type/offset" + at;
                    return false;
                }
            }
        } else if (rec.type == OMEGASL_SHADER_COMPUTE && !isStub) {
            if (!readValue(in, rec.threadgroupDesc.x) ||
                !readValue(in, rec.threadgroupDesc.y) ||
                !readValue(in, rec.threadgroupDesc.z)) {
                err = "could not read compute threadgroup size" + at;
                return false;
            }
        } else if (rec.type == OMEGASL_SHADER_MESH && !isStub) {
            if (!readValue(in, rec.threadgroupDesc.x) ||
                !readValue(in, rec.threadgroupDesc.y) ||
                !readValue(in, rec.threadgroupDesc.z) ||
                !readValue(in, rec.meshDesc.max_vertices) ||
                !readValue(in, rec.meshDesc.max_primitives) ||
                !readValue(in, rec.meshDesc.topology) ||
                /// §5 — payload size (0 when the mesh stage declares no
                /// `in payload`). Appended AFTER the pre-existing mesh fields so
                /// the on-disk order of everything before it is unchanged.
                !readValue(in, rec.payloadDesc.size)) {
                err = "could not read mesh descriptor" + at;
                return false;
            }
        } else if (rec.type == OMEGASL_SHADER_AMPLIFICATION && !isStub) {
            /// §5 — an amplification stage dispatches like compute (its
            /// `[numthreads]` rides `threadgroupDesc`, same as compute and mesh)
            /// and always carries a payload. Order mirrors the writer.
            if (!readValue(in, rec.threadgroupDesc.x) ||
                !readValue(in, rec.threadgroupDesc.y) ||
                !readValue(in, rec.threadgroupDesc.z) ||
                !readValue(in, rec.payloadDesc.size)) {
                err = "could not read amplification descriptor" + at;
                return false;
            }
        } else if (rec.type == OMEGASL_SHADER_HULL && !isStub) {
            /// §16 Phase E — a hull lowers to a compute kernel (Metal), so it
            /// carries a threadgroup size (dispatched one thread per patch) in
            /// addition to the tessellation descriptor. Order mirrors the writer.
            if (!readValue(in, rec.threadgroupDesc.x) ||
                !readValue(in, rec.threadgroupDesc.y) ||
                !readValue(in, rec.threadgroupDesc.z) ||
                !readValue(in, rec.tessellationDesc.domain) ||
                !readValue(in, rec.tessellationDesc.partitioning) ||
                !readValue(in, rec.tessellationDesc.output_topology) ||
                !readValue(in, rec.tessellationDesc.output_control_points)) {
                err = "could not read hull descriptor" + at;
                return false;
            }
        } else if (rec.type == OMEGASL_SHADER_DOMAIN && !isStub) {
            /// §16 Phase E — tessellation descriptor (no threadgroup size: the
            /// domain is a post-tessellation vertex stage, not a kernel).
            if (!readValue(in, rec.tessellationDesc.domain) ||
                !readValue(in, rec.tessellationDesc.partitioning) ||
                !readValue(in, rec.tessellationDesc.output_topology) ||
                !readValue(in, rec.tessellationDesc.output_control_points)) {
                err = "could not read domain descriptor" + at;
                return false;
            }
        }

        /// Per-shader required-feature bitfield — always present, even when 0.
        if (!readValue(in, rec.requiredFeatures)) {
            err = "could not read required-feature flags" + at;
            return false;
        }

        out.shaders.push_back(rec);
    }

    return true;
}

// --- write --------------------------------------------------------------------

bool WriteShaderArchive(std::ostream &out, const OmegaSLShaderArchive &lib, std::string &err) {
    err.clear();

    /// Container prefix (Phase 0).
    out.write(OMEGASLLIB_MAGIC, OMEGASLLIB_MAGIC_LEN);
    const std::uint32_t version = lib.formatVersion;
    writeValue(out, version);
    const std::uint8_t backendId = lib.backendId;
    writeValue(out, backendId);
    const std::uint8_t reserved[3] = {0, 0, 0};
    out.write(reinterpret_cast<const char *>(reserved), sizeof(reserved));

    /// Library name.
    const std::size_t libnameSize = lib.name.size();
    writeValue(out, libnameSize);
    out.write(lib.name.data(), static_cast<std::streamsize>(lib.name.size()));

    /// Entry table.
    const unsigned entryCount = static_cast<unsigned>(lib.shaders.size());
    writeValue(out, entryCount);

    for (const auto &shader : lib.shaders) {
        /// A record with no bytecode is a header-only stub: the backend could
        /// not express one of its `#requires(...)` features. Stubs carry the
        /// layout block but no bytecode and no stage decoration.
        const bool isStub = (shader.dataSize == 0);

        writeValue(out, shader.type);

        const std::size_t nameLen = shader.name != nullptr ? std::strlen(shader.name) : 0;
        writeValue(out, nameLen);
        out.write(shader.name, static_cast<std::streamsize>(nameLen));

        if (isStub) {
            const std::size_t zero = 0;
            writeValue(out, zero);
        } else {
            const std::size_t dataSize = shader.dataSize;
            writeValue(out, dataSize);
            out.write(reinterpret_cast<const char *>(shader.data), static_cast<std::streamsize>(dataSize));
        }

        if (shader.nLayout > 0) {
            writeValue(out, shader.nLayout);
            for (unsigned l = 0; l < shader.nLayout; ++l) {
                const auto &layout = shader.pLayout[l];
                /// Zero-fill the on-disk copy so struct/union padding is
                /// deterministic; copy only the populated fields (constant_desc
                /// and swizzle_desc stay zero, exactly as the original writer).
                omegasl_shader_layout_desc serialized;
                std::memset(&serialized, 0, sizeof(serialized));
                serialized.type = layout.type;
                serialized.gpu_relative_loc = layout.gpu_relative_loc;
                serialized.io_mode = layout.io_mode;
                serialized.location = layout.location;
                serialized.offset = layout.offset;
                serialized.sampler_desc.filter = layout.sampler_desc.filter;
                serialized.sampler_desc.u_address_mode = layout.sampler_desc.u_address_mode;
                serialized.sampler_desc.v_address_mode = layout.sampler_desc.v_address_mode;
                serialized.sampler_desc.w_address_mode = layout.sampler_desc.w_address_mode;
                serialized.sampler_desc.max_anisotropy = layout.sampler_desc.max_anisotropy;
                writeValue(out, serialized);
            }
        } else {
            const unsigned len = 0;
            writeValue(out, len);
        }

        if (!isStub) {
            if (shader.type == OMEGASL_SHADER_VERTEX) {
                writeValue(out, shader.vertexShaderInputDesc.useVertexID);
                writeValue(out, shader.vertexShaderInputDesc.nParam);
                for (unsigned p = 0; p < shader.vertexShaderInputDesc.nParam; ++p) {
                    const auto &param = shader.vertexShaderInputDesc.pParams[p];
                    const std::size_t paramNameLen = param.name != nullptr ? std::strlen(param.name) : 0;
                    writeValue(out, paramNameLen);
                    out.write(param.name, static_cast<std::streamsize>(paramNameLen));
                    writeValue(out, param.type);
                    writeValue(out, param.offset);
                }
            } else if (shader.type == OMEGASL_SHADER_COMPUTE) {
                writeValue(out, shader.threadgroupDesc.x);
                writeValue(out, shader.threadgroupDesc.y);
                writeValue(out, shader.threadgroupDesc.z);
            } else if (shader.type == OMEGASL_SHADER_MESH) {
                writeValue(out, shader.threadgroupDesc.x);
                writeValue(out, shader.threadgroupDesc.y);
                writeValue(out, shader.threadgroupDesc.z);
                writeValue(out, shader.meshDesc.max_vertices);
                writeValue(out, shader.meshDesc.max_primitives);
                writeValue(out, shader.meshDesc.topology);
                /// §5 — payload size (0 when the mesh stage declares no
                /// `in payload`). Appended after the pre-existing mesh fields.
                writeValue(out, shader.payloadDesc.size);
            } else if (shader.type == OMEGASL_SHADER_AMPLIFICATION) {
                /// §5 — threadgroup size + payload size (reader mirrors this order).
                writeValue(out, shader.threadgroupDesc.x);
                writeValue(out, shader.threadgroupDesc.y);
                writeValue(out, shader.threadgroupDesc.z);
                writeValue(out, shader.payloadDesc.size);
            } else if (shader.type == OMEGASL_SHADER_HULL) {
                /// §16 Phase E — hull threadgroup size (dispatched one thread per
                /// patch) + tessellation descriptor (reader mirrors this order).
                writeValue(out, shader.threadgroupDesc.x);
                writeValue(out, shader.threadgroupDesc.y);
                writeValue(out, shader.threadgroupDesc.z);
                writeValue(out, shader.tessellationDesc.domain);
                writeValue(out, shader.tessellationDesc.partitioning);
                writeValue(out, shader.tessellationDesc.output_topology);
                writeValue(out, shader.tessellationDesc.output_control_points);
            } else if (shader.type == OMEGASL_SHADER_DOMAIN) {
                /// §16 Phase E — tessellation descriptor only (reader mirrors this).
                writeValue(out, shader.tessellationDesc.domain);
                writeValue(out, shader.tessellationDesc.partitioning);
                writeValue(out, shader.tessellationDesc.output_topology);
                writeValue(out, shader.tessellationDesc.output_control_points);
            }
        }

        writeValue(out, shader.requiredFeatures);
    }

    if (!out) {
        err = "stream write failure";
        return false;
    }
    return true;
}

} // namespace omegasl
