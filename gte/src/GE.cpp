#include "omegaGTE/GE.h"
#include "omegaGTE/GTEShader.h"


#ifdef TARGET_DIRECTX
#include "d3d12/GED3D12.h"
#endif



#ifdef TARGET_METAL
#include "metal/GEMetal.h"
#endif



#ifdef TARGET_VULKAN
#include "vulkan/GEVulkan.h"
#endif

#include <cassert>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

_NAMESPACE_BEGIN_


typedef unsigned char ShaderByte;

namespace {
    constexpr std::size_t kMaxShaderLibraryNameBytes = 4096u;
    constexpr unsigned kMaxShaderEntryCount = 4096u;
    constexpr std::size_t kMaxShaderNameBytes = 4096u;
    constexpr std::size_t kMaxShaderBytecodeBytes = 256u * 1024u * 1024u;
    constexpr unsigned kMaxShaderLayoutCount = 1024u;
    constexpr unsigned kMaxVertexShaderParamCount = 256u;

    template <typename T>
    bool readBinaryValue(std::istream &in,T &value) {
        in.read(reinterpret_cast<char *>(&value),sizeof(T));
        return static_cast<bool>(in);
    }

    bool readBinaryBytes(std::istream &in,void *data,std::size_t size) {
        if(size == 0){
            return true;
        }
        in.read(static_cast<char *>(data),static_cast<std::streamsize>(size));
        return static_cast<bool>(in);
    }

    bool readBinaryString(std::istream &in,std::string &out,std::size_t size) {
        out.resize(size);
        return readBinaryBytes(in,out.data(),out.size());
    }
}

GEBuffer::GEBuffer(const BufferDescriptor::Usage &usage):usage(usage) {

}

bool GEBuffer::checkCanRead() {
    assert(usage == BufferDescriptor::Readback && "Can only read from a `Readback` type of GEBuffer");
    return true;
}

bool GEBuffer::checkCanWrite() {
    assert(usage == BufferDescriptor::Upload && "Can only write to an `Upload` type of GEBuffer");
    return true;
}

SharedHandle<GTEShaderLibrary> OmegaGraphicsEngine::loadShaderLibraryFromInputStream(std::istream &in) {
    if(!in.good()){
        std::cerr << "OmegaSL shader library load failed: input stream is not readable." << std::endl;
        return nullptr;
    }

    auto lib = std::make_shared<GTEShaderLibrary>();

    std::string libName;
    StrRef::size_type size = 0;
    if(!readBinaryValue(in,size)){
        std::cerr << "OmegaSL shader library load failed: could not read library name length." << std::endl;
        return nullptr;
    }
    if(size > kMaxShaderLibraryNameBytes){
        std::cerr << "OmegaSL shader library load failed: library name length " << size
                  << " exceeds supported limit." << std::endl;
        return nullptr;
    }
    if(!readBinaryString(in,libName,size)){
        std::cerr << "OmegaSL shader library load failed: could not read library name." << std::endl;
        return nullptr;
    }
    (void)libName;

    unsigned entryCount = 0;
    if(!readBinaryValue(in,entryCount)){
        std::cerr << "OmegaSL shader library load failed: could not read shader entry count." << std::endl;
        return nullptr;
    }
    if(entryCount > kMaxShaderEntryCount){
        std::cerr << "OmegaSL shader library load failed: shader entry count " << entryCount
                  << " exceeds supported limit." << std::endl;
        return nullptr;
    }

    for(unsigned entryIndex = 0; entryIndex < entryCount; ++entryIndex){
        /// 1. Read Shader Type.
        omegasl_shader shaderEntry {};
        if(!readBinaryValue(in,shaderEntry.type)){
            std::cerr << "OmegaSL shader library load failed: could not read shader type for entry "
                      << entryIndex << "." << std::endl;
            return nullptr;
        }

        /// 2. Read Shader Name Length and Data
        std::size_t name_len = 0;
        if(!readBinaryValue(in,name_len)){
            std::cerr << "OmegaSL shader library load failed: could not read shader name length for entry "
                      << entryIndex << "." << std::endl;
            return nullptr;
        }
        if(name_len == 0 || name_len > kMaxShaderNameBytes){
            std::cerr << "OmegaSL shader library load failed: invalid shader name length " << name_len
                      << " for entry " << entryIndex << "." << std::endl;
            return nullptr;
        }

        auto shaderName = std::make_unique<char[]>(name_len + 1);
        if(!readBinaryBytes(in,shaderName.get(),name_len)){
            std::cerr << "OmegaSL shader library load failed: could not read shader name for entry "
                      << entryIndex << "." << std::endl;
            return nullptr;
        }
        shaderName[name_len] = '\0';
        shaderEntry.name = shaderName.get();

        /// 3. Read Shader GPU Code Length and Data
        if(!readBinaryValue(in,shaderEntry.dataSize)){
            std::cerr << "OmegaSL shader library load failed: could not read shader bytecode size for `"
                      << shaderEntry.name << "`." << std::endl;
            return nullptr;
        }
        if(shaderEntry.dataSize == 0 || shaderEntry.dataSize > kMaxShaderBytecodeBytes){
            std::cerr << "OmegaSL shader library load failed: invalid shader bytecode size "
                      << shaderEntry.dataSize << " for `" << shaderEntry.name << "`." << std::endl;
            return nullptr;
        }

        auto shaderData = std::make_unique<ShaderByte[]>(shaderEntry.dataSize);
        if(!readBinaryBytes(in,shaderData.get(),shaderEntry.dataSize)){
            std::cerr << "OmegaSL shader library load failed: could not read shader bytecode for `"
                      << shaderEntry.name << "`." << std::endl;
            return nullptr;
        }
        shaderEntry.data = shaderData.get();

        /// 4. Read Shader Layout
        if(!readBinaryValue(in,shaderEntry.nLayout)){
            std::cerr << "OmegaSL shader library load failed: could not read resource layout count for `"
                      << shaderEntry.name << "`." << std::endl;
            return nullptr;
        }

        auto layout_count = shaderEntry.nLayout;
        if(layout_count > kMaxShaderLayoutCount){
            std::cerr << "OmegaSL shader library load failed: resource layout count " << layout_count
                      << " exceeds supported limit for `" << shaderEntry.name << "`." << std::endl;
            return nullptr;
        }

        std::unique_ptr<omegasl_shader_layout_desc[]> layoutDescArr;
        if(layout_count > 0){
            layoutDescArr = std::make_unique<omegasl_shader_layout_desc[]>(layout_count);
        }

        for(unsigned i = 0;i < layout_count;i++){
            if(!readBinaryValue(in,layoutDescArr[i])){
                std::cerr << "OmegaSL shader library load failed: could not read layout entry " << i
                          << " for `" << shaderEntry.name << "`." << std::endl;
                return nullptr;
            }
        }

        shaderEntry.pLayout = layoutDescArr.get();

        if(shaderEntry.type == OMEGASL_SHADER_VERTEX) {
            /// 5. (For Vertex Shaders) Read Vertex Shader Input Desc
            if(!readBinaryValue(in,shaderEntry.vertexShaderInputDesc.useVertexID)){
                std::cerr << "OmegaSL shader library load failed: could not read vertex input mode for `"
                          << shaderEntry.name << "`." << std::endl;
                return nullptr;
            }
            if(!readBinaryValue(in,shaderEntry.vertexShaderInputDesc.nParam)){
                std::cerr << "OmegaSL shader library load failed: could not read vertex input parameter count for `"
                          << shaderEntry.name << "`." << std::endl;
                return nullptr;
            }
            auto &param_c = shaderEntry.vertexShaderInputDesc.nParam;
            if(param_c > kMaxVertexShaderParamCount){
                std::cerr << "OmegaSL shader library load failed: vertex input parameter count " << param_c
                          << " exceeds supported limit for `" << shaderEntry.name << "`." << std::endl;
                return nullptr;
            }

            std::unique_ptr<omegasl_vertex_shader_param_desc[]> vertexShaderInputParams;
            if(param_c > 0){
                vertexShaderInputParams = std::make_unique<omegasl_vertex_shader_param_desc[]>(param_c);
            }
            std::vector<std::unique_ptr<char[]>> vertexParamNames;
            vertexParamNames.reserve(param_c);

            for (unsigned i = 0; i < param_c; i++) {
                auto &paramDesc = vertexShaderInputParams[i];
                std::size_t param_name_len = 0;
                if(!readBinaryValue(in,param_name_len)){
                    std::cerr << "OmegaSL shader library load failed: could not read vertex parameter name length "
                              << "for parameter " << i << " in `" << shaderEntry.name << "`." << std::endl;
                    return nullptr;
                }
                if(param_name_len == 0 || param_name_len > kMaxShaderNameBytes){
                    std::cerr << "OmegaSL shader library load failed: invalid vertex parameter name length "
                              << param_name_len << " for parameter " << i << " in `" << shaderEntry.name << "`."
                              << std::endl;
                    return nullptr;
                }
                auto paramName = std::make_unique<char[]>(param_name_len + 1);
                if(!readBinaryBytes(in,paramName.get(),param_name_len)){
                    std::cerr << "OmegaSL shader library load failed: could not read vertex parameter name for "
                              << "parameter " << i << " in `" << shaderEntry.name << "`." << std::endl;
                    return nullptr;
                }
                paramName[param_name_len] = '\0';
                paramDesc.name = paramName.get();
                if(!readBinaryValue(in,paramDesc.type)){
                    std::cerr << "OmegaSL shader library load failed: could not read vertex parameter type for "
                              << "parameter " << i << " in `" << shaderEntry.name << "`." << std::endl;
                    return nullptr;
                }
                if(!readBinaryValue(in,paramDesc.offset)){
                    std::cerr << "OmegaSL shader library load failed: could not read vertex parameter offset for "
                              << "parameter " << i << " in `" << shaderEntry.name << "`." << std::endl;
                    return nullptr;
                }
                vertexParamNames.push_back(std::move(paramName));
            }

            shaderEntry.vertexShaderInputDesc.pParams = vertexShaderInputParams.get();

            auto shader = _loadShaderFromDesc(&shaderEntry);
            lib->shaders.insert(std::make_pair(std::string(shaderEntry.name),shader));

            if(shader != nullptr){
                shaderEntry.name = shaderName.release();
                shaderEntry.data = shaderData.release();
                shaderEntry.pLayout = layoutDescArr.release();
                shaderEntry.vertexShaderInputDesc.pParams = vertexShaderInputParams.release();
                for(auto &paramName : vertexParamNames){
                    paramName.release();
                }
            }
            continue;

        }
        else if(shaderEntry.type == OMEGASL_SHADER_COMPUTE){
            if(!readBinaryValue(in,shaderEntry.threadgroupDesc.x) ||
               !readBinaryValue(in,shaderEntry.threadgroupDesc.y) ||
               !readBinaryValue(in,shaderEntry.threadgroupDesc.z)){
                std::cerr << "OmegaSL shader library load failed: could not read compute threadgroup size for `"
                          << shaderEntry.name << "`." << std::endl;
                return nullptr;
            }
        }

        auto shader = _loadShaderFromDesc(&shaderEntry);
        lib->shaders.insert(std::make_pair(std::string(shaderEntry.name),shader));

        if(shader != nullptr){
            shaderEntry.name = shaderName.release();
            shaderEntry.data = shaderData.release();
            shaderEntry.pLayout = layoutDescArr.release();
        }
    }
    return lib;
}

SharedHandle<GTEShaderLibrary> OmegaGraphicsEngine::loadShaderLibrary(FS::Path path) {
    auto absPath = path.absPath();
    if(absPath.empty()){
        std::cerr << "OmegaSL shader library load failed: resolved path is empty." << std::endl;
        return nullptr;
    }
    if(!path.exists()){
        std::cerr << "OmegaSL shader library load failed: file does not exist at `" << absPath << "`." << std::endl;
        return nullptr;
    }

    std::ifstream in(absPath,std::ios::in | std::ios::binary);
    if(!in.is_open()){
        std::cerr << "OmegaSL shader library load failed: could not open `" << absPath << "`." << std::endl;
        return nullptr;
    }

    auto res = loadShaderLibraryFromInputStream(in);
    in.close();
    return res;
}

#ifdef RUNTIME_SHADER_COMP_SUPPORT

SharedHandle<GTEShaderLibrary> OmegaGraphicsEngine::loadShaderLibraryRuntime(std::shared_ptr<omegasl_shader_lib> &lib) {
    OmegaCommon::ArrayRef<omegasl_shader> shaders {lib->shaders,lib->shaders + lib->header.entry_count};
    auto shaderLib = std::make_shared<GTEShaderLibrary>();
    for(auto & s : shaders){
        shaderLib->shaders.insert(std::make_pair(std::string(s.name), _loadShaderFromDesc((omegasl_shader *)&s,true)));
    }
    return shaderLib;
}
#endif

SharedHandle<OmegaGraphicsEngine> OmegaGraphicsEngine::Create(SharedHandle<GTEDevice> & device){
    #ifdef TARGET_METAL
        return CreateMetalEngine(device);
    #endif
    #ifdef TARGET_DIRECTX
        return GED3D12Engine::Create(device);
    #endif
    #ifdef TARGET_VULKAN
        return GEVulkanEngine::Create(device);
    #endif
};

_NAMESPACE_END_
