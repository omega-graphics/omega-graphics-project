#include "utils.h"
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
            UNKNOWN
        } type = UNKNOWN;

        typedef char *JString;

        typedef JSON *JArray;

        typedef int JNumber;

        union Data {
            JString str;
            bool b;
            OmegaCommon::Vector<JSON> *array;
            OmegaCommon::Map<String,JSON> *map;
            JNumber number;
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
    public:
        typedef std::remove_pointer_t<decltype(data.map)>::iterator map_iterator;

        typedef std::remove_pointer_t<decltype(data.array)>::iterator array_iterator;

        JSON() = default;

        /// Construct JSON as String
        JSON(const char *c_str);

         /// Construct JSON as String
        JSON(const String & str);

        /// Construct JSON as Boolean
        JSON(bool b);

        /// Construct JSON as Array
        JSON(std::initializer_list<JSON> array);
        
        /// Construct JSON as Map
        JSON(std::map<String,JSON> map);

        bool isString() const;

        bool isArray() const;

        bool isNumber() const;

        bool isMap() const;

        /// Get this JSON node as a String.
        OmegaCommon::StrRef asString();
        
        /// Get this JSON node as a Vector 
        /// (From a JSON Array).
        ArrayRef<JSON> asVector();

        /// Get this JSON node as a Map.
        MapRef<String,JSON> asMap();

        float asFloat();

        bool & asBool();

        // /// @name Mod Methods 
        // /// @{

        JSON & operator[](OmegaCommon::StrRef str);

        map_iterator insert(const std::pair<String,JSON> & p);

        void push_back(const JSON & j);

        /// @}


        

        static JSON parse(String str);

        static JSON parse(std::istream & in);

        static String serialize(JSON & json);

        static void serialize(JSON & json,std::ostream & out);

        ~JSON() = default;
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