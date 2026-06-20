// JSONLookupTest — verification for JSON API Completion Plan, Phase 3
// (null, bool, const-correct reads, non-mutating lookups). Covers JSON(nullptr)
// and isNull (distinct from a default/unknown node), isBool, the const accessor
// overloads readable through a `const JSON&`, and contains/find/at/size/empty —
// in particular that find()/contains() never insert the way operator[] does.

#include "omega-common/json.h"

#include <iostream>
#include <string>

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

static void testNull() {
  std::cout << "[null: explicit, distinct from unknown, serializes as null]\n";
  JSON explicitNull = nullptr; // nullptr_t constructor
  check(explicitNull.isNull(), "JSON(nullptr) is null");
  check(!explicitNull.isBool() && !explicitNull.isNumber(), "null is only null");
  check(JSON::serialize(explicitNull) == "null", "JSON(nullptr) serializes as null");

  JSON parsedNull = JSON::parse(String("null"));
  check(parsedNull.isNull(), "parsed null is null");

  JSON defaultNode;
  check(!defaultNode.isNull(), "default (unknown) node is not an explicit null");
}

static void testBool() {
  std::cout << "[bool: isBool + const asBool]\n";
  JSON t = true;
  check(t.isBool(), "JSON(true) is a bool");
  check(!t.isNull() && !t.isNumber(), "bool is only a bool");

  const JSON ct = false;
  check(ct.isBool(), "const bool node reports isBool");
  check(!ct.asBool(), "const asBool reads the value");
}

static void testConstReads() {
  std::cout << "[const reads: a const JSON& is fully readable]\n";
  const JSON obj =
      JSON::parse(String(R"({"name":"omega","count":7,"ratio":0.5,"on":true})"));
  check(obj.isMap(), "const map node reports isMap");
  check(obj.size() == 4, "const size() == 4");
  check(obj.asMap().size() == 4, "const asMap() view size == 4");

  // const operator[] and at() read present keys without mutating.
  check(obj["name"].asString() == "omega", "const operator[] reads a string");
  check(obj.at("count").asInt() == 7, "const at() reads an int");
  check(obj["ratio"].asDouble() == 0.5, "const operator[] reads a real");
  check(obj["on"].asBool(), "const operator[] reads a bool");
}

static void testFindAndContainsDoNotInsert() {
  std::cout << "[find/contains: located when present, never insert]\n";
  JSON obj = JSON::parse(String(R"({"a":"1"})"));
  size_t before = obj.size();

  check(obj.contains("a"), "contains present key");
  check(!obj.contains("missing"), "does not contain absent key");

  JSON *hit = obj.find("a");
  check(hit != nullptr && hit->asString() == "1", "find returns the present value");
  JSON *miss = obj.find("missing");
  check(miss == nullptr, "find returns nullptr for absent key");
  check(obj.size() == before, "find/contains did NOT insert anything");

  // Contrast: the mutating operator[] DOES insert a missing key.
  obj["missing"];
  check(obj.size() == before + 1, "non-const operator[] inserted the missing key");
}

static void testMutateThroughFindAndAt() {
  std::cout << "[find/at: mutate an existing member without inserting]\n";
  JSON obj = JSON::parse(String(R"({"x":"old"})"));

  JSON *x = obj.find("x");
  check(x != nullptr, "find located x");
  *x = JSON("viaFind");
  check(obj.at("x").asString() == "viaFind", "mutation through find* persisted");

  obj.at("x") = JSON("viaAt");
  check(obj.at("x").asString() == "viaAt", "mutation through at() persisted");
  check(obj.size() == 1, "mutations did not grow the map");
}

static void testSizeAndEmpty() {
  std::cout << "[size/empty: maps and arrays]\n";
  JSON arr = JSON::parse(String("[1,2,3]"));
  check(arr.size() == 3, "array size == 3");
  check(!arr.empty(), "non-empty array is not empty");

  JSON emptyArr = JSON::parse(String("[]"));
  check(emptyArr.size() == 0 && emptyArr.empty(), "empty array reports empty");

  JSON emptyObj = JSON::parse(String("{}"));
  check(emptyObj.size() == 0 && emptyObj.empty(), "empty object reports empty");
}

int main() {
  testNull();
  testBool();
  testConstReads();
  testFindAndContainsDoNotInsert();
  testMutateThroughFindAndAt();
  testSizeAndEmpty();

  if (g_failures == 0) {
    std::cout << "\nJSONLookupTest: ALL CHECKS PASSED\n";
    return 0;
  }
  std::cerr << "\nJSONLookupTest: " << g_failures << " CHECK(S) FAILED\n";
  return 1;
}
