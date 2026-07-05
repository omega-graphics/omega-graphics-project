#!/usr/bin/env python3
"""End-to-end test for omegasl-lsp #include resolution (LSP Phase 5)."""
import subprocess, json, os, sys, tempfile, shutil

LSP = sys.argv[1] if len(sys.argv) > 1 else "build/bin/omegasl-lsp"

def frame(obj):
    b = json.dumps(obj).encode()
    return b"Content-Length: %d\r\n\r\n%s" % (len(b), b)

def uri(path):
    return "file://" + path

def run_session(messages):
    """Feed framed messages, return (list of parsed responses, stderr text)."""
    data = b"".join(frame(m) for m in messages)
    p = subprocess.run([LSP], input=data, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=30)
    out, resps, i = p.stdout, [], 0
    while True:
        h = out.find(b"\r\n\r\n", i)
        if h < 0: break
        cl = None
        for line in out[i:h].decode().split("\r\n"):
            if line.lower().startswith("content-length:"):
                cl = int(line.split(":")[1])
        resps.append(json.loads(out[h+4:h+4+cl])); i = h+4+cl
    return resps, p.stderr.decode()

def diagnostics_for(resps, u):
    for r in resps:
        if r.get("method") == "textDocument/publishDiagnostics" and r["params"]["uri"] == u:
            return r["params"]["diagnostics"]
    return None

def result_for(resps, id_):
    for r in resps:
        if r.get("id") == id_:
            return r.get("result")
    return None

HEADER = """// shared declarations only
struct FuncData { float4 value; };
buffer<FuncData> hdr_src : 0;
buffer<FuncData> hdr_dst : 1;
float scale_value(float x, float k){ return x * k; }
"""

def main_src(include_line):
    return (include_line + "\n" +
            "[in hdr_src, out hdr_dst]\n"
            "compute(x=1,y=1,z=1)\n"
            "void useHeaderHelper(uint3 tid : GlobalThreadID){\n"
            "    float a = hdr_src[0].value[0];\n"
            "    float k = hdr_src[0].value[1];\n"
            "    float scaled = scale_value(a, k);\n"
            "    hdr_dst[0].value = float4(scaled, a, k, 1.0);\n"
            "}\n")

def open_and_query(proj, fname, text, dbdir=None):
    """Open <proj>/<fname>, return (diags, documentSymbol names, completion labels)."""
    path = os.path.join(proj, fname)
    u = uri(path)
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
        {"jsonrpc":"2.0","method":"initialized","params":{}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
            "textDocument":{"uri":u,"languageId":"omegasl","version":1,"text":text}}},
        {"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol",
            "params":{"textDocument":{"uri":u}}},
        {"jsonrpc":"2.0","id":3,"method":"textDocument/completion",
            "params":{"textDocument":{"uri":u},"position":{"line":0,"character":0}}},
        {"jsonrpc":"2.0","id":9,"method":"shutdown"},
        {"jsonrpc":"2.0","method":"exit"},
    ]
    resps, err = run_session(msgs)
    diags = diagnostics_for(resps, u)
    syms = result_for(resps, 2) or []
    symnames = [s["name"] for s in syms]
    comp = result_for(resps, 3) or {}
    labels = [it["label"] for it in comp.get("items", [])]
    return diags, symnames, labels, err

failures = []
def check(cond, msg):
    print(("PASS" if cond else "FAIL") + ": " + msg)
    if not cond: failures.append(msg)

proj = tempfile.mkdtemp(prefix="omegasl_lsp_e2e_")
try:
    # Layout:
    #   <proj>/helper.omegaslh          (relative include target)
    #   <proj>/inc/searchhelper.omegaslh (found only via -I)
    #   <proj>/omegasl_commands.json     (maps main_search -> inc)
    with open(os.path.join(proj, "helper.omegaslh"), "w") as f:
        f.write(HEADER)
    os.makedirs(os.path.join(proj, "inc"))
    with open(os.path.join(proj, "inc", "searchhelper.omegaslh"), "w") as f:
        f.write(HEADER.replace("scale_value", "search_scale")
                      .replace("hdr_src", "search_src").replace("hdr_dst", "search_dst"))

    # ---- Case A: NO include, uses scale_value -> should error (undeclared) ----
    diags, syms, labels, _ = open_and_query(proj, "main_missing.omegasl",
                                             main_src("// no include here"))
    check(diags is not None and len(diags) > 0,
          "A: file using an unresolved header symbol reports diagnostics (got %s)"
          % (None if diags is None else len(diags)))

    # ---- Case B: relative #include resolves -> clean ----
    diags, syms, labels, _ = open_and_query(proj, "main.omegasl",
                                            main_src('#include "helper.omegaslh"'))
    check(diags == [], "B: relative #include resolves, zero diagnostics (got %r)" % diags)
    check("useHeaderHelper" in syms,
          "B: the unit's own shader IS in the document outline (got %r)" % syms)
    check("scale_value" not in syms and "FuncData" not in syms,
          "B: header-declared symbols are NOT in the outline (got %r)" % syms)
    check("scale_value" in labels,
          "B: header helper IS offered in completion (index) (present=%s)"
          % ("scale_value" in labels))

    # ---- Case C: -I via omegasl_commands.json resolves a sibling-dir header ----
    search_src = main_src('#include "searchhelper.omegaslh"') \
        .replace("hdr_src", "search_src").replace("hdr_dst", "search_dst") \
        .replace("scale_value", "search_scale").replace("useHeaderHelper", "useSearchHeader")
    db = [{"file": os.path.join(proj, "main_search.omegasl"),
           "includeDirs": [os.path.join(proj, "inc")]}]
    with open(os.path.join(proj, "omegasl_commands.json"), "w") as f:
        json.dump(db, f)
    diags, syms, labels, _ = open_and_query(proj, "main_search.omegasl", search_src)
    check(diags == [], "C: -I include dir from omegasl_commands.json resolves, zero diags (got %r)" % diags)
    check("search_scale" in labels, "C: header symbol via -I is offered in completion")

    # ---- Case C-neg: same file, DB entry REMOVED -> should NOT resolve ----
    with open(os.path.join(proj, "omegasl_commands.json"), "w") as f:
        json.dump([], f)  # empty DB: no include dirs for main_search
    diags, syms, labels, _ = open_and_query(proj, "main_search.omegasl", search_src)
    check(diags is not None and len(diags) > 0,
          "C-neg: without the DB entry the sibling-dir header is unresolved -> diagnostics (got %s)"
          % (None if diags is None else len(diags)))

    # ---- Case D: malformed omegasl_commands.json is ignored, server survives ----
    with open(os.path.join(proj, "omegasl_commands.json"), "w") as f:
        f.write('[ {"file": BROKEN ]')  # invalid JSON
    diags, syms, labels, err = open_and_query(proj, "main.omegasl",
                                              main_src('#include "helper.omegaslh"'))
    check(diags == [], "D: malformed DB ignored; relative include still resolves (got %r)" % diags)
    check("ignoring malformed" in err, "D: malformed DB logged to stderr")

finally:
    shutil.rmtree(proj, ignore_errors=True)

print("\n" + ("ALL PASS" if not failures else "%d FAILURE(S)" % len(failures)))
sys.exit(1 if failures else 0)
