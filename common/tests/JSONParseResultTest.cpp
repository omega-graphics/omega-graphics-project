// JSONParseResultTest — verification for JSON API Completion Plan, Phase 4
// (error-returning parse). Covers JSON::TryParse returning Result<JSON,String>
// for both the String and istream overloads: the ok path yields the parsed
// value, and the error path yields a RapidJSON message + byte offset WITHOUT
// aborting the process (the pre-Phase-4 parser called std::exit(1) on malformed
// input). The mere fact that this program runs to completion past several
// malformed inputs is the "process stays alive" proof.

#include "omega-common/json.h"

#include <iostream>
#include <sstream>
#include <string>

using OmegaCommon::JSON;
using OmegaCommon::Result;
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

static void testValidStringOk() {
  std::cout << "[TryParse(String): ok path]\n";
  Result<JSON, String> r = JSON::TryParse(String(R"({"k":"v","n":7})"));
  check(r.isOk(), "valid object parses ok");
  check(!r.isErr(), "ok result is not an error");
  check(r.value().isMap(), "parsed value is a map");
  check(r.value()["k"].asString() == "v", "string member is correct");
  check(r.value()["n"].asInt() == 7, "number member is correct");
}

static void testMalformedStringErr() {
  std::cout << "[TryParse(String): error path, no process exit]\n";
  Result<JSON, String> r = JSON::TryParse(String("{ broken "));
  check(r.isErr(), "malformed input yields an error");
  check(!r.isOk(), "error result is not ok");
  check(!r.error().empty(), "error message is non-empty");
  check(r.error().find("offset") != String::npos,
        "error message reports a byte offset");
  std::cout << "    (message: " << r.error() << ")\n";

  // A second, differently-broken input — reaching here at all proves the first
  // malformed parse did not terminate the process.
  Result<JSON, String> r2 = JSON::TryParse(String("@@@"));
  check(r2.isErr(), "garbage input yields an error (still alive)");
}

static void testStreamOverload() {
  std::cout << "[TryParse(istream): ok + error]\n";
  std::istringstream good(R"([1,2,3])");
  Result<JSON, String> r = JSON::TryParse(good);
  check(r.isOk() && r.value().size() == 3, "stream of a valid array parses ok");

  std::istringstream bad("[1,2,");
  Result<JSON, String> r2 = JSON::TryParse(bad);
  check(r2.isErr(), "stream of a truncated array yields an error");
}

static void testValueOrFallback() {
  std::cout << "[Result::valueOr: graceful fallback on error]\n";
  JSON fallback = JSON::TryParse(String("not-json")).valueOr(JSON(nullptr));
  check(fallback.isNull(), "valueOr returns the fallback when parse fails");

  JSON parsed = JSON::TryParse(String(R"("hi")")).valueOr(JSON(nullptr));
  check(parsed.isString() && parsed.asString() == "hi",
        "valueOr returns the parsed value when parse succeeds");
}

static void testParseWrapperStillWorks() {
  std::cout << "[parse(): legacy wrapper still parses valid input]\n";
  JSON j = JSON::parse(String(R"({"a":"b"})"));
  check(j.isMap() && j["a"].asString() == "b", "parse() returns the value");
}

int main() {
  testValidStringOk();
  testMalformedStringErr();
  testStreamOverload();
  testValueOrFallback();
  testParseWrapperStillWorks();

  if (g_failures == 0) {
    std::cout << "\nJSONParseResultTest: ALL CHECKS PASSED\n";
    return 0;
  }
  std::cerr << "\nJSONParseResultTest: " << g_failures << " CHECK(S) FAILED\n";
  return 1;
}
