#include <initializer_list>
#ifndef __cplusplus
#error OmegaCommon must be compiled as a C++ api
#endif

#include <string>
#include <fstream>
#include <assert.h>
#include <sstream>
#include <type_traits>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstring>
#include <typeinfo>
#include <memory>
#include <optional>
#include <iostream>
#include <algorithm>
#include <cassert>



#ifndef OMEGA_COMMON_COMMON_UTILS_H
#define OMEGA_COMMON_COMMON_UTILS_H

#ifdef _WIN32
#ifdef OMEGACOMMON__BUILD__
#define OMEGACOMMON_EXPORT __declspec(dllexport)
#else 
#define OMEGACOMMON_EXPORT __declspec(dllimport)
#endif
#else 

#define OMEGACOMMON_EXPORT

#endif

#define OMEGACOMMON_CLASS_ID CLASS_ID

#define OMEGACOMMON_CLASS(id) static constexpr char OMEGACOMMON_CLASS_ID[] = id;


namespace OmegaCommon {
    template<class T,unsigned N_start>
    class HeapAllocator {
        T *_data;
        unsigned size;
        void resize(unsigned newSize){
            T *newData = new T[newSize];
            std::move(_data,_data + size,newData);
            size = newSize;
            delete [] _data;
            _data = newData;
        }
    public:
        HeapAllocator():_data(new T[N_start]),size(N_start){
            
        };
        T *allocate(size_t n){
            if(size < n){
                resize(n);
            }
            return _data;
        }
        void deallocate(T *ptr,size_t n){
            assert(size >= n);
        }
        ~HeapAllocator(){
            delete [] _data;
        }
    };

    typedef std::string String;
    typedef std::u16string WString;
    typedef std::u32string UString;

    template<class C,unsigned N_ch>
    using HeapString = std::basic_string<C,std::char_traits<C>,HeapAllocator<C,N_ch>>;

    typedef HeapString<char,10> SmallHeapString;
    typedef HeapString<char16_t,10> SmallWHeapString;
    typedef HeapString<char32_t,10> SmallUHeapString;
    
    template<class T,unsigned N_t>
    using HeapVector = std::vector<T,HeapAllocator<T,N_t>>;

    template<class T>
    using SmallHeapVector = std::vector<T,HeapAllocator<T,3>>;


    /** 
      Immutable Reference to a String.
    */
    template<class CharTY>
    class StrRefBase {
        const CharTY *_data = nullptr;
    public:
        typedef unsigned size_type;
    private:
        const size_type _len = 0;
        using SELF = StrRefBase<CharTY>;
    public:
        using const_iterator = const CharTY *;
        using const_reference = const CharTY &;

        const size_type & size() const{
            return _len;
        }
        const CharTY *data() const{
            return _data;
        }
        const_iterator begin() const{
            return const_iterator(_data);
        }
        const_iterator end() const{
            return const_iterator(_data + _len);
        }

        const_reference operator[](unsigned idx) const{
            return begin()[idx];
        };

    private:
        bool compare(StrRefBase & str) const{
            if(_len != str._len)
                return false;
            
            for(unsigned i = 0;i < _len;i++){
                auto & c = *(begin() + i);
                if(c != str[i]){
                    return false;
                };
            };
            return true;
        };
    public:

        StrRefBase():_data(nullptr),_len(0){

        };

        StrRefBase(const std::basic_string<CharTY> & str):_data(str.data()),_len((size_type)str.size()){

        }

        StrRefBase(CharTY *buffer,size_type length):_data(buffer),_len(length){

        }

        StrRefBase(const CharTY *c_str):_data(const_cast<char *>(c_str)),_len((size_type)strlen(c_str)){

        }

        StrRefBase( CharTY *c_str):_data(c_str),_len((size_type)strlen(c_str)){

        }

        bool operator==(const std::basic_string<CharTY> & str) const{
            StrRefBase ref(str);
            return compare(ref);
        };

        bool operator==(StrRefBase &str) const{
            return compare(str);
        };

        bool operator==(const CharTY *str) const{
            StrRefBase ref(str);
            return compare(ref);
        };

        bool operator!=(const std::basic_string<CharTY> & str) const{
            StrRefBase ref(str);
            return !compare(ref);
        };

        bool operator!=(StrRefBase &str) const{
            return !compare(str);
        };

        bool operator!=(const CharTY *str) const{
            StrRefBase ref(str);
            return !compare(ref);
        };

        operator std::basic_string<CharTY>(){
            return {this->begin(),this->end()};
        };

    };

    template<class CharTy>
    inline bool operator==(const std::basic_string<CharTy> & lhs,const StrRefBase<CharTy> & rhs){
        return rhs == lhs;
    }

    template<class CharTy>
    inline bool operator==(const char * lhs,const StrRefBase<CharTy> & rhs){
        return rhs == lhs;
    }

    typedef StrRefBase<char> StrRef;
    typedef StrRefBase<char16_t> WStrRef;
    typedef StrRefBase<char32_t> UStrRef;


    OMEGACOMMON_EXPORT String operator+(const String & lhs,const StrRef & rhs);
    OMEGACOMMON_EXPORT WString operator+(const WString & lhs,const WStrRef & rhs);
    OMEGACOMMON_EXPORT UString operator+(const UString & lhs,const UStrRef & rhs);

    OMEGACOMMON_EXPORT void operator+=(String & lhs, StrRef & rhs);
    OMEGACOMMON_EXPORT void operator+=(WString & lhs,WStrRef & rhs);
    OMEGACOMMON_EXPORT void operator+=(UString & lhs,UStrRef & rhs);

    OMEGACOMMON_EXPORT StrRef operator&(String & str);
    OMEGACOMMON_EXPORT WStrRef operator&(WString & str);
    OMEGACOMMON_EXPORT UStrRef operator&(UString & str);



    template<class T>
    using Vector = std::vector<T>;

    template<class T,class Comp = std::equal_to<T>>
    class SetVector : Vector<T>{
        Comp compare;
        typedef Vector<T> super;
    public:
        typedef typename super::size_type size_type;
        typedef typename super::iterator iterator;
        typedef typename super::reference reference;

        size_type & size() const{
            return super::size();
        }

        iterator begin(){
            return super::begin();
        }

        iterator end(){
            return super::end();
        }

        iterator find(const T & el){
            auto it = begin();
            for(;it != end();it++){
                if(compare(*it,el)){
                    break;
                }
            }
            return it;
        }

        void push(const T & el){
            if(find(el) == end())
                super::push_back(el);
        }

        void push(T && el){
            if(find(el) == end())
                super::push_back(el);
        }

        void pop(){
            super::pop_back();
        }
    };



    template<class T,unsigned len>
    using Array = std::array<T,(size_t)len>;

    /** 
      The base class for all container reference classes.
    */
    template<class T>
    class ContainerRefBase {
    protected:
        T *_data;
    public:
        typedef unsigned int size_type;
    protected:
        const size_type _size;
    public:
        typedef const T * const_iterator;
        typedef const T & const_reference;

        bool empty() noexcept{
            return _size == 0;
        };

        const size_type & size() const{
            return _size;
        };
        
        const_iterator begin() const{
            return const_iterator(_data);
        };
        const_iterator end() const{
            return const_iterator(_data + _size);
        };

        const_reference front() const{
            return begin()[0];
        };

        const_reference back() const{
            return end()[-1];
        };

        template<class _iterator>
        ContainerRefBase(_iterator _st,_iterator _end):_data(_st),_size(size_type(_end - _st)){

        };

//        template<class K,class V>
//        ContainerRefBase<std::pair<K,V>>(typename std::map<K,V>::iterator _st,
//                                         typename std::map<K,V>::iterator _end){
//
//        };

        ContainerRefBase(T * data,size_type len):_data(data),_size(len){

        };
    };

    /** 
      An immutable reference to an Array or Vector
    */
    template<class T>
    class ArrayRef : public ContainerRefBase<T>{
        using super = ContainerRefBase<T>;
    public:
        typedef typename super::const_reference const_reference;
        typedef typename super::size_type size_type;

        const_reference operator[](size_type idx){
            assert(idx < this->size() && "Index must be smaller than the size of the ArrayRef");
            return this->begin()[idx];
        };
        template<class __It>
        ArrayRef(__It beg,__It end):ContainerRefBase<T>(beg,end){
            
        };
        

        ArrayRef(Vector<T> & vec):ContainerRefBase<T>(vec.data(),(size_type)vec.size()){
            
        };

        template<size_type len>
        ArrayRef(const Array<T,len> & array):ContainerRefBase<T>(array.data(),len){
            
        };

        operator Vector<T>(){
            return {this->begin(),this->end()};
        }
    };

    template<class T>
    ArrayRef<T> operator&(Vector<T> & other){
        return other;
    };

    template<class T>
    ArrayRef<T> makeArrayRef(T * begin,T * end){
        return {begin,end};
    }

    template<class K,class V>
    using Map = std::map<K,V>;

    template<class K,class V>
    using MapVec = std::unordered_map<K,V>;



    /** 
      An immutable reference to an Map or an MapVector
    */
    template<class K,class V>
    class MapRef {
        OmegaCommon::Map<K,V> & ref;
    public:
        typedef typename OmegaCommon::Map<K,V>::const_iterator const_iterator;
        typedef typename OmegaCommon::Map<K,V>::size_type size_type;

        typedef const K & const_key_ref;
        typedef const V & const_val_ref;

        const_iterator begin(){
            return ref.begin();
        }

        const_iterator end(){
            return ref.end();
        }

        size_type size(){
            return ref.size();
        }


        const_iterator find(const_key_ref key){
            return ref.find(key);
        };

        const_val_ref operator[](const_key_ref key){
            return ref[key];
        };

        MapRef(Map<K,V> & map):ref(map){

        };

        operator Map<K,V>(){
            return {this->begin(),this->end()};
        };
    };

    template<class K,class V>
    MapRef<K,V> operator&(Map<K,V> & other){
        return other;
    };

    /// A vector that acts like a queue (first in , first out), but has control over every element and its order in the container.
    template<class Ty>
    class OMEGACOMMON_EXPORT   QueueVector
    {
        Ty *_data;
    public:
        typedef unsigned int size_type;
    private:
        size_type len = 0;
    public:
        using iterator = Ty *;
        using reference = Ty &;
        const size_type & size() noexcept {return len;};
        bool empty() noexcept {return len == 0;};
        iterator begin(){ return _data;};
        iterator end(){return _data + (len * sizeof(Ty));};
        reference first(){ return begin()[0];};
        reference last(){ return end()[-1];};
        reference operator[](size_type idx){ return begin()[idx];};
    private:
        void _push_el(const Ty & el){
            if(len == 0)
                _data = new Ty(std::move(el));
            else {
                Ty temp[len];
                std::move(begin(),end(),temp);
                delete [] _data;
                _data = new Ty[len + 1];
                std::move(temp,temp + (sizeof(Ty) * len),begin());
                begin()[len] = std::move(el);
            };
            ++len;
        };
        void _insert_el_at_idx(const Ty & el,size_type & idx){
            if(len == 0) {
                assert(idx == 0 && "Cannot emplace item at requested index! No mem allocated!");
                _data = new Ty(std::move(el));
            }
            else {
                assert(idx < len && "Index is out of range!");
                Ty temp[len + 1];
                std::move(begin(),begin() + (idx * sizeof(Ty)),temp);
                temp[idx] = std::move(el);
                std::move(begin() + (idx * sizeof(Ty)),end(),temp + ((idx+1) * sizeof(Ty)));
                delete [] _data;
                _data = new Ty[len + 1];
                std::move(temp,temp + (sizeof(Ty) * (len + 1)),begin());
            };
            ++len;
        };
    public:
        void insert(const Ty & el,size_type idx){
            _insert_el_at_idx(el,idx);
        };
        void insert(Ty && el,size_type idx){
            _insert_el_at_idx(el,idx);
        };
        void push(const Ty & el){
            _push_el(el);
        };
        void push(Ty && el){
            _push_el(el);
        };
        void pop(){
            assert(!empty() && "Cannot call pop() on empty QueueVector!");
            auto f_el = first();
            f_el.~Ty();
            Ty temp[len-1];
            std::move(begin() + sizeof(Ty),end(),temp);
            delete [] _data;
            --len;
            _data = new Ty[len];
            std::move(temp,temp + (len * sizeof(Ty)),begin());
        };
        QueueVector():_data(nullptr),len(0){};
        QueueVector(const QueueVector<Ty> & other):len(other.len){
            _data = new Ty[len];
            std::copy(other.begin(),other.end(),begin());
        };
        QueueVector(QueueVector<Ty> && other):len(other.len){
            _data = new Ty[len];
            std::copy(other.begin(),other.end(),begin());

        };
        ~QueueVector(){
            auto it = begin();
            while(it != end()){
                reference item = *it;
                item.~Ty();
                ++it;
            };
            delete [] _data;
        };

    };

    /** @brief A queue data type that preallocates its memory on the heap that has a limited capacity, however it can be resized when nesscary.
            @paragraph
             This class typically gets used in a scenario
             where there could be thousands of objects that get dynamically constructed and destroyed by a standard data type such as std::vector
             but in a scenario as such, all the standard types are extremely inefficient and can cause fragmented memory.

             The QueueHeap class has a similar implementation to a heap data type rather it has more control over how the data gets copied to/removed from the heap.
             In addition, it only allows construction/destruction of objects in the notions of a "first in, first out" data type.
        */
    template<class Ty>
    class QueueHeap {
        std::allocator<Ty> _alloc;
    protected:
        Ty *_data;
    public:
        using size_type = unsigned;
    private:
        size_type len;
        size_type max_len;
    public:
        using reference = Ty &;
        bool empty() noexcept {return len == 0;};
        bool full() noexcept {return len == max_len;};

        /// @brief Gets the length of the Queue Heap
        size_type & length(){ return len;};

        /// @brief Returns a reference to the first element.
        reference first(){ return _data[0];};

        /// @brief Returns a reference to the last element.
        reference last(){ return _data[len-1];};
        template<class Pred>
        void filter(Pred pred){
            auto _end = std::remove_if(_data,_data + len,pred);
            auto it_end = _data + len;
            unsigned count = 0;
            while(_end != it_end){
                _end->~Ty();
                ++_end;
                count += 1;
            }
            auto newSize = len - count;
            auto temp = _alloc.allocate(newSize);
            memcpy(temp,_data,newSize);
            _alloc.deallocate(_data,len);
            _data = _alloc.allocate(newSize);
            memcpy(_data,temp,newSize);
            _alloc.deallocate(_data,newSize);
            len = newSize;
        }
    protected:
        void _push_el(const Ty & el){
            new (_data + len) Ty(el);
            ++len;
        };
    public:
        virtual void push(const Ty & el){
            _push_el(el);
        };
        virtual void push(Ty && el){
            _push_el(el);
        };
        void pop(){
            assert(!empty() && "Cannot call pop() on empty QueueHeap!");
            _data[0].~Ty();
            --len;
            memcpy(_data,_data + 1,sizeof(Ty) * len);
        };
        void resize(size_type new_max_size){
            assert(max_len < new_max_size && "");
            _alloc.deallocate(_data,max_len);
            _data = _alloc.allocate(new_max_size);
            max_len = new_max_size;
        };

        explicit QueueHeap(size_type max_size):_data((Ty *)_alloc.allocate(max_size)),max_len(max_size),len(0){

        };
        virtual ~QueueHeap(){
            _alloc.deallocate(_data,max_len);
        };
    };

    template<class Ty,class Compare_Ty>
    class PriorityQueueHeap : public QueueHeap<Ty> {
        Compare_Ty comp;
        using super = QueueHeap<Ty>;
        void _sort(){
            std::sort(super::_data,super::_data + super::length(),comp);
        };
    public:
        void push(const Ty & el) override{
            super::_push_el(el);
            _sort();
        };
        void push(Ty && el) override{
            super::_push_el(el);
            _sort();
        };

        explicit PriorityQueueHeap(typename super::size_type max_size,Compare_Ty comp = Compare_Ty()):QueueHeap<Ty>(max_size),comp(comp){

        };
        ~PriorityQueueHeap() override = default;
    };


    struct RuntimeObject {
        unsigned refCount;
        void inc() { refCount += 1;};
        void dec() {refCount -= 1;}
        RuntimeObject() : refCount(1){

        }
    };

    template<class T,std::enable_if_t<std::is_base_of_v<RuntimeObject,T>,int> = 0>
    class ARC {
    protected:
        T *data;
    public:
        operator bool(){
            return data != nullptr;
        }
        T & operator *() const{
            return *data;
        }
        T * operator->() const{
            return data;
        }
        explicit ARC(T * ptr){
            data = ptr;
        };
        template<class OTy,std::enable_if_t<std::is_convertible_v<OTy,T>,int> = 0>
        ARC(OTy * ptr){
            data = ptr;
        };
        template<class OTy,std::enable_if_t<std::is_convertible_v<OTy,T>,int> = 0>
        ARC(const ARC<OTy> &other){
            data = other.data;
            data->inc();
        }
        template<class OTy,std::enable_if_t<std::is_convertible_v<OTy,T>,int> = 0>
        ARC(ARC<OTy> && other){
            data = other.data;
            data->inc();
        }
        ~ARC(){
            data->dec();
            if(data->refCount == 0){
                delete data;
            }
        };
    };

    template<class T,class ...Args>
    ARC<T> makeARC(Args && ...args){
        return ARC<T>(new T(args...));
    };

    template<class T>
    struct RuntimeTypeWrapper : public RuntimeObject {
        T data;
    };

    template<class T>
    class ARCAny : protected ARC<RuntimeTypeWrapper<T>> {
    public:
        explicit ARCAny(RuntimeTypeWrapper<T> * ptr): ARC<RuntimeTypeWrapper<T>>(ptr){

        }
        T & operator *() const {
            return this->data->data;
        }
        T * operator->() const {
            return &(this->data->data);
        }
    };

    template<class T,class ...Args>
    ARCAny<T> makeARCAny(Args && ...args){
        return ARCAny<T>(new RuntimeTypeWrapper<T>{{args...}});
    };

    typedef enum : int {
        Ok,
        Failed
    } StatusCode;
    template<class T>
    using Optional = std::optional<T>;

    #define string_enum_field static constexpr const char *
    #define string_enum namespace


//    namespace Argv {
//
//        template<class T>
//        struct ArgVal{
//            std::shared_ptr<T> value;
//            inline operator bool(){
//                return value;
//            };
//            inline operator T & (){
//                return *value;
//            }
//        };
//
//        enum class ArgumentType : int {
//            Flag,
//            Positional,
//        };
//
//        struct Flag {
//            StrRef val;
//            Flag(StrRef val):val(val){
//
//            }
//        };
//
//        struct Desc {
//            StrRef val;
//            Desc(StrRef val):val(val){
//
//            }
//        };
//
//        template<class T>
//        struct ArgumentParser;
//
//        struct ArgumentParserBase {
//            virtual void printHelp() = 0;
//            virtual bool parseArg(OmegaCommon::StrRef arg) = 0;
//        };
//
//        template<class T,
//                std::enable_if_t<std::is_function_v<decltype(ArgumentParser<T>::help)>,int> = 0,
//                std::enable_if_t<std::is_function_v<decltype(ArgumentParser<T>::format)>,int> = 0>
//        struct ArgumentParserImpl : public ArgumentParserBase {
//
//            std::vector<std::string> flagMatches;
//            Desc desc;
//            ArgumentType type;
//            std::shared_ptr<T> & val;
//
//            ArgumentParserImpl(ArgumentType type,std::vector<std::string> flagMatches, Desc & desc,std::shared_ptr<T> & val):
//            flagMatches(flagMatches),
//            desc(desc),
//            type(type),val(val){
//
//            };
//
//            void printHelp() override{
//                ArgumentParser<T>::help(std::cout);
//            }
//
//            bool parseArg(OmegaCommon::StrRef arg) override{
//                return ArgumentParser<T>::format(arg,flagMatches,val);
//            }
//        };
//
//        template<>
//        struct  ArgumentParser<bool> {
//
//            static void help(std::ostream & out,Desc & desc,ArrayRef<std::string> & m){
//
//            }
//            static bool parse(OmegaCommon::StrRef arg,const ArrayRef<std::string> & m,std::shared_ptr<bool> & value){
//
//            }
//        };
//
//
//        template<class T,class ..._Args>
//        ArgumentParserBase * buildArgumentParser(std::shared_ptr<T> & val,ArgumentType type,std::vector<std::string> flagMatches, Desc & desc){
//            return new ArgumentParserImpl<T>(type,flagMatches,desc,val);
//        };
//
//
//        class Parser {
//
//        public:
//
//            Parser();
//
//            template<class T>
//            ArgVal<T> argument();
//
//            template<>
//            ArgVal<bool> argument(){
//
//            };
//            template<>
//            ArgVal<String> argument(){
//
//            };
//            template<>
//            ArgVal<Vector<String>> argument(){
//
//            };
//
//            void parseArgv(int & argc,char **argv);
//        };
//
//    }
    template<class O,class T>
    bool is(std::shared_ptr<T> &object){
        return (dynamic_cast<O *>(object.get()) != nullptr);
    };
};

bool findProgramInPath(const OmegaCommon::StrRef & prog,OmegaCommon::String & out);

inline std::ostream & operator<<(std::ostream &os,OmegaCommon::StrRef &str){
    return os << str.data();
};

inline std::wostream & operator<<(std::wostream &os,OmegaCommon::WStrRef &str){
    return os << str.data();
};

template<class _Ty>
using SharedHandle = std::shared_ptr<_Ty>;
/**Creates a Shared Instance of _Ty and returns it*/
template<class _Ty,class... _Args>
inline SharedHandle<_Ty> make(_Args && ...args){
//    static_assert(std::is_constructible<_Ty,_Args...>::value,"Cannot construct item");
    return std::make_shared<_Ty>(args...);
};

template<class _Ty>
using UniqueHandle = std::unique_ptr<_Ty>;
/**Creates a Unique Instance of _Ty and returns it*/
template<class _Ty,class... _Args>
inline UniqueHandle<_Ty> && construct(_Args && ...args){
    static_assert(std::is_constructible<_Ty,_Args...>::value,"Cannot construct item");
    return std::make_unique<_Ty>(args...);
};
/**
 * @brief Creates a SharedHandle type-alias.
 * 
 */
#define OMEGACOMMON_SHARED_CLASS(name) typedef SharedHandle<name> name##Ptr
/**
 * @brief Creates a SharedHandle type-alias.
 * 
 */
#define OMEGACOMMON_UNIQUE_CLASS(name) typedef UniqueHandle<name> name##UPtr

#define OMEGACOMMON_SHARED(name) name##Ptr;

#define OMEGACOMMON_UNIQUE(name) name##UPtr;

//inline std::basic_ostream<char32_t> & operator<<(std::basic_ostream<char32_t> &os,OmegaCommon::UStrRef &str){
//    return os << str.data();
//};



#endif
