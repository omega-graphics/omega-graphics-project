// Preprocessor source-map test (OmegaSL LSP Phase 5.1).
//
//   preprocessor_sourcemap_test <temp_dir>
//       Verify Preprocessor::sourceMap() — the output-line -> editor-line map
//       the language server uses to keep diagnostics/symbols aligned with the
//       buffer once `#include`s expand inline. The test writes its own header
//       fixture into <temp_dir>, includes it from a synthetic main buffer, and
//       asserts every main line maps back to itself while the expanded header
//       body maps to 0 (foreign). Also checks the no-include case is an
//       identity map. Pure host code, no GPU device — mirrors the
//       ArchiveRoundTripTest host-test pattern.

#include "../src/Preprocessor.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expectEq(const std::string &what, long long got, long long want) {
    if (got != want) {
        std::cerr << "FAIL: " << what << " — got " << got << ", want " << want << "\n";
        ++failures;
    }
}

std::string mapToString(const std::vector<unsigned> &m) {
    std::string s = "[";
    for (size_t i = 0; i < m.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(m[i]);
    }
    s += "]";
    return s;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: preprocessor_sourcemap_test <temp_dir>\n";
        return 2;
    }
    const std::string tempDir = argv[1];

    // A shared header of exactly two output-producing lines (a comment and a
    // helper function). No shader entry point — those are rejected in a header.
    const std::string headerPath = tempDir + "/sourcemap_header.omegaslh";
    {
        std::ofstream h(headerPath, std::ios::trunc);
        if (!h) {
            std::cerr << "cannot write fixture header " << headerPath << "\n";
            return 2;
        }
        h << "// shared helper header\n";
        h << "float4 sm_tint(float4 c){ return c; }\n";
    }

    // --- Case 1: a buffer that #includes the header on line 2 --------------
    //   line 1: struct decl              (own line)
    //   line 2: #include "..."           (directive -> blank, then header body)
    //   line 3: a decl using the header  (own line, shifted down in output)
    {
        std::string src;
        src += "struct SMV { float4 pos; };\n";                       // line 1
        src += "#include \"sourcemap_header.omegaslh\"\n";            // line 2
        src += "float4 sm_use(float4 x){ return sm_tint(x); }\n";     // line 3

        omegasl::Preprocessor pp;
        pp.setBackend(omegasl::PPBackend::MSL);
        pp.setLinePreserving(true);
        pp.setSourceMap(true);
        pp.setRejectIncludes(false);
        std::string out = pp.process(src, tempDir);

        if (pp.hasErrors()) {
            std::cerr << "FAIL: case1 preprocessor reported errors (include should resolve)\n";
            ++failures;
        }

        const std::vector<unsigned> &m = pp.sourceMap();
        // Expected output lines:
        //   1: struct decl                -> 1
        //   2: #include blank (directive) -> 2
        //   3: header comment (foreign)   -> 0
        //   4: header function (foreign)  -> 0
        //   5: sm_use decl                -> 3
        std::cerr << "case1 map = " << mapToString(m) << "\n";
        expectEq("case1 map size", (long long)m.size(), 5);
        if (m.size() == 5) {
            expectEq("case1 map[0] (struct)",  m[0], 1);
            expectEq("case1 map[1] (#include)", m[1], 2);
            expectEq("case1 map[2] (header)",  m[2], 0);
            expectEq("case1 map[3] (header)",  m[3], 0);
            expectEq("case1 map[4] (sm_use)",  m[4], 3);
        }
        // The header's declaration must actually be inlined into the output.
        if (out.find("sm_tint") == std::string::npos) {
            std::cerr << "FAIL: case1 header body not inlined into processed output\n";
            ++failures;
        }
    }

    // --- Case 2: no includes -> identity map -------------------------------
    {
        std::string src;
        src += "struct A { float4 p; };\n";   // 1
        src += "#define K 4\n";               // 2 (directive -> blank)
        src += "float4 f(){ return p; }\n";   // 3
        src += "\n";                          // 4 (blank line)
        src += "int g(){ return K; }\n";      // 5 (K expands, still one line)

        omegasl::Preprocessor pp;
        pp.setBackend(omegasl::PPBackend::MSL);
        pp.setLinePreserving(true);
        pp.setSourceMap(true);
        pp.setRejectIncludes(true); // no includes here anyway
        pp.process(src, tempDir);

        const std::vector<unsigned> &m = pp.sourceMap();
        std::cerr << "case2 map = " << mapToString(m) << "\n";
        expectEq("case2 map size", (long long)m.size(), 5);
        for (size_t i = 0; i < m.size() && i < 5; ++i) {
            expectEq("case2 identity map[" + std::to_string(i) + "]", m[i], (long long)(i + 1));
        }
    }

    if (failures == 0) {
        std::cerr << "preprocessor_sourcemap_test: all checks passed\n";
        return 0;
    }
    std::cerr << "preprocessor_sourcemap_test: " << failures << " check(s) failed\n";
    return 1;
}
