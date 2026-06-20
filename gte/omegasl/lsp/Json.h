#ifndef OMEGASL_LSP_JSON_H
#define OMEGASL_LSP_JSON_H

#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <cstdio>

/// Minimal JSON value used to *author* outgoing LSP messages.
///
/// Incoming messages are parsed with the robust RapidJSON-backed
/// `OmegaCommon::JSON`. That type, however, has no public way to construct an
/// integer value (numbers only arrive via parsing), and LSP responses are full
/// of integers — line/character positions, severities, symbol kinds, request
/// ids. This small writer fills that gap: it supports real integers and emits
/// compact, correctly-escaped JSON. It is write-only by design; we never parse
/// with it.
namespace omegasl {
namespace lsp {

    class Json {
    public:
        enum class Type : int { Null, Bool, Int, Double, String, Array, Object };

        Json() : type_(Type::Null) {}

        static Json null() { return Json(); }
        static Json boolean(bool v) { Json j; j.type_ = Type::Bool; j.b_ = v; return j; }
        static Json integer(long long v) { Json j; j.type_ = Type::Int; j.i_ = v; return j; }
        static Json number(double v) { Json j; j.type_ = Type::Double; j.d_ = v; return j; }
        static Json str(std::string v) { Json j; j.type_ = Type::String; j.s_ = std::move(v); return j; }
        static Json array() { Json j; j.type_ = Type::Array; return j; }
        static Json object() { Json j; j.type_ = Type::Object; return j; }

        /// Set `key` on an object value (the value is created as an object if it
        /// was null). Returns `*this` for chaining.
        Json & set(const std::string & key, Json value) {
            if (type_ == Type::Null) {
                type_ = Type::Object;
            }
            obj_.emplace_back(key, std::move(value));
            return *this;
        }

        /// Append to an array value (created as an array if it was null).
        Json & push(Json value) {
            if (type_ == Type::Null) {
                type_ = Type::Array;
            }
            arr_.push_back(std::move(value));
            return *this;
        }

        bool empty() const {
            if (type_ == Type::Array) {
                return arr_.empty();
            }
            if (type_ == Type::Object) {
                return obj_.empty();
            }
            return type_ == Type::Null;
        }

        std::string dump() const {
            std::string out;
            write(out);
            return out;
        }

        void write(std::string & out) const {
            switch (type_) {
                case Type::Null:
                    out += "null";
                    break;
                case Type::Bool:
                    out += b_ ? "true" : "false";
                    break;
                case Type::Int: {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%lld", i_);
                    out += buf;
                    break;
                }
                case Type::Double: {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%g", d_);
                    out += buf;
                    break;
                }
                case Type::String:
                    writeEscaped(s_, out);
                    break;
                case Type::Array: {
                    out += '[';
                    for (size_t i = 0; i < arr_.size(); i++) {
                        if (i != 0) {
                            out += ',';
                        }
                        arr_[i].write(out);
                    }
                    out += ']';
                    break;
                }
                case Type::Object: {
                    out += '{';
                    for (size_t i = 0; i < obj_.size(); i++) {
                        if (i != 0) {
                            out += ',';
                        }
                        writeEscaped(obj_[i].first, out);
                        out += ':';
                        obj_[i].second.write(out);
                    }
                    out += '}';
                    break;
                }
            }
        }

    private:
        /// Escape a string per RFC 8259: quotes, backslash, and the control
        /// characters (with the short forms where they exist, `\uXXXX`
        /// otherwise). UTF-8 bytes >= 0x20 pass through unchanged — they are
        /// valid JSON and every LSP client accepts raw UTF-8.
        static void writeEscaped(const std::string & s, std::string & out) {
            out += '"';
            for (char ch : s) {
                unsigned char c = (unsigned char)ch;
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                            out += buf;
                        } else {
                            out += (char)c;
                        }
                        break;
                }
            }
            out += '"';
        }

        Type type_;
        bool b_ = false;
        long long i_ = 0;
        double d_ = 0.0;
        std::string s_;
        std::vector<Json> arr_;
        std::vector<std::pair<std::string, Json>> obj_;
    };

} // namespace lsp
} // namespace omegasl

#endif // OMEGASL_LSP_JSON_H
