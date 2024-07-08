#include "omega-common/json.h"
#include <istream>
#include <cctype>
#include <sstream>
#include <iostream>
#include <cassert>


namespace OmegaCommon {
    
    struct JSONTok {
        typedef enum : int {
            StrLiteral,
            LBrace,
            RBrace,
            LBracket,
            RBracket,
            Comma,
            Colon,
            BoolLiteral,
            NumLiteral,
            Eof
        } TokType;
        TokType type;
        String str;
    };

    class JSONLexer {
        std::istream * in = nullptr;
    public:
        void setInputStream(std::istream * in){
            this->in = in;
        };
        
        void lexToBuffer(char *bufferStart,char *bufferEnd,size_t * len,JSONTok::TokType * type){
            
            auto getChar = [&]() -> char{
                return in->get();
            };

            auto aheadChar = [&]() -> char {
                auto ch = in->get();
                in->seekg(-1,std::ios::cur);
                return ch;
            };

            #define PUSH_CHAR(c) *bufferEnd = c;++bufferEnd;
            #define PUSH_TOK(t) *type = t;*len = bufferEnd - bufferStart;return;
            char c;
            while((c = getChar()) != -1){
                switch(c){
                    case '\0':{
                        PUSH_CHAR(c)
                        PUSH_TOK(JSONTok::Eof)
                        break;
                    }
                    case '"':{
                        PUSH_CHAR(c)
                        while((c = getChar()) != '"'){
                            PUSH_CHAR(c)
                        };
                        PUSH_CHAR(c)
                        PUSH_TOK(JSONTok::StrLiteral)
                        break;
                    }
                    case ':' : {
                        PUSH_CHAR(c)
                        PUSH_TOK(JSONTok::Colon)
                        break;
                    }
                    case ',' : {
                        PUSH_CHAR(c)
                        PUSH_TOK(JSONTok::Comma)
                        break;
                    }
                    case '[' : {
                        PUSH_CHAR(c)
                        PUSH_TOK(JSONTok::LBracket)
                        break;
                    }
                    case ']': {
                        PUSH_CHAR(c)
                        PUSH_TOK(JSONTok::RBracket)
                        break;
                    }
                    case '{': {
                        PUSH_CHAR(c)
                        PUSH_TOK(JSONTok::RBrace)
                        break;
                    }
                    case '}': {
                        PUSH_CHAR(c)
                        PUSH_TOK(JSONTok::LBrace)
                        break;
                    }
                    default:{
                        break;
                    }
                };
            };

            PUSH_TOK(JSONTok::Eof)


        };


        JSONTok nextTok(){
            char buffer[200];
            JSONTok tok;
            size_t len;
            lexToBuffer(buffer,buffer,&len,&tok.type);
            tok.str.resize(len);
            std::move(buffer,buffer + len,tok.str.begin());
            return tok;
        };
        void finish(){
            in = nullptr;
        };
    };

    #define JSON_ERROR(message) std::cerr << "JSON Parse Error:" << message << std::endl;exit(1);

    class JSONParser {
        std::unique_ptr<JSONLexer> lexer;
    public:
        void setInputStream(std::istream * in){
            lexer->setInputStream(in);
        };

        JSON parseToJSON(JSONTok & firstTok){
            JSON j {};
            /// Map
            if(firstTok.type == JSONTok::LBrace){
                firstTok = lexer->nextTok();
                j.type = JSON::MAP;
                j.data = JSON::Data(JSON::MAP);
                j.data.map = new Map<String,JSON>();
                OmegaCommon::Map<String,JSON> & m = *j.data.map;
                while(firstTok.type != JSONTok::RBrace){
                    if(firstTok.type != JSONTok::StrLiteral) {
                        JSON_ERROR("Expected a StrLiteral")
                    }
                    auto key = firstTok.str;
                    if(lexer->nextTok().type != JSONTok::Colon) {
                        JSON_ERROR("Expected a Colon!")
                    }
                    
                    firstTok = lexer->nextTok();
                    auto val = parseToJSON(firstTok);
                    m.insert(std::make_pair(key,val));

                    firstTok = lexer->nextTok();
                    if(firstTok.type != JSONTok::Comma && firstTok.type != JSONTok::RBrace) {
                        JSON_ERROR("Expected Comma!")
                    }
                    if(firstTok.type == JSONTok::Comma) {
                        firstTok = lexer->nextTok();
                    }
                };
            }
            /// Vector
            else if(firstTok.type == JSONTok::LBracket){
                firstTok = lexer->nextTok();
              
                j.type = JSON::ARRAY;
                j.data = JSON::Data(JSON::ARRAY);
                j.data.array = new Vector<JSON>();
                auto & v = *j.data.array;
                while(firstTok.type != JSONTok::RBracket){
                    v.push_back(parseToJSON(firstTok));
                    firstTok = lexer->nextTok();
                    if(firstTok.type != JSONTok::Comma && firstTok.type != JSONTok::RBracket)
                        JSON_ERROR("Expected Comma!")
                    if(firstTok.type == JSONTok::Comma)
                        firstTok = lexer->nextTok();
                };
            }
            /// String
            else if(firstTok.type == JSONTok::StrLiteral){
                j.type = JSON::STRING;
                j.data = JSON::Data(firstTok.str);
            }

            /// Number

            /// Boolean
            return j;
        };

        JSON parse(){
            JSONTok firstTok = lexer->nextTok();
            return parseToJSON(firstTok);
        };

        void finish(){
            lexer->finish();
        };
    } parser;



    class JSONSerializer {
        std::ostream * out;
    public:
        void setOutputStream(std::ostream * out){
            this->out = out;
        };
        void serializeToStream(JSON & j){
            auto & out = *this->out;
            if(j.type == JSON::MAP){
                out << "{";
                auto map = j.asMap();
                for(auto it = map.begin();it != map.end();it++){
                    if(it == map.begin())
                        out << ",";
                    auto & ent = *it;
                    out << "\"" << ent.first << "\":";
                    serializeToStream(const_cast<JSON &>(ent.second));
                    ++it;
                };
                out << "}";
            }
            else if(j.type == JSON::ARRAY){
                out << "[";
                auto vec = j.asVector();
                for(auto it = vec.begin();it != vec.end();it++){
                    if(it == vec.end())
                        out << ",";
                    auto & ent = *it;
                    serializeToStream(const_cast<JSON &>(ent));
                    ++it;
                };
                out << "]";
            }
            else if(j.type == JSON::STRING){
                out << "\"" << j.asString().data() << "\"";
            };
        };
        void serialize(JSON & j){
            serializeToStream(j);
        };
        void finish(){
            out = nullptr;
        };
    } serializer;

    bool JSON::isString() const {
        return type == STRING;
    }

    bool JSON::isArray() const {
        return type == ARRAY;
    }

    bool JSON::isMap() const {
        return type == MAP;
    }

    bool JSON::isNumber() const {
        return type == NUMBER;
    }

    MapRef<String,JSON> JSON::asMap(){
        assert(isMap());
        return *data.map;
    };

    ArrayRef<JSON> JSON::asVector(){
        assert(isArray());
        return *data.array;
    };

    StrRef JSON::asString(){
        assert(isString());
        return {data.str};
    };


    
    JSON JSON::parse(String str){
        std::istringstream in(str);
        parser.setInputStream(&in);
        auto j = parser.parse();
        parser.finish();
        return j;
    };

    JSON JSON::parse(std::istream & in){
        parser.setInputStream(&in);
        auto j = parser.parse();
        parser.finish();
        return j;
    };

    String JSON::serialize(JSON & j){
        std::ostringstream out;
        serialize(j,out);
        return out.str();
    };

    void JSON::serialize(JSON & j,std::ostream & out){
        serializer.setOutputStream(&out);
        serializer.serialize(j);
        serializer.finish();
    };

    JSON::Data::Data(decltype(JSON::type) t) : Data(){
        if(t == ARRAY){
            array = nullptr;
        }
        else {
            map = nullptr;
        }
    }

    JSON::Data::Data(StrRef str) : str(new char [str.size()]){
        std::move(str.begin(),str.end(),this->str);
    }

    JSON::Data::Data(ArrayRef<JSON> array) : array(new Vector<JSON>(array.begin(),array.end())){
        
    }

    JSON::Data::Data(MapRef<String,JSON> map) : map(new Map<String,JSON>(map.begin(),map.end())){

    }

    JSON::Data::Data(bool &b) : b(b){

    }

    void JSON::Data::_destroy(decltype(type) t){
        if(t == MAP){
            delete map;
        }
        else {
            delete array;
        }
    }
    

    JSON::JSON(const char *c_str):type(STRING),data(c_str){

    };

     /// Construct JSON as String
    JSON::JSON(const String & str):type(STRING),data(str){

    };

    JSON::JSON(bool b):type(BOOLEAN),data(b){

    }

    /// Construct JSON as Array
    JSON::JSON(std::initializer_list<JSON> array):type(ARRAY),data(ArrayRef<JSON>{
            const_cast<JSON *>(array.begin()),
            const_cast<JSON *>(array.end())
    }){

    };

    JSON & JSON::operator[](OmegaCommon::StrRef str){
        assert(isMap() && "Cannot insert pair unless object is Map");
        return data.map->operator[](str);
    }
        

    JSON::map_iterator JSON::insert(const std::pair<String,JSON> & j){
        assert(isMap() && "Cannot insert pair unless object is Map");
        return data.map->insert(j).first;
    }

    void JSON::push_back(const JSON & j){
        assert(isArray() && "Cannot push object unless is Array");
        data.array->push_back(j);
    }
    /// Construct JSON as Map
    JSON::JSON(std::map<String,JSON> map):type(MAP),data(map){

    };

    std::istream & operator>>(std::istream & in,JSON & json){
        json = JSON::parse(in);
        return in;
    };

    std::ostream & operator<<(std::ostream & out,JSON & json){
         JSON::serialize(json,out);
        return out;
    };


};