#include "LspServer.h"

#include <omega-common/json.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
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

        /// Echo a request id back — it may be a number or a string, and the
        /// client matches the response by it. Numbers are normalized to an
        /// integer (LSP ids are integral), preserving the prior wire behavior.
        OJSON idToJson(OJSON * idField) {
            if (idField == nullptr) {
                return OJSON(nullptr);
            }
            if (idField->isNumber()) {
                return OJSON(idField->asInt());
            }
            if (idField->isString()) {
                return OJSON(OmegaCommon::String(idField->asString()));
            }
            return OJSON(nullptr);
        }

        /// --- Outgoing LSP value builders -------------------------------------

        OJSON makePosition(unsigned line, unsigned character) {
            OJSON p = OJSON::Object();
            p["line"] = OJSON((long long)line);
            p["character"] = OJSON((long long)character);
            return p;
        }

        OJSON makeRange(const Range & r) {
            OJSON out = OJSON::Object();
            out["start"] = makePosition(r.startLine, r.startChar);
            out["end"] = makePosition(r.endLine, r.endChar);
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

        /// --- Document path + compile-command database ------------------------

        /// Percent-decode a URI component (`%20` → space, etc.). Bytes that are
        /// not a valid `%XX` escape pass through unchanged.
        std::string percentDecode(const std::string & s) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i] == '%' && i + 2 < s.size()) {
                    int hi = hexVal(s[i + 1]);
                    int lo = hexVal(s[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out += (char)((hi << 4) | lo);
                        i += 2;
                        continue;
                    }
                }
                out += s[i];
            }
            return out;
        }

        /// Convert a `file://` document URI to an absolute filesystem path.
        /// Returns "" for a non-`file:` URI (`untitled:` etc.), which the
        /// analyzer treats as "no anchor". Handles the empty-authority form
        /// (`file:///path`) clients emit, a rare non-empty authority, and the
        /// Windows drive form (`file:///C:/x` → `C:/x`).
        std::string uriToPath(const std::string & uri) {
            const std::string scheme = "file://";
            if (uri.compare(0, scheme.size(), scheme) != 0) {
                return "";
            }
            std::string rest = uri.substr(scheme.size());
            /// Strip a non-empty authority (host) if present; the common
            /// `file:///` form has an empty authority so `rest` already starts
            /// with '/'.
            if (!rest.empty() && rest[0] != '/') {
                size_t slash = rest.find('/');
                rest = (slash == std::string::npos) ? std::string() : rest.substr(slash);
            }
            std::string path = percentDecode(rest);
            if (path.size() >= 3 && path[0] == '/' &&
                (std::isalpha((unsigned char)path[1]) != 0) && path[2] == ':') {
                path.erase(0, 1);
            }
            return path;
        }

        /// The parent directory of `path` (no trailing slash), or "" when there
        /// is no separator. The parent of a top-level entry (`/a`) is `/`.
        std::string parentDir(const std::string & path) {
            size_t slash = path.find_last_of("/\\");
            if (slash == std::string::npos) {
                return "";
            }
            if (slash == 0) {
                return "/";
            }
            return path.substr(0, slash);
        }

        /// Walk up from `startDir` looking for an `omegasl_commands.json`.
        /// Returns its path, or "" if none is found before the filesystem root
        /// (mirrors clang's `compile_commands.json` upward search).
        std::string discoverCompileDb(std::string dir) {
            while (!dir.empty()) {
                std::string candidate = dir + "/omegasl_commands.json";
                std::ifstream f(candidate);
                if (f.good()) {
                    return candidate;
                }
                std::string parent = parentDir(dir);
                if (parent == dir) {
                    break; // reached the root
                }
                dir = parent;
            }
            return std::string();
        }

        /// Parse an `omegasl_commands.json` into `absSourceFile → includeDirs`.
        /// The schema is an array of `{ "file": <abs>, "includeDirs": [<abs>…] }`.
        /// A malformed database is logged and ignored (never fatal) — the server
        /// simply falls back to relative-only include resolution.
        std::map<std::string, std::vector<std::string>> loadCompileDb(const std::string & dbPath) {
            std::map<std::string, std::vector<std::string>> db;
            std::ifstream f(dbPath, std::ios::binary);
            if (!f) {
                return db;
            }
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            auto parsed = OJSON::TryParse(OmegaCommon::String(content));
            if (parsed.isErr()) {
                std::cerr << "omegasl-lsp: ignoring malformed " << dbPath << ": "
                          << parsed.error() << std::endl;
                return db;
            }
            OJSON & root = parsed.value();
            if (!root.isArray()) {
                return db;
            }
            auto arr = root.asVector();
            for (const OJSON & entryC : arr) {
                OJSON & entry = const_cast<OJSON &>(entryC);
                if (!entry.isMap()) {
                    continue;
                }
                OJSON * fileF = field(entry, "file");
                if (fileF == nullptr || !fileF->isString()) {
                    continue;
                }
                std::string file(fileF->asString());
                std::vector<std::string> dirs;
                OJSON * dirsF = field(entry, "includeDirs");
                if (dirsF != nullptr && dirsF->isArray()) {
                    auto darr = dirsF->asVector();
                    for (const OJSON & d : darr) {
                        if (d.isString()) {
                            dirs.emplace_back(d.asString());
                        }
                    }
                }
                db[file] = std::move(dirs);
            }
            return db;
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

    void LspServer::writeMessage(OJSON & message) {
        /// Compact (whitespace-free) serialization for the wire.
        OmegaCommon::String body = OJSON::serialize(message, false);
        out_ << "Content-Length: " << body.size() << "\r\n\r\n" << body;
        out_.flush();
    }

    void LspServer::sendResult(const OJSON & id, OJSON result) {
        OJSON msg = OJSON::Object();
        msg["jsonrpc"] = OJSON("2.0");
        msg["id"] = id;
        msg["result"] = std::move(result);
        writeMessage(msg);
    }

    void LspServer::sendError(const OJSON & id, int code, const std::string & message) {
        OJSON err = OJSON::Object();
        err["code"] = OJSON(code);
        err["message"] = OJSON(OmegaCommon::String(message));
        OJSON msg = OJSON::Object();
        msg["jsonrpc"] = OJSON("2.0");
        msg["id"] = id;
        msg["error"] = std::move(err);
        writeMessage(msg);
    }

    int LspServer::run() {
        std::string body;
        while (readMessage(body)) {
            if (body.empty()) {
                continue;
            }
            /// Parse defensively: a single malformed message must not take the
            /// server down. Per JSON-RPC 2.0, answer with ParseError (-32700)
            /// and a null id — we can't recover an id from unparseable text —
            /// log to stderr, and keep serving the next message.
            auto parsed = OJSON::TryParse(OmegaCommon::String(body));
            if (parsed.isErr()) {
                std::cerr << "omegasl-lsp: dropping malformed message: "
                          << parsed.error() << std::endl;
                sendError(OJSON(nullptr), -32700, "Parse error: " + parsed.error());
                continue;
            }
            if (dispatch(parsed.value())) {
                break; // `exit`
            }
        }
        return shutdownRequested_ ? 0 : 1;
    }

    bool LspServer::dispatch(OJSON & message) {
        std::string method = getString(message, "method");
        OJSON * idField = field(message, "id");
        OJSON id = idToJson(idField);
        OJSON * paramsField = field(message, "params");
        OJSON emptyParams = OJSON::Object();
        OJSON & params = paramsField != nullptr ? *paramsField : emptyParams;

        if (method == "initialize") {
            handleInitialize(id, params);
        } else if (method == "initialized") {
            // No-op notification.
        } else if (method == "shutdown") {
            shutdownRequested_ = true;
            sendResult(id, OJSON(nullptr));
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

    void LspServer::handleInitialize(const OJSON & id, OJSON & /*params*/) {
        OJSON completion = OJSON::Object();
        completion["resolveProvider"] = OJSON(false);

        OJSON capabilities = OJSON::Object();
        capabilities["textDocumentSync"] = OJSON(1); // Full
        capabilities["documentSymbolProvider"] = OJSON(true);
        capabilities["hoverProvider"] = OJSON(true);
        capabilities["completionProvider"] = std::move(completion);

        OJSON serverInfo = OJSON::Object();
        serverInfo["name"] = OJSON("omegasl-lsp");
        serverInfo["version"] = OJSON("0.1.0");

        OJSON result = OJSON::Object();
        result["capabilities"] = std::move(capabilities);
        result["serverInfo"] = std::move(serverInfo);
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

    std::vector<std::string> LspServer::includeDirsForDocument(const std::string & docPath) {
        if (docPath.empty()) {
            return {};
        }
        std::string dir = parentDir(docPath);
        /// 1. Which `omegasl_commands.json` governs this directory? Cache the
        ///    upward search per directory so it runs once, not per keystroke.
        std::string dbPath;
        auto dirIt = dbPathForDir_.find(dir);
        if (dirIt != dbPathForDir_.end()) {
            dbPath = dirIt->second;
        } else {
            dbPath = discoverCompileDb(dir);
            dbPathForDir_[dir] = dbPath;
        }
        if (dbPath.empty()) {
            return {};
        }
        /// 2. Load + cache the database.
        auto dbIt = dbByPath_.find(dbPath);
        if (dbIt == dbByPath_.end()) {
            dbIt = dbByPath_.emplace(dbPath, loadCompileDb(dbPath)).first;
        }
        /// 3. This document's entry (exact absolute-path match), if any.
        auto e = dbIt->second.find(docPath);
        if (e != dbIt->second.end()) {
            return e->second;
        }
        return {};
    }

    void LspServer::analyzeAndPublish(const std::string & uri) {
        auto it = documents_.find(uri);
        if (it == documents_.end()) {
            return;
        }
        std::string docPath = uriToPath(uri);
        std::vector<std::string> includeDirs = includeDirsForDocument(docPath);
        AnalysisResult result = analyzer_.analyze(it->second, docPath, includeDirs);
        publishDiagnostics(uri, result);
        analyses_[uri] = std::move(result);
    }

    void LspServer::publishDiagnostics(const std::string & uri, const AnalysisResult & result) {
        OJSON diagnostics = OJSON::Array();
        for (const auto & d : result.diagnostics) {
            OJSON item = OJSON::Object();
            item["range"] = makeRange(d.range);
            item["severity"] = OJSON((int)d.severity);
            item["source"] = OJSON("omegasl");
            item["message"] = OJSON(OmegaCommon::String(d.message));
            diagnostics.push_back(item);
        }
        OJSON params = OJSON::Object();
        params["uri"] = OJSON(OmegaCommon::String(uri));
        params["diagnostics"] = std::move(diagnostics);

        OJSON msg = OJSON::Object();
        msg["jsonrpc"] = OJSON("2.0");
        msg["method"] = OJSON("textDocument/publishDiagnostics");
        msg["params"] = std::move(params);
        writeMessage(msg);
    }

    void LspServer::handleDocumentSymbol(const OJSON & id, OJSON & params) {
        OJSON * doc = field(params, "textDocument");
        std::string uri = doc != nullptr ? getString(*doc, "uri") : std::string();

        OJSON symbols = OJSON::Array();
        auto it = analyses_.find(uri);
        if (it != analyses_.end()) {
            for (const auto & sym : it->second.symbols) {
                OJSON node = OJSON::Object();
                node["name"] = OJSON(OmegaCommon::String(sym.name));
                if (!sym.detail.empty()) {
                    node["detail"] = OJSON(OmegaCommon::String(sym.detail));
                }
                node["kind"] = OJSON(lspSymbolKind(sym.kind));
                node["range"] = makeRange(sym.range);
                node["selectionRange"] = makeRange(sym.range);
                symbols.push_back(node);
            }
        }
        sendResult(id, std::move(symbols));
    }

    void LspServer::handleHover(const OJSON & id, OJSON & params) {
        OJSON * doc = field(params, "textDocument");
        OJSON * position = field(params, "position");
        std::string uri = doc != nullptr ? getString(*doc, "uri") : std::string();
        int line = position != nullptr ? getInt(*position, "line") : 0;
        int character = position != nullptr ? getInt(*position, "character") : 0;

        auto docIt = documents_.find(uri);
        if (docIt == documents_.end()) {
            sendResult(id, OJSON(nullptr));
            return;
        }

        std::string lineText = lineAt(docIt->second, line);
        unsigned startCol = 0;
        unsigned endCol = 0;
        std::string word = identifierAt(lineText, character, startCol, endCol);
        if (word.empty()) {
            sendResult(id, OJSON(nullptr));
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
            sendResult(id, OJSON(nullptr));
            return;
        }

        std::string markdown = "```omegasl\n" + signature + "\n```";
        if (!note.empty()) {
            markdown += "\n\n" + note;
        }

        OJSON contents = OJSON::Object();
        contents["kind"] = OJSON("markdown");
        contents["value"] = OJSON(OmegaCommon::String(markdown));

        Range range;
        range.startLine = (unsigned)line;
        range.endLine = (unsigned)line;
        range.startChar = startCol;
        range.endChar = endCol;

        OJSON result = OJSON::Object();
        result["contents"] = std::move(contents);
        result["range"] = makeRange(range);
        sendResult(id, std::move(result));
    }

    void LspServer::handleCompletion(const OJSON & id, OJSON & params) {
        OJSON * doc = field(params, "textDocument");
        std::string uri = doc != nullptr ? getString(*doc, "uri") : std::string();

        OJSON items = OJSON::Array();

        auto addItem = [&](const std::string & label, int kind, const std::string & detail) {
            OJSON item = OJSON::Object();
            item["label"] = OJSON(OmegaCommon::String(label));
            item["kind"] = OJSON(kind);
            if (!detail.empty()) {
                item["detail"] = OJSON(OmegaCommon::String(detail));
            }
            items.push_back(item);
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

        /// Every declaration in scope — the document's own plus any pulled in
        /// from `#include`d headers. The symbol index (keyed by name) holds
        /// both, whereas `symbols` is the document-outline subset (own decls
        /// only), so completion draws from the index to offer header-provided
        /// structs, resources, and helpers too.
        auto analysisIt = analyses_.find(uri);
        if (analysisIt != analyses_.end()) {
            for (const auto & entry : analysisIt->second.index) {
                const Symbol & sym = entry.second;
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
        OJSON result = OJSON::Object();
        result["isIncomplete"] = OJSON(false);
        result["items"] = std::move(items);
        sendResult(id, std::move(result));
    }

} // namespace lsp
} // namespace omegasl
