// JSONConvertTest — verification for JSON API Completion Plan, Phase 5
// (serialize options + JSONConvertible bridge + empty-container factories).
// Covers compact-vs-pretty serialization, JSON::Object()/Array() builders, and a
// round-trip through JSON::From / JSON::into using a real IJSONConvertible type.

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

// A small value type that opts into the JSONConvertible interface.
struct Point : IJSONConvertible {
  int x = 0;
  int y = 0;

  void toJSON(JSON &j) override {
    j = JSON::Object();
    j["x"] = JSON(x);
    j["y"] = JSON(y);
  }

  void fromJSON(JSON &j) override {
    x = static_cast<int>(j["x"].asInt());
    y = static_cast<int>(j["y"].asInt());
  }
};

static void testCompactVsPretty() {
  std::cout << "[serialize: compact vs pretty]\n";
  JSON obj = JSON::parse(String(R"({"a":"b"})"));

  String pretty = JSON::serialize(obj, true);
  String compact = JSON::serialize(obj, false);

  check(compact == R"({"a":"b"})", "compact output is whitespace-free");
  check(compact.find('\n') == String::npos, "compact output has no newlines");
  check(pretty.find('\n') != String::npos, "pretty output has newlines");
  check(JSON::serialize(obj) == pretty, "default serialize equals pretty");
}

static void testObjectFactory() {
  std::cout << "[Object(): empty, buildable map]\n";
  JSON o = JSON::Object();
  check(o.isMap() && o.empty(), "Object() is an empty map");

  o["k"] = JSON("v");
  o["n"] = JSON(42);
  check(o.size() == 2, "Object() grew via operator[]");
  check(o["k"].asString() == "v" && o["n"].asInt() == 42, "built members read back");
  check(JSON::serialize(o, false) == R"({"k":"v","n":42})", "built object serializes compact");
}

static void testArrayFactory() {
  std::cout << "[Array(): empty, buildable array]\n";
  JSON a = JSON::Array();
  check(a.isArray() && a.empty(), "Array() is an empty array");

  a.push_back(JSON(1));
  a.push_back(JSON(2));
  a.push_back(JSON(3));
  check(a.size() == 3, "Array() grew via push_back");
  check(JSON::serialize(a, false) == "[1,2,3]", "built array serializes compact");
}

static void testConvertibleRoundTrip() {
  std::cout << "[JSONConvertible: From / into round-trip]\n";
  Point p;
  p.x = 3;
  p.y = 4;

  JSON j = JSON::From(p);
  check(j.isMap(), "From(p) built a map");
  check(j["x"].asInt() == 3 && j["y"].asInt() == 4, "From(p) carried the fields");

  Point q;
  j.into(q);
  check(q.x == 3 && q.y == 4, "into(q) populated the struct");

  // And it survives a compact serialize + reparse.
  JSON reparsed = JSON::parse(JSON::serialize(j, false));
  Point r;
  reparsed.into(r);
  check(r.x == 3 && r.y == 4, "convertible survives serialize -> parse -> into");
}

int main() {
  testCompactVsPretty();
  testObjectFactory();
  testArrayFactory();
  testConvertibleRoundTrip();

  if (g_failures == 0) {
    std::cout << "\nJSONConvertTest: ALL CHECKS PASSED\n";
    return 0;
  }
  std::cerr << "\nJSONConvertTest: " << g_failures << " CHECK(S) FAILED\n";
  return 1;
}
