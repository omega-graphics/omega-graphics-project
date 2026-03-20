# Debuggable Class Formatting Proposal

## Problem

Classes marked with `OMEGACOMMON_CLASS("...")` currently format as just their class ID:

```
<OmegaWTK.Composition.Brush>
```

This is enough to identify the type, but useless for debugging. At 3am when a render pass fails, you want to see:

```
OmegaWTK.Composition.Brush { isColor: true, isGradient: false, color: rgba(1.0, 0.5, 0.0, 1.0) }
```

The infrastructure to get there is mostly in place. `FormatProvider<T>` already detects `OMEGACOMMON_CLASS_ID` via SFINAE. The Formatter already handles `@{0}` placeholders. What's missing is a way for classes to describe their fields, and a way for the formatter to call that description.

## Design

### Core Idea: `FieldWriter` + `describeFields`

Each class that wants debug-printable formatting defines a `describeFields` method. The macro expansion wires it into the `FormatProvider` automatically.

A lightweight `FieldWriter` object is passed to `describeFields`. It collects name/value pairs and writes them to the output stream.

### API Surface

#### 1. FieldWriter (new class in format.h)

```cpp
namespace OmegaCommon {

class FieldWriter {
    std::ostream &os;
    unsigned count = 0;
public:
    explicit FieldWriter(std::ostream &os) : os(os) {}

    // Write a named field. Uses FormatProvider<T> for the value.
    template<typename T>
    FieldWriter & field(const char *name, const T &value) {
        if (count > 0) os << ", ";
        os << name << ": ";
        // Reuse existing FormatProvider infrastructure
        auto copy = value;
        FormatProvider<std::decay_t<T>>::format(os, copy);
        ++count;
        return *this;
    }

    // Write a raw string field (no FormatProvider lookup).
    FieldWriter & field(const char *name, const char *value) {
        if (count > 0) os << ", ";
        os << name << ": " << value;
        ++count;
        return *this;
    }

    unsigned fieldCount() const { return count; }
};

} // namespace OmegaCommon
```

`field()` returns `*this` so calls chain:
```cpp
w.field("isColor", isColor).field("width", width).field("name", name);
```

#### 2. Extended OMEGACOMMON_CLASS macro

The existing macro stays as-is for backward compatibility. A new macro opts into field-level formatting:

```cpp
// Existing (unchanged) -- class ID only, formats as <ClassName>
#define OMEGACOMMON_CLASS(id) \
    static constexpr char OMEGACOMMON_CLASS_ID[] = id;

// New -- class ID + field-level debug formatting
#define OMEGACOMMON_CLASS_DEBUGGABLE(id) \
    static constexpr char OMEGACOMMON_CLASS_ID[] = id; \
    static constexpr bool OMEGACOMMON_DEBUGGABLE = true; \
    void describeFields(OmegaCommon::FieldWriter &w) const
```

Usage in a class:

```cpp
struct Brush {
    OMEGACOMMON_CLASS_DEBUGGABLE("OmegaWTK.Composition.Brush") {
        w.field("isColor", isColor)
         .field("isGradient", isGradient);
        if (isColor) {
            w.field("color.r", color.r)
             .field("color.g", color.g)
             .field("color.b", color.b)
             .field("color.a", color.a);
        }
    }

    bool isColor;
    bool isGradient;
    // ...
};
```

The macro ends with `void describeFields(OmegaCommon::FieldWriter &w) const` -- an incomplete function signature. The `{ ... }` block that follows in user code becomes the function body. This is the same pattern used by test frameworks (e.g., `TEST_CASE("name") { body }`).

#### 3. FormatProvider integration

The primary `FormatProvider<T>` template gains a second SFINAE path:

```cpp
// Primary template -- handles OMEGACOMMON_CLASS types
template<typename T>
struct FormatProvider {
    // SFINAE: only enabled if T has OMEGACOMMON_CLASS_ID

    static void format(std::ostream &os, T &object) {
        if constexpr (detail::has_describe_fields_v<T>) {
            // Full debug output
            os << object.OMEGACOMMON_CLASS_ID << " { ";
            FieldWriter w(os);
            object.describeFields(w);
            os << " }";
        } else {
            // ID-only fallback (current behavior)
            os << "<" << T::OMEGACOMMON_CLASS_ID << ">";
        }
    }
};
```

Where the detection trait is:

```cpp
namespace detail {
    template<typename T, typename = void>
    struct has_describe_fields : std::false_type {};

    template<typename T>
    struct has_describe_fields<T,
        std::void_t<decltype(std::declval<const T&>().describeFields(
            std::declval<FieldWriter&>()))>
    > : std::true_type {};

    template<typename T>
    inline constexpr bool has_describe_fields_v = has_describe_fields<T>::value;
}
```

**Backward compatible**: classes using `OMEGACOMMON_CLASS(...)` without `describeFields` continue to print `<ClassName>` exactly as before.

#### 4. FormatProvider specialization for bool (needed by field output)

```cpp
template<>
struct FormatProvider<bool> {
    static void format(std::ostream &os, bool &object) {
        os << (object ? "true" : "false");
    }
};
```

#### 5. FormatProvider specialization for float/double

```cpp
template<>
struct FormatProvider<float> {
    static void format(std::ostream &os, float &object) {
        os << object;
    }
};

template<>
struct FormatProvider<double> {
    static void format(std::ostream &os, double &object) {
        os << object;
    }
};
```

### Example Output

Given:
```cpp
struct GERenderTarget::RenderPassDesc::ColorAttachment {
    OMEGACOMMON_CLASS_DEBUGGABLE("OmegaGTE.ColorAttachment") {
        const char *actions[] = {"Load","LoadPreserve","Clear","Discard"};
        w.field("loadAction", actions[loadAction])
         .field("clearColor.r", clearColor.r)
         .field("clearColor.g", clearColor.g)
         .field("clearColor.b", clearColor.b)
         .field("clearColor.a", clearColor.a);
    }
    // ...
};
```

`fmtString("RenderPass attachment: @0", attachment)` produces:

```
RenderPass attachment: OmegaGTE.ColorAttachment { loadAction: Clear, clearColor.r: 0.0, clearColor.g: 0.0, clearColor.b: 0.0, clearColor.a: 1.0 }
```

### Debug Logging Helper

A convenience function for quick debug dumps, no format string needed:

```cpp
template<typename T>
void DebugDump(const char *label, const T &object) {
    std::ostringstream oss;
    auto copy = object;
    FormatProvider<std::decay_t<T>>::format(oss, copy);
    std::cout << "[DEBUG] " << label << " = " << oss.str() << std::endl;
}
```

Usage:
```cpp
DebugDump("current brush", brush);
// [DEBUG] current brush = OmegaWTK.Composition.Brush { isColor: true, isGradient: false }
```

### SharedHandle integration

The existing `FormatProvider<std::shared_ptr<T>>` already chains into `FormatProvider<T>::format`. So `fmtString("@0", brushPtr)` would produce:

```
SharedHandle(0x7f8a...) : OmegaWTK.Composition.Brush { isColor: true, isGradient: false }
```

No changes needed -- it just works.

## Migration Path

1. **Phase 0**: Add `FieldWriter`, the detection trait, `if constexpr` branch in primary `FormatProvider`, and the `OMEGACOMMON_CLASS_DEBUGGABLE` macro. Add `bool`/`float`/`double` FormatProvider specializations. Zero impact on existing code.

2. **Phase 1**: Convert high-value debugging targets -- classes you actually print during development. Start with:
   - `Brush` (simple struct, union-based -- good test of conditional field output)
   - `GECommandBuffer` (central to GPU debugging)
   - `Layer` (compositor debugging)

3. **Phase 2**: Convert remaining classes at your discretion. Classes that are never printed in logs can stay with `OMEGACOMMON_CLASS`.

### What changes per class

Replace:
```cpp
OMEGACOMMON_CLASS("Some.Class.Name")
```

With:
```cpp
OMEGACOMMON_CLASS_DEBUGGABLE("Some.Class.Name") {
    w.field("fieldA", fieldA)
     .field("fieldB", fieldB);
}
```

One line becomes 3-4 lines. No other changes to the class.

## Files Touched

| File | Change |
|------|--------|
| `common/include/omega-common/utils.h` | Add `OMEGACOMMON_CLASS_DEBUGGABLE` macro |
| `common/include/omega-common/format.h` | Add `FieldWriter`, detection trait, `if constexpr` branch, `bool`/`float`/`double` specializations, `DebugDump` |
| Per-class headers (opt-in) | Replace `OMEGACOMMON_CLASS` with `OMEGACOMMON_CLASS_DEBUGGABLE` + field body |

## Open Questions

1. **Verbosity levels** -- Should `FieldWriter` support a depth/verbosity parameter so nested objects can be printed in summary vs. full mode? (e.g., a `Layer` printing its sublayers recursively could get very long.) I'd start without this and add it when it's actually needed.

2. **Const correctness** -- The current `FormatProvider::format` takes `T&` (non-const). `describeFields` is `const`. The `field()` method makes a copy for the `FormatProvider` call. An alternative is changing `FormatProvider::format` signatures to take `const T&` across the board, but that's a larger refactor. The copy approach works for now since field values are typically small scalars/strings.

3. **Thread safety** -- `FieldWriter` writes directly to the stream. If `LogV` is called from multiple threads, the output can interleave. This is an existing problem with `LogV`, not introduced by this proposal. A per-call `ostringstream` buffer (which `fmtString` already uses) avoids it for formatted strings.
