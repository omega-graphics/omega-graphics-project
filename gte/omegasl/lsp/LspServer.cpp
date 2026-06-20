#include "LspServer.h"
#include "Json.h"

#include <omega-common/json.h>

#include <cctype>
#include <cstdlib>
#include <vector>

namespace omegasl {
namespace lsp {

    namespace {

        using OJSON = OmegaCommon::JSON;

        /// --- Incoming-message navigation (over OmegaCommon::JSON) -------------
        /// The accessors on OmegaCommon::JSON are non-const and `operator[]`
        /// would insert default entries, so we navigate via `asMap().find()` and
        /// `const_cast` back to a mutable node — safe because the parsed message
        /// is our own throwaway object.

        OJSON * field(OJSON & obj, const char * key) {
            if (!obj.isMap()) {
                return nullptr;
            }
            auto map = obj.asMap();
            auto it = map.find(OmegaCommon::String(key));
            if (it == map.end()) {
                return nullptr;
            }
            return const_cast<OJSON *>(&it->second);
        }

        std::string getString(OJSON & obj, const char * key, const std::string & dflt = "") {
            OJSON * f = field(obj, key);
            if (f != nullptr && f->isString()) {
                return std::string(f->asString());
            }
            return dflt;
        }

        int getInt(OJSON & obj, const char * key, int dflt = 0) {
            OJSON * f = field(obj, key);
            if (f != nullptr && f->isNumber()) {
                return (int)f->asFloat();
            }
            return dflt;
        }

        /// Echo a request id back verbatim — it may be a number or a string,
        /// and the client matches the response by it.
        Json idToJson(OJSON * idField) {
            if (idField == nullptr) {
                return Json::null();
            }
            if (idField->isNumber()) {
                return Json::integer((long long)idField->asFloat());
            }
            if (idField->isString()) {
                return Json::str(std::string(idField->asString()));
            }
            return Json::null();
        }

        /// --- Outgoing LSP value builders -------------------------------------

        Json makePosition(unsigned line, unsigned character) {
            Json p = Json::object();
            p.set("line", Json::integer(line));
            p.set("character", Json::integer(character));
            return p;
        }

        Json makeRange(const Range & r) {
            Json out = Json::object();
            out.set("start", makePosition(r.startLine, r.startChar));
            out.set("end", makePosition(r.endLine, r.endChar));
            return out;
        }

        /// Map our coarse symbol kind onto an LSP `SymbolKind` integer.
        int lspSymbolKind(SymbolKind k) {
            switch (k) {
                case SymbolKind::Struct:         return 23; // Struct
                case SymbolKind::Function:       return 12; // Function
                case SymbolKind::VertexShader:
                case SymbolKind::FragmentShader:
                case SymbolKind::ComputeShader:
                case SymbolKind::HullShader:
                case SymbolKind::DomainShader:
                case SymbolKind::MeshShader:     return 12; // Function
                case SymbolKind::Buffer:
                case SymbolKind::Texture:
                case SymbolKind::Sampler:        return 13; // Variable
                case SymbolKind::Constant:       return 14; // Constant
            }
            return 13;
        }

        /// LSP `CompletionItemKind`.
        enum : int {
            kCompletionFunction = 3,
            kCompletionField = 5,
            kCompletionVariable = 6,
            kCompletionClass = 7,
            kCompletionStruct = 22,
            kCompletionKeyword = 14,
            kCompletionConstant = 21
        };

        /// --- Static catalogs for hover / completion --------------------------
        /// Presentation data: the language's keyword, type, and intrinsic
        /// vocabulary. Kept here (not derived from the compiler's builtin
        /// tables) because these are editor-facing labels, not compiler facts.

        const std::vector<std::string> & keywords() {
            static const std::vector<std::string> kw = {
                "struct", "internal", "vertex", "fragment", "compute", "hull",
                "domain", "mesh", "if", "else", "for", "while", "break",
                "continue", "discard", "switch", "case", "default", "return",
                "in", "out", "inout", "static", "const", "threadgroup",
                "true", "false"
            };
            return kw;
        }

        const std::vector<std::string> & builtinTypes() {
            static const std::vector<std::string> ty = {
                "void", "bool", "int", "uint", "float",
                "bool2", "bool3", "bool4",
                "int2", "int3", "int4", "uint2", "uint3", "uint4",
                "float2", "float3", "float4",
                "float2x2", "float3x3", "float4x4",
                "float2x3", "float2x4", "float3x2", "float3x4", "float4x2", "float4x3",
                "int2x2", "int3x3", "int4x4", "uint2x2", "uint3x3", "uint4x4",
                "half", "half2", "half3", "half4", "double",
                "buffer", "uniform",
                "texture1d", "texture2d", "texture3d",
                "texture1d_array", "texture2d_array",
                "texturecube", "texturecube_array",
                "texture2d_ms", "texture2d_ms_array",
                "sampler1d", "sampler2d", "sampler3d", "samplercube",
                "atomic_int", "atomic_uint"
            };
            return ty;
        }

        const std::vector<std::string> & builtinFunctions() {
            static const std::vector<std::string> fn = {
                "dot", "cross", "normalize", "length", "reflect",
                "sample", "read", "write",
                "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
                "sqrt", "abs", "floor", "ceil", "round", "frac",
                "exp", "exp2", "log", "log2", "pow",
                "min", "max", "clamp", "lerp", "step", "smoothstep",
                "transpose", "determinant", "inverse",
                "float2", "float3", "float4",
                "int2", "int3", "int4", "uint2", "uint3", "uint4",
                "make_float2", "make_float3", "make_float4"
            };
            return fn;
        }

        bool listContains(const std::vector<std::string> & list, const std::string & s) {
            for (const auto & e : list) {
                if (e == s) {
                    return true;
                }
            }
            return false;
        }

        /// --- Source helpers (operate on the editor's raw text) ---------------

        /// Extract the contents of 0-based line `line0` from `text`.
        std::string lineAt(const std::string & text, int line0) {
            if (line0 < 0) {
                return "";
            }
            int current = 0;
            size_t start = 0;
            for (size_t i = 0; i <= text.size(); i++) {
                if (i == text.size() || text[i] == '\n') {
                    if (current == line0) {
                        size_t end = i;
                        if (end > start && text[end - 1] == '\r') {
                            end--;
                        }
                        return text.substr(start, end - start);
                    }
                    current++;
                    start = i + 1;
                }
            }
            return "";
        }

        bool isIdentChar(char c) {
            return (std::isalnum((unsigned char)c) != 0) || c == '_';
        }

        /// Identifier token covering 0-based `character` on `line`, with its
        /// half-open column span. Empty name when the cursor isn't on a word.
        std::string identifierAt(const std::string & line, int character,
                                 unsigned & startCol, unsigned & endCol) {
            int n = (int)line.size();
            int pos = character;
            if (pos > n) {
                pos = n;
            }
            /// Allow the cursor to sit just past the end of a word.
            if (pos >= n || !isIdentChar(line[pos])) {
                if (pos > 0 && isIdentChar(line[pos - 1])) {
                    pos = pos - 1;
                } else {
                    return "";
                }
            }
            int s = pos;
            int e = pos;
            while (s > 0 && isIdentChar(line[s - 1])) {
                s--;
            }
            while (e + 1 < n && isIdentChar(line[e + 1])) {
                e++;
            }
            startCol = (unsigned)s;
            endCol = (unsigned)(e + 1);
            return line.substr(s, (size_t)(e - s + 1));
        }

    } // namespace

    LspServer::LspServer(std::istream & in, std::ostream & out, Analyzer & analyzer)
        : in_(in), out_(out), analyzer_(analyzer) {}

    bool LspServer::readMessage(std::string & body) {
        size_t contentLength = 0;
        bool sawContentLength = false;
        std::string headerLine;

        while (std::getline(in_, headerLine)) {
            if (!headerLine.empty() && headerLine.back() == '\r') {
                headerLine.pop_back();
            }
            if (headerLine.empty()) {
                break; // blank line terminates the header block
            }
            size_t colon = headerLine.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string key = headerLine.substr(0, colon);
            std::string value = headerLine.substr(colon + 1);
            size_t vs = value.find_first_not_of(" \t");
            if (vs != std::string::npos) {
                value = value.substr(vs);
            }
            /// Case-insensitive compare on the one header we need.
            std::string lowered;
            lowered.reserve(key.size());
            for (char c : key) {
                lowered += (char)std::tolower((unsigned char)c);
            }
            if (lowered == "content-length") {
                contentLength = (size_t)std::strtoul(value.c_str(), nullptr, 10);
                sawContentLength = true;
            }
        }

        if (!sawContentLength) {
            return false; // EOF or malformed framing
        }
        body.resize(contentLength);
        if (contentLength > 0) {
            in_.read(&body[0], (std::streamsize)contentLength);
            if ((size_t)in_.gcount() != contentLength) {
                return false;
            }
        }
        return true;
    }

    void LspServer::writeMessage(const Json & message) {
        std::string body = message.dump();
        out_ << "Content-Length: " << body.size() << "\r\n\r\n" << body;
        out_.flush();
    }

    void LspServer::sendResult(const Json & id, Json result) {
        Json msg = Json::object();
        msg.set("jsonrpc", Json::str("2.0"));
        msg.set("id", id);
        msg.set("result", std::move(result));
        writeMessage(msg);
    }

    void LspServer::sendError(const Json & id, int code, const std::string & message) {
        Json err = Json::object();
        err.set("code", Json::integer(code));
        err.set("message", Json::str(message));
        Json msg = Json::object();
        msg.set("jsonrpc", Json::str("2.0"));
        msg.set("id", id);
        msg.set("error", std::move(err));
        writeMessage(msg);
    }

    int LspServer::run() {
        std::string body;
        while (readMessage(body)) {
            if (body.empty()) {
                continue;
            }
            OJSON message = OJSON::parse(OmegaCommon::String(body));
            if (dispatch(message)) {
                break; // `exit`
            }
        }
        return shutdownRequested_ ? 0 : 1;
    }

    bool LspServer::dispatch(OJSON & message) {
        std::string method = getString(message, "method");
        OJSON * idField = field(message, "id");
        Json id = idToJson(idField);
        OJSON * paramsField = field(message, "params");
        OJSON emptyParams = OJSON::parse(OmegaCommon::String("{}"));
        OJSON & params = paramsField != nullptr ? *paramsField : emptyParams;

        if (method == "initialize") {
            handleInitialize(id, params);
        } else if (method == "initialized") {
            // No-op notification.
        } else if (method == "shutdown") {
            shutdownRequested_ = true;
            sendResult(id, Json::null());
        } else if (method == "exit") {
            return true;
        } else if (method == "textDocument/didOpen") {
            handleDidOpen(params);
        } else if (method == "textDocument/didChange") {
            handleDidChange(params);
        } else if (method == "textDocument/didClose") {
            handleDidClose(params);
        } else if (method == "textDocument/documentSymbol") {
            handleDocumentSymbol(id, params);
        } else if (method == "textDocument/hover") {
            handleHover(id, params);
        } else if (method == "textDocument/completion") {
            handleCompletion(id, params);
        } else if (idField != nullptr) {
            /// Unknown *request* (has an id): answer with MethodNotFound so the
            /// client isn't left waiting. Unknown notifications are ignored.
            sendError(id, -32601, "method not found: " + method);
        }
        return false;
    }

    void LspServer::handleInitialize(const Json & id, OJSON & /*params*/) {
        Json completion = Json::object();
        completion.set("resolveProvider", Json::boolean(false));

        Json capabilities = Json::object();
        capabilities.set("textDocumentSync", Json::integer(1)); // Full
        capabilities.set("documentSymbolProvider", Json::boolean(true));
        capabilities.set("hoverProvider", Json::boolean(true));
        capabilities.set("completionProvider", std::move(completion));

        Json serverInfo = Json::object();
        serverInfo.set("name", Json::str("omegasl-lsp"));
        serverInfo.set("version", Json::str("0.1.0"));

        Json result = Json::object();
        result.set("capabilities", std::move(capabilities));
        result.set("serverInfo", std::move(serverInfo));
        sendResult(id, std::move(result));
    }

    void LspServer::handleDidOpen(OJSON & params) {
        OJSON * doc = field(params, "textDocument");
        if (doc == nullptr) {
            return;
        }
        std::string uri = getString(*doc, "uri");
        std::string text = getString(*doc, "text");
        if (uri.empty()) {
            return;
        }
        documents_[uri] = text;
        analyzeAndPublish(uri);
    }

    void LspServer::handleDidChange(OJSON & params) {
        OJSON * doc = field(params, "textDocument");
        if (doc == nullptr) {
            return;
        }
        std::string uri = getString(*doc, "uri");
        if (uri.empty()) {
            return;
        }
        /// Full text sync: the last content change carries the whole document.
        OJSON * changes = field(params, "contentChanges");
        if (changes != nullptr && changes->isArray()) {
            auto arr = changes->asVector();
            if (arr.size() > 0) {
                OJSON & last = const_cast<OJSON &>(arr[arr.size() - 1]);
                documents_[uri] = getString(last, "text");
            }
        }
        analyzeAndPublish(uri);
    }

    void LspServer::handleDidClose(OJSON & params) {
        OJSON * doc = field(params, "textDocument");
        if (doc == nullptr) {
            return;
        }
        std::string uri = getString(*doc, "uri");
        if (uri.empty()) {
            return;
        }
        documents_.erase(uri);
        analyses_.erase(uri);
        /// Clear diagnostics for the closed document.
        AnalysisResult empty;
        publishDiagnostics(uri, empty);
    }

    void LspServer::analyzeAndPublish(const std::string & uri) {
        auto it = documents_.find(uri);
        if (it == documents_.end()) {
            return;
        }
        AnalysisResult result = analyzer_.analyze(it->second);
        publishDiagnostics(uri, result);
        analyses_[uri] = std::move(result);
    }

    void LspServer::publishDiagnostics(const std::string & uri, const AnalysisResult & result) {
        Json diagnostics = Json::array();
        for (const auto & d : result.diagnostics) {
            Json item = Json::object();
            item.set("range", makeRange(d.range));
            item.set("severity", Json::integer((int)d.severity));
            item.set("source", Json::str("omegasl"));
            item.set("message", Json::str(d.message));
            diagnostics.push(std::move(item));
        }
        Json params = Json::object();
        params.set("uri", Json::str(uri));
        params.set("diagnostics", std::move(diagnostics));

        Json msg = Json::object();
        msg.set("jsonrpc", Json::str("2.0"));
        msg.set("method", Json::str("textDocument/publishDiagnostics"));
        msg.set("params", std::move(params));
        writeMessage(msg);
    }

    void LspServer::handleDocumentSymbol(const Json & id, OJSON & params) {
        OJSON * doc = field(params, "textDocument");
        std::string uri = doc != nullptr ? getString(*doc, "uri") : std::string();

        Json symbols = Json::array();
        auto it = analyses_.find(uri);
        if (it != analyses_.end()) {
            for (const auto & sym : it->second.symbols) {
                Json node = Json::object();
                node.set("name", Json::str(sym.name));
                if (!sym.detail.empty()) {
                    node.set("detail", Json::str(sym.detail));
                }
                node.set("kind", Json::integer(lspSymbolKind(sym.kind)));
                node.set("range", makeRange(sym.range));
                node.set("selectionRange", makeRange(sym.range));
                symbols.push(std::move(node));
            }
        }
        sendResult(id, std::move(symbols));
    }

    void LspServer::handleHover(const Json & id, OJSON & params) {
        OJSON * doc = field(params, "textDocument");
        OJSON * position = field(params, "position");
        std::string uri = doc != nullptr ? getString(*doc, "uri") : std::string();
        int line = position != nullptr ? getInt(*position, "line") : 0;
        int character = position != nullptr ? getInt(*position, "character") : 0;

        auto docIt = documents_.find(uri);
        if (docIt == documents_.end()) {
            sendResult(id, Json::null());
            return;
        }

        std::string lineText = lineAt(docIt->second, line);
        unsigned startCol = 0;
        unsigned endCol = 0;
        std::string word = identifierAt(lineText, character, startCol, endCol);
        if (word.empty()) {
            sendResult(id, Json::null());
            return;
        }

        std::string signature;
        std::string note;

        auto analysisIt = analyses_.find(uri);
        if (analysisIt != analyses_.end()) {
            auto symIt = analysisIt->second.index.find(word);
            if (symIt != analysisIt->second.index.end()) {
                signature = symIt->second.signature;
                note = symIt->second.detail;
            }
        }
        if (signature.empty()) {
            if (listContains(builtinFunctions(), word)) {
                signature = word + "(...)";
                note = "builtin function";
            } else if (listContains(builtinTypes(), word)) {
                signature = word;
                note = "builtin type";
            } else if (listContains(keywords(), word)) {
                signature = word;
                note = "keyword";
            }
        }

        if (signature.empty()) {
            sendResult(id, Json::null());
            return;
        }

        std::string markdown = "```omegasl\n" + signature + "\n```";
        if (!note.empty()) {
            markdown += "\n\n" + note;
        }

        Json contents = Json::object();
        contents.set("kind", Json::str("markdown"));
        contents.set("value", Json::str(markdown));

        Range range;
        range.startLine = (unsigned)line;
        range.endLine = (unsigned)line;
        range.startChar = startCol;
        range.endChar = endCol;

        Json result = Json::object();
        result.set("contents", std::move(contents));
        result.set("range", makeRange(range));
        sendResult(id, std::move(result));
    }

    void LspServer::handleCompletion(const Json & id, OJSON & params) {
        OJSON * doc = field(params, "textDocument");
        std::string uri = doc != nullptr ? getString(*doc, "uri") : std::string();

        Json items = Json::array();

        auto addItem = [&](const std::string & label, int kind, const std::string & detail) {
            Json item = Json::object();
            item.set("label", Json::str(label));
            item.set("kind", Json::integer(kind));
            if (!detail.empty()) {
                item.set("detail", Json::str(detail));
            }
            items.push(std::move(item));
        };

        for (const auto & kw : keywords()) {
            addItem(kw, kCompletionKeyword, "keyword");
        }
        for (const auto & ty : builtinTypes()) {
            addItem(ty, kCompletionClass, "builtin type");
        }
        for (const auto & fn : builtinFunctions()) {
            addItem(fn, kCompletionFunction, "builtin function");
        }

        /// The document's own top-level declarations.
        auto analysisIt = analyses_.find(uri);
        if (analysisIt != analyses_.end()) {
            for (const auto & sym : analysisIt->second.symbols) {
                int kind = kCompletionVariable;
                switch (sym.kind) {
                    case SymbolKind::Struct:   kind = kCompletionStruct; break;
                    case SymbolKind::Function: kind = kCompletionFunction; break;
                    case SymbolKind::VertexShader:
                    case SymbolKind::FragmentShader:
                    case SymbolKind::ComputeShader:
                    case SymbolKind::HullShader:
                    case SymbolKind::DomainShader:
                    case SymbolKind::MeshShader: kind = kCompletionFunction; break;
                    case SymbolKind::Constant: kind = kCompletionConstant; break;
                    default:                   kind = kCompletionVariable; break;
                }
                addItem(sym.name, kind, sym.detail);
            }
        }

        /// `isIncomplete:false` — the list is exhaustive for this position.
        Json result = Json::object();
        result.set("isIncomplete", Json::boolean(false));
        result.set("items", std::move(items));
        sendResult(id, std::move(result));
    }

} // namespace lsp
} // namespace omegasl
