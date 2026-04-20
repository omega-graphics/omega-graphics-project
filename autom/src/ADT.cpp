#include "ADT.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstdint>

#include <filesystem>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#elif defined(_WIN32)
#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib,"bcrypt.lib")

#endif

namespace autom {
    
    typedef unsigned char HashByte;

namespace {

    constexpr std::size_t SHA256DigestLength = 32;
    constexpr std::size_t SHA256BlockLength = 64;

#if !defined(__APPLE__) && !defined(_WIN32)
    struct SHA256Context {
        std::uint32_t state[8];
        std::uint64_t bitCount;
        std::uint8_t buffer[SHA256BlockLength];
        std::size_t bufferSize;

        SHA256Context()
            : state{
                0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u},
              bitCount(0),
              buffer{},
              bufferSize(0) {
        }
    };

    inline std::uint32_t rotr(std::uint32_t value, unsigned shift) {
        return (value >> shift) | (value << (32u - shift));
    }

    inline std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return (x & y) ^ (~x & z);
    }

    inline std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    inline std::uint32_t bigSigma0(std::uint32_t x) {
        return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
    }

    inline std::uint32_t bigSigma1(std::uint32_t x) {
        return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
    }

    inline std::uint32_t smallSigma0(std::uint32_t x) {
        return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
    }

    inline std::uint32_t smallSigma1(std::uint32_t x) {
        return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
    }

    void sha256Transform(SHA256Context &context, const std::uint8_t block[SHA256BlockLength]) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        std::uint32_t schedule[64];
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t offset = i * 4;
            schedule[i] = (static_cast<std::uint32_t>(block[offset]) << 24u)
                        | (static_cast<std::uint32_t>(block[offset + 1]) << 16u)
                        | (static_cast<std::uint32_t>(block[offset + 2]) << 8u)
                        | static_cast<std::uint32_t>(block[offset + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            schedule[i] = smallSigma1(schedule[i - 2]) + schedule[i - 7]
                        + smallSigma0(schedule[i - 15]) + schedule[i - 16];
        }

        std::uint32_t a = context.state[0];
        std::uint32_t b = context.state[1];
        std::uint32_t c = context.state[2];
        std::uint32_t d = context.state[3];
        std::uint32_t e = context.state[4];
        std::uint32_t f = context.state[5];
        std::uint32_t g = context.state[6];
        std::uint32_t h = context.state[7];

        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t temp1 = h + bigSigma1(e) + choose(e, f, g) + k[i] + schedule[i];
            const std::uint32_t temp2 = bigSigma0(a) + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        context.state[0] += a;
        context.state[1] += b;
        context.state[2] += c;
        context.state[3] += d;
        context.state[4] += e;
        context.state[5] += f;
        context.state[6] += g;
        context.state[7] += h;
    }

    void sha256Update(SHA256Context &context, const std::uint8_t *input, std::size_t length) {
        context.bitCount += static_cast<std::uint64_t>(length) * 8u;
        while (length > 0) {
            const auto toCopy = std::min(length, SHA256BlockLength - context.bufferSize);
            std::memcpy(context.buffer + context.bufferSize, input, toCopy);
            context.bufferSize += toCopy;
            input += toCopy;
            length -= toCopy;

            if (context.bufferSize == SHA256BlockLength) {
                sha256Transform(context, context.buffer);
                context.bufferSize = 0;
            }
        }
    }

    void sha256Final(SHA256Context &context, std::uint8_t output[SHA256DigestLength]) {
        context.buffer[context.bufferSize++] = 0x80u;

        if (context.bufferSize > 56) {
            while (context.bufferSize < SHA256BlockLength) {
                context.buffer[context.bufferSize++] = 0;
            }
            sha256Transform(context, context.buffer);
            context.bufferSize = 0;
        }

        while (context.bufferSize < 56) {
            context.buffer[context.bufferSize++] = 0;
        }

        for (int i = 7; i >= 0; --i) {
            context.buffer[context.bufferSize++] = static_cast<std::uint8_t>((context.bitCount >> (i * 8)) & 0xffu);
        }

        sha256Transform(context, context.buffer);

        for (std::size_t i = 0; i < 8; ++i) {
            output[i * 4] = static_cast<std::uint8_t>((context.state[i] >> 24u) & 0xffu);
            output[i * 4 + 1] = static_cast<std::uint8_t>((context.state[i] >> 16u) & 0xffu);
            output[i * 4 + 2] = static_cast<std::uint8_t>((context.state[i] >> 8u) & 0xffu);
            output[i * 4 + 3] = static_cast<std::uint8_t>(context.state[i] & 0xffu);
        }
    }
#endif

    void writeHashAsHex(const HashByte *hash, std::size_t hashLength, std::string &out) {
        std::ostringstream o;
        o << std::hex << std::uppercase;
        for (std::size_t i = 0; i < hashLength; ++i) {
            o << std::setw(2) << std::setfill('0') << int(hash[i]);
        }
        out = o.str();
    }
}
    
#ifdef __APPLE__
    
    SHA256Hash::SHA256Hash():data(new CC_SHA256_CTX){
        CC_SHA256_Init((CC_SHA256_CTX *)data);
    }
    
    void SHA256Hash::addData(void * data,size_t dataSize){
        auto * ctxt = (CC_SHA256_CTX *)this->data;
        CC_SHA256_Update(ctxt,data,dataSize);
    };
    
    HashByte * SHA256Hash::getResult(){
        auto * hash = new HashByte[CC_SHA256_DIGEST_LENGTH];
        auto * ctxt = (CC_SHA256_CTX *)data;
        
        CC_SHA256_Final(hash,ctxt);
        return hash;
    }
    
    void SHA256Hash::getResultAsHex(std::string & out){

        auto *hash = getResult();
        writeHashAsHex(hash, CC_SHA256_DIGEST_LENGTH, out);
        delete[] hash;
    }
    
    SHA256Hash::~SHA256Hash(){
        delete (CC_SHA256_CTX *)data;
    }
#elif defined(_WIN32)

    SHA256Hash::SHA256Hash():data(new BCRYPT_HASH_HANDLE) {
        BCRYPT_ALG_HANDLE algHandle;
        BCryptOpenAlgorithmProvider(&algHandle,BCRYPT_SHA256_ALGORITHM,NULL,NULL);

        BCryptCreateHash(algHandle,(BCRYPT_HASH_HANDLE *)data,NULL,NULL,NULL,NULL,NULL);
        BCryptCloseAlgorithmProvider(algHandle,NULL);
    }

    void SHA256Hash::addData(void *data, size_t dataSize) {
        auto hashHandle = (BCRYPT_HASH_HANDLE *)this->data;
        BCryptHashData(*hashHandle,(PUCHAR)data,dataSize,NULL);
    }

    unsigned char *SHA256Hash::getResult() {
        PUCHAR hash = new UCHAR[32];
        auto hashHandle = (BCRYPT_HASH_HANDLE *)this->data;
        BCryptFinishHash(*hashHandle,hash,32,NULL);
        return hash;
    }

    void SHA256Hash::getResultAsHex(std::string &out) {
        auto *hash = getResult();
        writeHashAsHex(hash, SHA256DigestLength, out);
        delete[] hash;
    }

    SHA256Hash::~SHA256Hash() {
        BCryptDestroyHash(*((BCRYPT_HASH_HANDLE *)data));
        delete (BCRYPT_HASH_HANDLE *)data;
    }
#else
    SHA256Hash::SHA256Hash():data(new SHA256Context) {
    }

    void SHA256Hash::addData(void *data, size_t dataSize) {
        auto *context = static_cast<SHA256Context *>(this->data);
        sha256Update(*context, static_cast<const std::uint8_t *>(data), dataSize);
    }

    unsigned char *SHA256Hash::getResult() {
        auto *context = static_cast<SHA256Context *>(data);
        auto *hash = new HashByte[SHA256DigestLength];
        auto contextCopy = *context;
        sha256Final(contextCopy, hash);
        return hash;
    }

    void SHA256Hash::getResultAsHex(std::string &out) {
        auto *hash = getResult();
        writeHashAsHex(hash, SHA256DigestLength, out);
        delete[] hash;
    }

    SHA256Hash::~SHA256Hash() {
        delete static_cast<SHA256Context *>(data);
    }
#endif


    std::ostream & operator<<(std::ostream & os,StrRef & strRef){
        return os.write(strRef.data(),strRef.size());
    }

    std::ostream & operator<<(std::ostream & os,const StrRef & strRef){
        return os.write(strRef.data(),strRef.size());
    };

bool locateProgram(autom::StrRef prog,std::string path,std::string & out){
    
    std::istringstream in(path);
    std::string parentPath;

#ifdef _WIN32
    while(!in.eof()){
        std::getline(in,parentPath,';');
#else

    while(!in.eof()){
        std::getline(in,parentPath,':');

#endif
        
        auto p = std::filesystem::path(parentPath).append(prog.data());
        if(std::filesystem::is_symlink(p)){
            out = std::filesystem::read_symlink(p).string();
            return true;
        }
        else {
#ifdef _WIN32
            p = p.replace_extension("exe");
            if(std::filesystem::exists(p)){
                out = p.string();
                return true;
            }
#else
            if(std::filesystem::exists(p)){
                out = p.string();
                return true;
            }
#endif
        }
    }
    return false;
    
};



}
