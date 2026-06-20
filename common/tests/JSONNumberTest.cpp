// JSONNumberTest — verification for JSON API Completion Plan, Phase 2 (number
// model). Covers the tagged int-or-double representation: the int/long long/
// double constructors, isInt/isReal, asInt/asDouble/asFloat, the parse ->
// serialize round-trip with exact text, and the fix for the pre-Phase-2 footgun
// where an integer literal silently became a boolean.

#include "omega-common/json.h"

#include <cmath>
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

// Parse `src`, then assert it serializes back to `expected` byte-for-byte.
static void roundTrip(const char *src, const char *expected) {
  JSON parsed = JSON::parse(String(src));
  String out = JSON::serialize(parsed);
  if (out == expected) {
    std::cout << "  ok: round-trip " << src << " -> " << expected << "\n";
  } else {
    std::cerr << "  FAIL: round-trip " << src << " produced [" << out
              << "], expected [" << expected << "]\n";
    ++g_failures;
  }
}

static void testRoundTripExactText() {
  std::cout << "[round-trip: exact text for the plan's cases]\n";
  roundTrip("3", "3");
  roundTrip("2147483648", "2147483648"); // > INT_MAX: int-only storage truncated
  roundTrip("3.14", "3.14");
  roundTrip("-0.0", "-0.0");
  roundTrip("1e308", "1e308");
}

static void testIntegerTagAndValue() {
  std::cout << "[integers: tag + value preserved losslessly]\n";
  JSON small = JSON::parse(String("3"));
  check(small.isNumber() && small.isInt() && !small.isReal(), "3 is an integer");
  check(small.asInt() == 3, "3 asInt == 3");

  // 2^31 overflows the old `int` arm; the tagged long long arm holds it exactly.
  JSON big = JSON::parse(String("2147483648"));
  check(big.isInt(), "2147483648 is an integer");
  check(big.asInt() == 2147483648LL, "2147483648 asInt exact");
}

static void testRealTagAndValue() {
  std::cout << "[reals: tag + value + sign preserved]\n";
  JSON pi = JSON::parse(String("3.14"));
  check(pi.isNumber() && pi.isReal() && !pi.isInt(), "3.14 is a real");
  check(pi.asDouble() == 3.14, "3.14 asDouble exact");
  check(pi.asInt() == 3, "3.14 asInt truncates to 3");

  JSON negZero = JSON::parse(String("-0.0"));
  check(negZero.isReal(), "-0.0 is a real");
  check(std::signbit(negZero.asDouble()), "-0.0 keeps its negative sign");
}

static void testConstructors() {
  std::cout << "[constructors: int / long long / double]\n";
  JSON i = 42;                  // int constructor
  check(i.isNumber() && i.isInt(), "JSON(42) is an integer number");
  check(i.asInt() == 42, "JSON(42) asInt == 42");

  JSON big = 5000000000LL;      // long long constructor
  check(big.isInt() && big.asInt() == 5000000000LL, "JSON(5e9 ll) exact");
  check(JSON::serialize(big) == "5000000000", "JSON(5e9 ll) serializes exact");

  JSON d = 2.5;                 // double constructor
  check(d.isNumber() && d.isReal(), "JSON(2.5) is a real number");
  check(d.asDouble() == 2.5, "JSON(2.5) asDouble == 2.5");
  check(JSON::serialize(d) == "2.5", "JSON(2.5) serializes as a real");
}

static void testIntegerLiteralIsNotBoolean() {
  std::cout << "[footgun fix: integer literal is a number, not a boolean]\n";
  JSON n = 42;
  check(n.isNumber(), "JSON(42) selects the number ctor, not JSON(bool)");
  check(!n.isString() && !n.isArray() && !n.isMap(), "JSON(42) is purely a number");

  JSON t = true;
  check(!t.isNumber(), "JSON(true) still selects the boolean ctor");
  check(t.asBool(), "JSON(true) asBool is true");
}

static void testFloatNarrowsDouble() {
  std::cout << "[asFloat narrows asDouble]\n";
  JSON d = JSON::parse(String("3.5"));
  check(d.asFloat() == 3.5f, "3.5 asFloat == 3.5f");
  JSON i = JSON::parse(String("7"));
  check(i.asFloat() == 7.0f, "integer 7 asFloat == 7.0f");
}

int main() {
  testRoundTripExactText();
  testIntegerTagAndValue();
  testRealTagAndValue();
  testConstructors();
  testIntegerLiteralIsNotBoolean();
  testFloatNarrowsDouble();

  if (g_failures == 0) {
    std::cout << "\nJSONNumberTest: ALL CHECKS PASSED\n";
    return 0;
  }
  std::cerr << "\nJSONNumberTest: " << g_failures << " CHECK(S) FAILED\n";
  return 1;
}
