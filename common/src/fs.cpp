 #include "omega-common/fs.h"
 #include <cctype>
 #include "omega-common/utils.h"


 namespace OmegaCommon::FS {

    struct Token {
        typedef enum : int {
            ID,
            Dot,
            Slash,
        } Type;
        Type type;
        String str;
    };
 
    void Path::parse(const String & str){
        std::istringstream is(_str);

        Vector<Token> tokens;

        // Core::Regex regex(R"(([\w|_|\/|\.]*)\/(\w+)(?:\.(\w+))?$)");

        unsigned idx = 0;
        
        char buffer[200];
        char *buffer_ptr = buffer;
        char *buf_start = buffer_ptr;
        
        auto get_char = [&](){
            return (char)is.get();
        };
        
        auto ahead_char = [&]() -> char{
            char ch = is.get();
            is.seekg(-1,std::ios::cur);
            if(ch == -1){
                return '\0';
            };
            return ch;
        };
        
        auto clear_buffer = [&](Token::Type ty){
            auto len = buffer_ptr - buf_start;
            tokens.push_back({ty,String(buffer,len)});
            buffer_ptr = buffer;
        };
        
        auto isAlnumAndOther = [&](char c){
            return isalnum(c) ||  (c == '-') || (c == '_') || (c == ' ');
        };
        
        char c;
        /// A Boolean to decide whether to continue!
        bool cont = true;
        while((c = get_char()) != -1){
            switch (c) {
//                case '\0' : {
//                    cont = false;
//                    break;
//                }
                case '/' : {
                    *buffer_ptr = c;
                    ++buffer_ptr;
                    clear_buffer(Token::Slash);
                    break;
                };
                case '\\' : {
                    *buffer_ptr = c;
                    ++buffer_ptr;
                    clear_buffer(Token::Slash);
                    break;
                };
                case '.' : {
                    *buffer_ptr = c;
                    ++buffer_ptr;
                    clear_buffer(Token::Dot);
                    break;
                }
//                case ' ' : {
//                    *buffer_ptr = c;
//                    ++buffer_ptr;
//                    break;
//                }
                default : {
                    if(isAlnumAndOther(c)){
                        *buffer_ptr = c;
                        ++buffer_ptr;
                        if(!isAlnumAndOther(ahead_char())){
                            clear_buffer(Token::ID);
                        };
                    };
                    break;
                }
            };
            ++idx;
        };
        

        auto it = tokens.rbegin();
        _dir = "";
        while(it != tokens.rend()){
            auto & tok = *it;
            if(tok.type == Token::ID && it == tokens.rbegin()){
                ++it;
                auto & tok2 = *it;
                if(tok2.type == Token::Dot){
                    _ext = tok.str;
                }
                else {
                    _fname = tok.str;
                    ++it;
                    continue;
                };
                ++it;
                tok2 = *it;
                if(tok2.type == Token::ID){
                    _fname = tok2.str;
                }
                ++it;
                
            }
            else {
                _dir = tok.str + _dir;
            };
            ++it;
        };

        isRelative = tokens.front().type == Token::Dot || tokens.front().type == Token::ID;
           
        
    };

    String & Path::dir(){
        return _dir;
    };

    String & Path::ext(){
        return _ext;
    };

    String & Path::filename(){
        return _fname;
    };

    String & Path::str(){
        return _str;
    };

    Path & Path::concat(const char *str){
        _str += str;
        parse(_str);
        return *this;
    }

    Path & Path::concat(const String & str){
        _str += str;
        parse(_str);
        return *this;
    }

    Path & Path::concat(const StrRef & str){
        _str += str.data();
        parse(_str);
        return *this;
    }


    Path & Path::append(const char *str){
        _str = _str + PATH_SLASH + str;
        parse(_str);
        return *this;
    };

    Path & Path::append(const String & str){
        _str = _str + PATH_SLASH + str;
        parse(_str);
        return *this;
    };

    Path & Path::append(const StrRef & str){
        _str = _str + PATH_SLASH + str.data();
        parse(_str);
        return *this;
    };

    Path::Path(const char *str):_str(str){
        parse(str);
    };

    Path::Path(StrRef & str):_str(str.data()){
        parse(str.data());
    };

    Path::Path(const String & str):_str(str){
        parse(str);
    };

    

    Path Path::operator+(const char *str){
        return _str + PATH_SLASH + str;
    };

    Path Path::operator+(const String & str){
        return _str + PATH_SLASH + str;
    };

    Path Path::operator+(const StrRef & str){
        return _str + PATH_SLASH + str;
    };

    bool & DirectoryIterator::end(){
        return _end;
    };

 };