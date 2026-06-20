// JSONLifecycleTest — verification for JSON API Completion Plan, Phase 1
// (ownership foundation). Exercises the Rule-of-Five members added to
// OmegaCommon::JSON: deep-copy independence, move transfer, copy/move
// assignment, and self-assignment safety. Run under AddressSanitizer to catch
// double-free / use-after-free, and under the macOS `leaks` tool to confirm the
// pre-Phase-1 container leak is gone.
//
// Only the pre-existing public JSON surface is used (parse/serialize/isX/asX/
// operator[]); parse input is always valid because parse() still hard-exits on
// malformed input until Phase 4 lands TryParse.

#include "omega-common/json.h"

#include <iostream>
#include <string>
#include <utility>

using OmegaCommon::JSON;
using OmegaCommon::String;

static int g_failures = 0;

static void check(bool cond, const char *what) {
  if (cond) {
    std::cout << "  ok: " << what << "\n";
  } else {
    std::cerr << "  FAIL: " << what << "\n";
    ++g_failures;
  }
}

// Compare an asString() view against a literal (StrRef::operator== handles this).
static bool strEq(JSON &node, const char *expected) {
  return node.asString() == expected;
}

static void testCopyIsDeepAndIndependent() {
  std::cout << "[copy ctor: deep + independent]\n";
  JSON original = JSON::parse(
      String(R"({"name":"omega","nested":{"k":"v"},"list":["a","b"]})"));
  check(original.isMap(), "parsed root is a map");

  JSON copy = original; // copy constructor

  // Mutating a top-level string in the copy must not touch the original.
  copy["name"] = JSON("changed");
  check(strEq(original["name"], "omega"), "original.name unchanged by copy edit");
  check(strEq(copy["name"], "changed"), "copy.name reflects the edit");

  // Mutating a *nested* node proves the nested Map was cloned, not aliased.
  copy["nested"]["k"] = JSON("changed2");
  check(strEq(original["nested"]["k"], "v"), "original.nested.k unchanged");
  check(strEq(copy["nested"]["k"], "changed2"), "copy.nested.k reflects edit");

  // Growing the copy's array must not grow the original's.
  copy["list"].push_back(JSON("c"));
  check(original["list"].asVector().size() == 2, "original.list still size 2");
  check(copy["list"].asVector().size() == 3, "copy.list grew to size 3");
}

static void testMoveTransfersOwnership() {
  std::cout << "[move ctor: transfer + empty source]\n";
  JSON a = JSON::parse(String(R"(["x","y","z"])"));
  check(a.isArray(), "a is an array before move");

  JSON b = std::move(a); // move constructor
  check(b.isArray(), "b owns the array after move");
  check(b.asVector().size() == 3, "b has all 3 elements");
  check(!a.isArray() && !a.isMap() && !a.isString(),
        "moved-from a is an empty node");
}

static void testCopyAssignment() {
  std::cout << "[copy assignment: replaces + stays independent]\n";
  JSON a = JSON::parse(String(R"({"a":"1"})"));
  JSON b = JSON::parse(String(R"({"b":"2"})"));

  b = a; // copy assignment: frees b's old map, deep-copies a's
  check(strEq(b["a"], "1"), "b adopted a's contents");

  b["a"] = JSON("mutated");
  check(strEq(a["a"], "1"), "a unaffected by later edit to b");
}

static void testMoveAssignment() {
  std::cout << "[move assignment: replaces + empties source]\n";
  JSON a = JSON::parse(String(R"({"k":"v"})"));
  JSON b = JSON::parse(String(R"(["junk"])"));

  b = std::move(a); // move assignment: frees b's old array, takes a's map
  check(b.isMap(), "b is a map after move-assign");
  check(strEq(b["k"], "v"), "b holds the moved value");
  check(!a.isMap(), "moved-from a is an empty node");
}

static void testSelfAssignmentIsSafe() {
  std::cout << "[self-assignment: guarded]\n";
  JSON a = JSON::parse(String(R"({"k":"v"})"));
  JSON &alias = a;

  a = alias; // self copy-assign
  check(strEq(a["k"], "v"), "self copy-assign left value intact");

  a = std::move(alias); // self move-assign
  check(a.isMap() && strEq(a["k"], "v"), "self move-assign left value intact");
}

static void testRoundTripSurvivesCopy() {
  std::cout << "[serialize: copy serializes identically]\n";
  JSON original = JSON::parse(String(R"({"arr":[{"x":"1"},{"y":"2"}]})"));
  JSON copy = original;
  String s1 = JSON::serialize(original);
  String s2 = JSON::serialize(copy);
  check(s1 == s2, "deep copy serializes byte-for-byte like the original");
}

int main() {
  testCopyIsDeepAndIndependent();
  testMoveTransfersOwnership();
  testCopyAssignment();
  testMoveAssignment();
  testSelfAssignmentIsSafe();
  testRoundTripSurvivesCopy();

  if (g_failures == 0) {
    std::cout << "\nJSONLifecycleTest: ALL CHECKS PASSED\n";
    return 0;
  }
  std::cerr << "\nJSONLifecycleTest: " << g_failures << " CHECK(S) FAILED\n";
  return 1;
}
