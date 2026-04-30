#include "CodeGen.h"
#include <iomanip>
#include <sstream>
#include <string>

#define TEXTURE1D "Texture1D"
#define RW_TEXTURE1D "RWTexture1D"
#define TEXTURE2D "Texture2D"
#define RW_TEXTURE2D "RWTexture2D"
#define BUFFER "StructuredBuffer"
#define RW_BUFFER "RWStructuredBuffer"
#define SAMPLER1D "SamplerState"
#define SAMPLER2D "SamplerState"
#define SAMPLER3D "SamplerState"

namespace omegasl {

    /// Print a float literal so it cannot be misread as an integer literal.
    /// Why: ostream << float drops a trailing `.0`, so a source `0.0` becomes
    /// `0` in the generated shader. HLSL has integer overloads for
    /// `max`/`min`/`clamp`/etc. — an integer-shaped argument can pick the
    /// int overload silently or produce ambiguity warnings under stricter
    /// FXC/DXC settings.
    static std::string formatFloatLit(double v) {
        std::ostringstream os;
        os << std::setprecision(9) << v;
        std::string s = os.str();
        if (s.find_first_of(".eEnN") == std::string::npos) {
            s += ".0";
        }
        return s;
    }

class HLSLCodeGen final : public CodeGen {
    std::ostream &shaderOut;
    std::ofstream fileOut;
    std::ostringstream stringOut;
    HLSLCodeOpts &hlslCodeOpts;

    OmegaCommon::Map<OmegaCommon::String, OmegaCommon::String> generatedStructs;

    void emitUserFunctionSignature(ast::FuncDecl *f) {
        writeTypeExpr(f->returnType, shaderOut);
        shaderOut << " " << spellUserFuncName(f->name) << "(";
        for (size_t i = 0; i < f->params.size(); i++) {
            if (i > 0)
                shaderOut << ", ";
            writeTypeExpr(f->params[i].typeExpr, shaderOut);
            shaderOut << " " << f->params[i].name;
        }
        shaderOut << ")";
    }

    void emitUserFunctionPrototype(ast::FuncDecl *f) {
        emitUserFunctionSignature(f);
        shaderOut << ";" << std::endl;
    }

    void emitUserFunction(ast::FuncDecl *f) {
        emitUserFunctionSignature(f);
        shaderOut << std::endl;
        generateBlock(*f->block);
        shaderOut << std::endl;
    }

public:
    explicit HLSLCodeGen(CodeGenOpts &opts, HLSLCodeOpts &hlslCodeOpts)
        : CodeGen(opts, std::make_unique<HLSLTarget>(hlslCodeOpts)), shaderOut(fileOut), hlslCodeOpts(hlslCodeOpts) {}
    explicit HLSLCodeGen(CodeGenOpts &opts, HLSLCodeOpts &hlslCodeOpts, std::ostringstream &out)
        : CodeGen(opts, std::make_unique<HLSLTarget>(hlslCodeOpts)), shaderOut(stringOut), stringOut(std::move(out)), hlslCodeOpts(hlslCodeOpts) {}
    ~HLSLCodeGen() override = default;

    std::ostream &shaderOutStream() override { return shaderOut; }

    inline void writeTypeExpr(ast::TypeExpr *typeExpr, std::ostream &out) {
        target->writeTypeName(typeResolver->resolveTypeWithExpr(typeExpr), typeExpr->pointer, out);
    }
    void generateDecl(ast::Decl *decl) override {
        switch (decl->type) {
            case VAR_DECL: {
                auto _decl = (ast::VarDecl *)decl;
                writeTypeExpr(_decl->typeExpr, shaderOut);
                shaderOut << " " << _decl->spec.name;
                if (_decl->typeExpr->arraySize.has_value()) {
                    shaderOut << "[" << _decl->typeExpr->arraySize.value() << "]";
                }
                if (_decl->spec.initializer.has_value()) {
                    shaderOut << " = ";
                    generateExpr(_decl->spec.initializer.value());
                }
                break;
            }
            case RETURN_DECL: {
                auto _decl = (ast::ReturnDecl *)decl;
                if (_decl->expr) {
                    shaderOut << "return ";
                    generateExpr(_decl->expr);
                } else {
                    shaderOut << "return";
                }
                break;
            }
            case IF_STMT: {
                auto _stmt = (ast::IfStmt *)decl;
                shaderOut << "if(";
                generateExpr(_stmt->condition);
                shaderOut << ")";
                generateBlock(*_stmt->thenBlock);
                for (auto &branch : _stmt->elseIfs) {
                    shaderOut << " else if(";
                    generateExpr(branch.condition);
                    shaderOut << ")";
                    generateBlock(*branch.block);
                }
                if (_stmt->elseBlock) {
                    shaderOut << " else ";
                    generateBlock(*_stmt->elseBlock);
                }
                break;
            }
            case FOR_STMT: {
                auto _stmt = (ast::ForStmt *)decl;
                shaderOut << "for(";
                if (_stmt->init) {
                    generateDecl((ast::Decl *)_stmt->init);
                }
                shaderOut << ";";
                if (_stmt->condition) {
                    generateExpr(_stmt->condition);
                }
                shaderOut << ";";
                if (_stmt->increment) {
                    generateExpr(_stmt->increment);
                }
                shaderOut << ")";
                generateBlock(*_stmt->body);
                break;
            }
            case WHILE_STMT: {
                auto _stmt = (ast::WhileStmt *)decl;
                shaderOut << "while(";
                generateExpr(_stmt->condition);
                shaderOut << ")";
                generateBlock(*_stmt->body);
                break;
            }
            case BREAK_STMT: {
                shaderOut << "break";
                break;
            }
            case CONTINUE_STMT: {
                shaderOut << "continue";
                break;
            }
            case DISCARD_STMT: {
                auto kw = target->discardStatement();
                shaderOut << kw;
                break;
            }
            case STRUCT_DECL: {
                auto _decl = (ast::StructDecl *)decl;
                std::ostringstream out;
                out << "struct " << _decl->name << "{" << std::endl;
                for (auto &f : _decl->fields) {
                    out << "    " << std::flush;
                    writeTypeExpr(f.typeExpr, out);
                    out << " " << f.name;
                    if (f.attributeName.has_value()) {
                        out << ":";
                        target->writeAttribute(f.attributeName.value(), f.attributeIndex, out);
                    }
                    out << ";" << std::endl;
                }
                out << "};" << std::endl;

                generatedStructs.insert(std::make_pair(_decl->name, out.str()));

                break;
            }
            case RESOURCE_DECL: {
                resourceStore.add((ast::ResourceDecl *)decl);
                break;
            }
            case FUNC_DECL: {
                auto *_fd = (ast::FuncDecl *)decl;
                userFuncDecls.push_back(_fd);
                userFuncNames.insert(std::string(_fd->name));
                break;
            }
            case SHADER_DECL: {
                auto _decl = (ast::ShaderDecl *)decl;
                if (opts.runtimeCompile) {
                    stringOut.str("");
                } else {
                    fileOut.open(OmegaCommon::FS::Path(opts.tempDir).append(_decl->name).concat(target->shaderFileExt(_decl->shaderType)).str());
                }

                /// Emit prototypes for all user functions first so that
                /// forward declarations and call-before-definition work across
                /// function bodies. Dedupe by name.
                {
                    OmegaCommon::Map<OmegaCommon::String, int> emittedProtos;
                    for (auto *uf : userFuncDecls) {
                        if (emittedProtos.find(uf->name) != emittedProtos.end())
                            continue;
                        emittedProtos.insert(std::make_pair(uf->name, 0));
                        emitUserFunctionPrototype(uf);
                    }
                }
                for (auto *uf : userFuncDecls) {
                    if (uf->isForwardDecl)
                        continue;
                    emitUserFunction(uf);
                }

                omegasl_shader shaderDesc{};
                /// 1. Write Structs for Shader
                OmegaCommon::Vector<OmegaCommon::String> struct_names;
                typeResolver->getStructsInFuncDecl(_decl, struct_names);

                for (auto &s : struct_names) {
                    shaderOut << generatedStructs[s] << std::endl;
                }

                shaderDesc.type = _decl->shaderType == ast::ShaderDecl::Vertex     ? OMEGASL_SHADER_VERTEX
                                  : _decl->shaderType == ast::ShaderDecl::Fragment ? OMEGASL_SHADER_FRAGMENT
                                  : _decl->shaderType == ast::ShaderDecl::Compute  ? OMEGASL_SHADER_COMPUTE
                                  : _decl->shaderType == ast::ShaderDecl::Hull     ? OMEGASL_SHADER_HULL
                                                                                   : OMEGASL_SHADER_DOMAIN;

                //                    std::cout << "Shader Name:" << _decl->name << std::endl;

                shaderDesc.name = new char[_decl->name.size() + 1];
                std::copy(_decl->name.begin(), _decl->name.end(), (char *)shaderDesc.name);
                ((char *)shaderDesc.name)[_decl->name.size()] = '\0';

                OmegaCommon::Vector<omegasl_shader_layout_desc> shaderLayout;

                /// 2. Write Resources for Shader (delegated to Target)
                target->resetForNextShader();
                for (auto &res : _decl->resourceMap) {
                    auto res_desc = *(resourceStore.find(res.name));
                    omegasl_shader_layout_desc layoutDesc{};
                    omegasl_shader_layout_desc_io_mode ioMode =
                        res.access == ast::ShaderDecl::ResourceMapDesc::In    ? OMEGASL_SHADER_DESC_IO_IN
                        : res.access == ast::ShaderDecl::ResourceMapDesc::Inout ? OMEGASL_SHADER_DESC_IO_INOUT
                                                                                : OMEGASL_SHADER_DESC_IO_OUT;
                    target->emitResourceBinding(*this, res_desc, _decl, ioMode, shaderOut, layoutDesc);
                    shaderLayout.push_back(layoutDesc);
                }
                target->emitStaticPreamble(shaderOut);

                shaderDesc.nLayout = shaderLayout.size();
                shaderDesc.pLayout = new omegasl_shader_layout_desc[shaderLayout.size()];
                std::copy(shaderLayout.begin(), shaderLayout.end(), shaderDesc.pLayout);

                /// 3. Function entry signature (delegated to Target);
                /// then body via the shared generateBlock.
                target->emitShaderEntryHeader(*this, _decl, shaderDesc, shaderOut);
                generateBlock(*_decl->block);

                OmegaCommon::String object_file;
                if (!opts.runtimeCompile) {
                    object_file = OmegaCommon::FS::Path(opts.tempDir).append(_decl->name).concat(".cso").absPath();
                } else {
                    object_file = _decl->name;
                }
                if (!opts.runtimeCompile) {
                    fileOut.close();
                }

                shaderMap.insert(std::make_pair(object_file, shaderDesc));
                break;
            }
        }
    }
    bool compileShader(ast::ShaderDecl::Type type, const OmegaCommon::StrRef &name, const OmegaCommon::FS::Path &path,
                       const OmegaCommon::FS::Path &outputPath) override {
        return target->compileShader(type, name, path, outputPath);
    }

    void compileShaderOnRuntime(ast::ShaderDecl::Type type, const OmegaCommon::StrRef &name) override {
        auto &shaderEntry = shaderMap[name.data()];
        target->compileShaderRuntime(type, name, stringOut.str(), shaderEntry);
    }
};

std::shared_ptr<CodeGen> HLSLCodeGenMake(CodeGenOpts &opts, HLSLCodeOpts &hlslCodeOpts) {
    return std::make_shared<HLSLCodeGen>(opts, hlslCodeOpts);
}

std::shared_ptr<CodeGen> HLSLCodeGenMakeRuntime(CodeGenOpts &opts, HLSLCodeOpts &hlslCodeOpts,
                                                std::ostringstream &out) {
    return std::make_shared<HLSLCodeGen>(opts, hlslCodeOpts, out);
}

} // namespace omegasl
