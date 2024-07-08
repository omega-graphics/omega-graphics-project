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

_NAMESPACE_BEGIN_


typedef unsigned char ShaderByte;

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
    auto lib = std::make_shared<GTEShaderLibrary>();

    std::string libName;
    StrRef::size_type size;
    in.read((char *)&size,sizeof(size));
    libName.resize(size);
    in.read(libName.data(),libName.size());
    unsigned entryCount;
    in.read((char *)&entryCount,sizeof(entryCount));
    for(;entryCount > 0;entryCount--){
        /// 1. Read Shader Type.
        omegasl_shader shaderEntry {};
        in.read((char *)&shaderEntry.type,sizeof(shaderEntry.type));
        /// 2. Read Shader Name Length and Data
        size_t name_len;
        in.read((char *)&name_len,sizeof(name_len));
        shaderEntry.name = new char[name_len + 1];
        in.read((char *)shaderEntry.name,name_len);
        ((char *)shaderEntry.name)[name_len] = '\0';

        /// 3. Read Shader GPU Code Length and Data
        in.read((char *)&shaderEntry.dataSize,sizeof(shaderEntry.dataSize));
        shaderEntry.data = new ShaderByte [shaderEntry.dataSize];
        in.read((char *)shaderEntry.data,shaderEntry.dataSize);

        /// 4. Read Shader Layout
        in.read((char *)&shaderEntry.nLayout,sizeof(shaderEntry.nLayout));

        auto layout_count = shaderEntry.nLayout;

        auto layoutDescArr = new omegasl_shader_layout_desc[layout_count];

        for(unsigned i = 0;i < layout_count;i++){
            in.read((char *)(layoutDescArr + i),sizeof(omegasl_shader_layout_desc));
        }

        shaderEntry.pLayout = layoutDescArr;

        if(shaderEntry.type == OMEGASL_SHADER_VERTEX) {
            /// 5. (For Vertex Shaders) Read Vertex Shader Input Desc
            in.read((char *) &shaderEntry.vertexShaderInputDesc.useVertexID,
                    sizeof(shaderEntry.vertexShaderInputDesc.nParam));
            in.read((char *) &shaderEntry.vertexShaderInputDesc.nParam,
                    sizeof(shaderEntry.vertexShaderInputDesc.nParam));
            auto &param_c = shaderEntry.vertexShaderInputDesc.nParam;

            auto vertexShaderInputParams = new omegasl_vertex_shader_param_desc[param_c];

            for (unsigned i = 0; i < param_c; i++) {
                omegasl_vertex_shader_param_desc paramDesc{};
                size_t param_name_len;
                in.read((char *) &param_name_len, sizeof(param_name_len));
                paramDesc.name = new char[param_name_len + 1];
                in.read((char *) paramDesc.name, (std::streamsize)param_name_len);
                ((char *) paramDesc.name)[param_name_len] = '\0';
                in.read((char *) &paramDesc.type, sizeof(paramDesc.type));
                in.read((char *) &paramDesc.offset, sizeof(paramDesc.offset));
                memcpy(vertexShaderInputParams + i, &paramDesc, sizeof(paramDesc));
            }

            shaderEntry.vertexShaderInputDesc.pParams = vertexShaderInputParams;

        }
        else if(shaderEntry.type == OMEGASL_SHADER_COMPUTE){
            in.read((char *)&shaderEntry.threadgroupDesc.x,sizeof(unsigned int));
            in.read((char *)&shaderEntry.threadgroupDesc.y,sizeof(unsigned int));
            in.read((char *)&shaderEntry.threadgroupDesc.z,sizeof(unsigned int));
        }

        lib->shaders.insert(std::make_pair(OmegaCommon::StrRef((char *)shaderEntry.name,(unsigned)name_len), _loadShaderFromDesc(&shaderEntry)));
    }
    return lib;
}

SharedHandle<GTEShaderLibrary> OmegaGraphicsEngine::loadShaderLibrary(FS::Path path) {
    assert(path.exists() && "Path does not exist!");
    std::ifstream in(path.absPath(),std::ios::in | std::ios::binary);
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
