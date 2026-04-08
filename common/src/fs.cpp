 #include "omega-common/fs.h"
 #include <cctype>
 #include <fstream>
 #include <cstdio>
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

    // -- File I/O helpers (cross-platform via std::fstream) --

    Result<String, StatusCode> readFile(Path path){
        std::ifstream in(path.str(), std::ios::in);
        if(!in.is_open()){
            return Result<String, StatusCode>::err(Failed);
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        if(in.bad()){
            return Result<String, StatusCode>::err(Failed);
        }
        return Result<String, StatusCode>::ok(buf.str());
    }

    Result<Vector<std::uint8_t>, StatusCode> readBinaryFile(Path path){
        std::ifstream in(path.str(), std::ios::in | std::ios::binary | std::ios::ate);
        if(!in.is_open()){
            return Result<Vector<std::uint8_t>, StatusCode>::err(Failed);
        }
        auto size = in.tellg();
        in.seekg(0, std::ios::beg);
        Vector<std::uint8_t> data(static_cast<size_t>(size));
        in.read(reinterpret_cast<char *>(data.data()), size);
        if(in.bad()){
            return Result<Vector<std::uint8_t>, StatusCode>::err(Failed);
        }
        return Result<Vector<std::uint8_t>, StatusCode>::ok(std::move(data));
    }

    StatusCode writeFile(Path path, StrRef contents){
        std::ofstream out(path.str(), std::ios::out | std::ios::trunc);
        if(!out.is_open()){
            return Failed;
        }
        out.write(contents.data(), contents.size());
        return out.good() ? Ok : Failed;
    }

    StatusCode writeBinaryFile(Path path, ArrayRef<std::uint8_t> data){
        std::ofstream out(path.str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if(!out.is_open()){
            return Failed;
        }
        out.write(reinterpret_cast<const char *>(&*data.begin()), data.size());
        return out.good() ? Ok : Failed;
    }

    // -- Copy / Move (cross-platform fallback) --

    StatusCode copyFile(Path src, Path dest){
        std::ifstream in(src.str(), std::ios::in | std::ios::binary);
        if(!in.is_open()){
            return Failed;
        }
        std::ofstream out(dest.str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if(!out.is_open()){
            return Failed;
        }
        out << in.rdbuf();
        return (in.good() || in.eof()) && out.good() ? Ok : Failed;
    }

    StatusCode moveFile(Path src, Path dest){
        if(std::rename(src.str().c_str(), dest.str().c_str()) == 0){
            return Ok;
        }
        // rename fails across filesystems — fall back to copy + delete
        if(copyFile(src, dest) == Ok){
            std::remove(src.str().c_str());
            return Ok;
        }
        return Failed;
    }

    // -- Glob / Filtered Enumeration --

    static bool globMatch(const char *pattern, const char *str){
        while(*pattern && *str){
            if(*pattern == '*'){
                ++pattern;
                if(*pattern == '\0') return true;
                while(*str){
                    if(globMatch(pattern, str)) return true;
                    ++str;
                }
                return false;
            }
            else if(*pattern == '?'){
                ++pattern;
                ++str;
            }
            else {
                if(*pattern != *str) return false;
                ++pattern;
                ++str;
            }
        }
        while(*pattern == '*') ++pattern;
        return *pattern == '\0' && *str == '\0';
    }

    Vector<Path> glob(Path dir, StrRef pattern){
        Vector<Path> results;
        DirectoryIterator it(dir);
        while(!it.end()){
            Path entry = *it;
            String fname = entry.filename();
            if(!entry.ext().empty()){
                fname += "." + entry.ext();
            }
            if(globMatch(pattern.data(), fname.c_str())){
                results.push_back(entry);
            }
            ++it;
        }
        return results;
    }

 };