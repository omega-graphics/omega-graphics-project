#include "utils.h"
#include <cstddef>
#include <initializer_list>
#include <istream>
#include <sstream>
#include <type_traits>

#ifndef OMEGA_COMMON_JSON_H
#define OMEGA_COMMON_JSON_H

namespace OmegaCommon {

    /**
     @brief A simple class for parsing and serializing JSON, and for defining all of the JSON structures and data types.
     (Represents a single JSON object node in a JSON tree.)
     @paragraph 
     An instance of this class represents a single JSON Object Node in the JSON object tree.
    */
    class OMEGACOMMON_EXPORT JSON {

        enum : int {
            STRING,
            ARRAY,
            MAP,
            NUMBER,
            BOOLEAN,
            NUL,
            UNKNOWN
        } type = UNKNOWN;

        typedef char *JString;

        typedef JSON *JArray;

        /// A JSON number, tagged as either an integer or a real so that both
        /// kinds round-trip losslessly. `isReal` selects which member is
        /// authoritative: `d` for reals, `i` for integers. Trivially copyable,
        /// so it lives in the Data union with no special destruction.
        struct Num {
            bool isReal;
            long long i;
            double d;
        };

        union Data {
            JString str;
            bool b;
            OmegaCommon::Vector<JSON> *array;
            OmegaCommon::Map<String,JSON> *map;
            Num number;
            Data() = default;
            Data(decltype(type) t);
            Data(bool & b);
            Data(OmegaCommon::StrRef str);
            Data(OmegaCommon::ArrayRef<JSON> array);
            Data(OmegaCommon::MapRef<String,JSON> map);
            void _destroy(decltype(type) t);
        } data;

        
        friend class JSONParser;
        friend class JSONSerializer;
        friend class JSONReader;
        friend class RapidJSONBridge;
    public:
        typedef std::remove_pointer_t<decltype(data.map)>::iterator map_iterator;

        typedef std::remove_pointer_t<decltype(data.array)>::iterator array_iterator;

        JSON() = default;

        /// @name Special members
        /// Deep-copy value semantics. A JSON node owns its String, Array, and Map
        /// storage on the heap; copying clones that storage so two nodes never
        /// alias the same buffer, and moving transfers ownership and leaves the
        /// source as an empty (UNKNOWN) node whose destructor is a no-op.
        /// @{
        JSON(const JSON & other);
        JSON(JSON && other) noexcept;
        JSON & operator=(const JSON & other);
        JSON & operator=(JSON && other) noexcept;
        /// @}

        /// Construct JSON as String
        JSON(const char *c_str);

         /// Construct JSON as String
        JSON(const String & str);

        /// Construct JSON as Boolean
        JSON(bool b);

        /// Construct JSON as an explicit null (distinct from the default,
        /// uninitialized node).
        JSON(std::nullptr_t);

        /// Construct JSON as Number (stored as an integer)
        JSON(int v);

        /// Construct JSON as Number (stored as an integer)
        JSON(long long v);

        /// Construct JSON as Number (stored as a real)
        JSON(double v);

        /// Construct JSON as Array
        JSON(std::initializer_list<JSON> array);
        
        /// Construct JSON as Map
        JSON(std::map<String,JSON> map);

        /// Make an empty Map node, ready to be filled via operator[]/insert.
        static JSON Object();

        /// Make an empty Array node, ready to be filled via push_back.
        static JSON Array();

        OMEGA_NODISCARD bool isString() const;

        OMEGA_NODISCARD bool isArray() const;

        OMEGA_NODISCARD bool isNumber() const;

        /// True when this is a number stored as an integer.
        OMEGA_NODISCARD bool isInt() const;

        /// True when this is a number stored as a real (floating-point).
        OMEGA_NODISCARD bool isReal() const;

        OMEGA_NODISCARD bool isMap() const;

        /// True when this is an explicit null node.
        OMEGA_NODISCARD bool isNull() const;

        /// True when this is a boolean node.
        OMEGA_NODISCARD bool isBool() const;

        /// Get this JSON node as a String.
        OMEGA_NODISCARD OmegaCommon::StrRef asString() const;

        /// Get this JSON node as a Vector
        /// (From a JSON Array).
        OMEGA_NODISCARD ArrayRef<JSON> asVector() const;

        /// Get this JSON node as a Map.
        OMEGA_NODISCARD MapRef<String,JSON> asMap() const;

        /// Get this number as a 64-bit integer (truncates a real value).
        OMEGA_NODISCARD long long asInt() const;

        /// Get this number as a double (widens an integer value).
        OMEGA_NODISCARD double asDouble() const;

        /// Get this number as a float (narrows asDouble()).
        OMEGA_NODISCARD float asFloat() const;

        /// Get a mutable reference to this boolean node.
        bool & asBool();

        /// Get this boolean node's value (const).
        OMEGA_NODISCARD bool asBool() const;

        // /// @name Mod Methods 
        // /// @{

        JSON & operator[](OmegaCommon::StrRef str);

        /// Read-only map lookup. Asserts the key is present (never inserts).
        const JSON & operator[](OmegaCommon::StrRef str) const;

        map_iterator insert(const std::pair<String,JSON> & p);

        void push_back(const JSON & j);

        /// @}

        /// @name Lookup
        /// Non-mutating queries over a Map or Array node.
        /// @{

        /// True when this Map node holds `key`.
        OMEGA_NODISCARD bool contains(OmegaCommon::StrRef key) const;

        /// Find a Map member by key, or nullptr when absent. Never inserts.
        JSON * find(OmegaCommon::StrRef key);

        OMEGA_NODISCARD const JSON * find(OmegaCommon::StrRef key) const;

        /// Access a Map member by key. Asserts the key is present (never inserts).
        JSON & at(OmegaCommon::StrRef key);

        OMEGA_NODISCARD const JSON & at(OmegaCommon::StrRef key) const;

        /// Number of members (Map) or elements (Array).
        OMEGA_NODISCARD size_t size() const;

        /// True when this Map/Array node holds nothing.
        OMEGA_NODISCARD bool empty() const;

        /// @}


        

        /// Parse JSON text, returning the parsed value or an error message
        /// (RapidJSON description + byte offset). Never aborts the process —
        /// prefer this anywhere the input may be malformed.
        static Result<JSON,String> TryParse(OmegaCommon::StrRef source);

        static Result<JSON,String> TryParse(std::istream & in);

        /// Parse JSON text. Asserts on malformed input (and yields an empty node
        /// in release builds); use TryParse to handle parse errors gracefully.
        static JSON parse(String str);

        static JSON parse(std::istream & in);

        /// Serialize to a String. `pretty` (default) indents and adds newlines;
        /// pass false for compact, whitespace-free output (e.g. wire payloads).
        static String serialize(JSON & json, bool pretty = true);

        static void serialize(JSON & json,std::ostream & out, bool pretty = true);

        /// @name JSONConvertible bridge
        /// Connect the JSONConvertible interface (see the IJSONConvertible macro):
        /// build a JSON tree from any value with a `toJSON(JSON&)` method, or
        /// populate any value with a `fromJSON(JSON&)` method from this node.
        /// @{

        /// Build a JSON tree from `v` by calling `v.toJSON(...)`.
        template<class T>
        static JSON From(T & v) {
            JSON j;
            v.toJSON(j);
            return j;
        }

        /// Populate `v` from this node by calling `v.fromJSON(*this)`.
        template<class T>
        void into(T & v) {
            v.fromJSON(*this);
        }

        /// @}

        /// Releases this node's owned String/Array/Map storage (recursively, via
        /// the owned containers' own destructors). A no-op for Number, Boolean,
        /// and the default UNKNOWN node.
        ~JSON();
    };

    OMEGACOMMON_EXPORT std::istream & operator>>(std::istream & in,JSON & json);
    OMEGACOMMON_EXPORT std::ostream & operator<<(std::ostream & out,JSON & json);

    typedef Map<String,JSON> JSONMap;
    typedef Vector<JSON> JSONArray;

    struct OMEGACOMMON_EXPORT JSONConvertible {
        virtual void toJSON(JSON & j) = 0;
        virtual void fromJSON(JSON & j) = 0;
    };

    #define IJSONConvertible public ::OmegaCommon::JSONConvertible


    

};




#endif
