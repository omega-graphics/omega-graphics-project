#include "omega-common/assets.h"

#include "omega-common/utils.h"
#include "omega-common/crypto.h"

#include "../assetc/assetc.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <istream>
#include <limits>
#include <streambuf>
#include <utility>
#include <vector>

namespace OmegaCommon {

namespace {

namespace assetc = OmegaCommon::assetc;

using Byte = unsigned char;
constexpr const char *EncryptionNonceLabel = "omega-assetc:entry-nonce:v1";

struct RuntimeAssetEntry {
    String name;
    AssetType type = AssetType::Raw;
    std::uint64_t fileOffset = 0;
    std::uint64_t rawSize = 0;
    std::uint64_t storedSize = 0;
    std::uint32_t flags = static_cast<std::uint32_t>(assetc::AssetEntryFlags::None);
    std::array<std::uint8_t, 32> entryHash {};
    bool hasHash = false;
};

String stringFromRef(StrRef value) {
    if(value.data() == nullptr || value.size() == 0) {
        return {};
    }
    return String(value.data(), value.size());
}

bool readExact(std::istream &in, char *data, std::streamsize size) {
    if(size == 0) {
        return true;
    }
    in.read(data, size);
    return static_cast<std::streamsize>(in.gcount()) == size;
}

bool toStreamSize(std::uint64_t value, std::streamsize &out) {
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max());
    if(value > kMax) {
        return false;
    }
    out = static_cast<std::streamsize>(value);
    return true;
}

bool toStreamOffset(std::uint64_t value, std::streamoff &out) {
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max());
    if(value > kMax) {
        return false;
    }
    out = static_cast<std::streamoff>(value);
    return true;
}

bool toSizeT(std::uint64_t value, size_t &out) {
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<size_t>::max());
    if(value > kMax) {
        return false;
    }
    out = static_cast<size_t>(value);
    return true;
}

AssetType assetTypeFromSerializedValue(std::uint16_t value) {
    switch(static_cast<AssetType>(value)) {
        case AssetType::Raw:
        case AssetType::Image:
        case AssetType::Font:
        case AssetType::Shader:
        case AssetType::Text:
        case AssetType::Audio:
        case AssetType::Binary:
        case AssetType::Model:
        case AssetType::Material:
        case AssetType::Scene:
        case AssetType::Animation:
        case AssetType::Skeleton:
            return static_cast<AssetType>(value);
    }
    return AssetType::Raw;
}

Result<std::array<std::uint8_t, 32>, String> sha256(ArrayRef<std::uint8_t> bytes) {
    auto digestResult = digest(DigestAlgorithm::SHA256, bytes);
    if(digestResult.isErr()) {
        return Result<std::array<std::uint8_t, 32>, String>::err(
            "SHA-256 failed: " + digestResult.error().message);
    }

    auto &hashBytes = digestResult.value().bytes;
    if(hashBytes.size() != 32) {
        return Result<std::array<std::uint8_t, 32>, String>::err(
            "SHA-256 returned an unexpected digest length.");
    }

    std::array<std::uint8_t, 32> out {};
    std::copy(hashBytes.begin(), hashBytes.end(), out.begin());
    return Result<std::array<std::uint8_t, 32>, String>::ok(std::move(out));
}

String bundleKeyPath(StrRef bundlePath) {
    return String(bundlePath.data(), bundlePath.size()) + ".key";
}

bool isHexDigit(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

int hexValue(char ch) {
    if(ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if(ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    return 10 + (ch - 'A');
}

Result<Vector<std::uint8_t>, String> parseKeyFileBytes(const Vector<std::uint8_t> &bytes,
                                                       const String &sourcePath) {
    if(bytes.size() == EncryptionKey::KeySize) {
        return Result<Vector<std::uint8_t>, String>::ok(bytes);
    }

    String hex;
    hex.reserve(bytes.size());
    for(auto byte : bytes) {
        auto ch = static_cast<char>(byte);
        if(std::isspace(static_cast<unsigned char>(ch)) != 0) {
            continue;
        }
        if(!isHexDigit(ch)) {
            return Result<Vector<std::uint8_t>, String>::err(
                "Key file contains non-hex data and is not a raw 32-byte key: " + sourcePath);
        }
        hex.push_back(ch);
    }

    if(hex.size() != (EncryptionKey::KeySize * 2)) {
        return Result<Vector<std::uint8_t>, String>::err(
            "Key file must contain either 32 raw bytes or 64 hex characters: " + sourcePath);
    }

    Vector<std::uint8_t> parsed;
    parsed.reserve(EncryptionKey::KeySize);
    for(size_t i = 0; i < hex.size(); i += 2) {
        auto high = hexValue(hex[i]);
        auto low = hexValue(hex[i + 1]);
        parsed.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return Result<Vector<std::uint8_t>, String>::ok(std::move(parsed));
}

Result<Vector<std::uint8_t>, String> readKeyFile(StrRef pathValue) {
    String path(pathValue.data(), pathValue.size());
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if(!in.is_open()) {
        return Result<Vector<std::uint8_t>, String>::err("Failed to open key file: " + path);
    }

    auto size = in.tellg();
    if(size < 0) {
        return Result<Vector<std::uint8_t>, String>::err("Failed to read key file: " + path);
    }

    in.seekg(0, std::ios::beg);
    Vector<std::uint8_t> bytes(static_cast<size_t>(size));
    if(!bytes.empty()) {
        in.read(reinterpret_cast<char *>(bytes.data()), size);
    }
    if(in.bad()) {
        return Result<Vector<std::uint8_t>, String>::err("Failed to read key file: " + path);
    }

    return parseKeyFileBytes(bytes, path);
}

Result<Vector<std::uint8_t>, String> tryLoadCompanionKey(StrRef bundlePath) {
    auto keyPath = bundleKeyPath(bundlePath);
    auto keyBytes = readKeyFile(keyPath);
    if(keyBytes.isErr()) {
        return Result<Vector<std::uint8_t>, String>::err(
            "Encrypted asset bundle requires a 32-byte key. Tried companion key file: " + keyPath);
    }
    return keyBytes;
}

template <typename T>
void appendScalarBytes(Vector<std::uint8_t> &out, const T &value) {
    auto ptr = reinterpret_cast<const std::uint8_t *>(std::addressof(value));
    out.insert(out.end(), ptr, ptr + sizeof(T));
}

Vector<std::uint8_t> buildEntryAad(StrRef bundleName,
                                   AssetType assetType,
                                   std::uint64_t rawSize,
                                   std::uint32_t flags) {
    Vector<std::uint8_t> aad;
    auto nameLength = static_cast<std::uint16_t>(bundleName.size());
    auto serializedType = static_cast<std::uint16_t>(assetType);
    aad.reserve(sizeof(nameLength) + bundleName.size() + sizeof(serializedType) +
                sizeof(rawSize) + sizeof(flags));
    appendScalarBytes(aad, nameLength);
    aad.insert(aad.end(), bundleName.begin(), bundleName.end());
    appendScalarBytes(aad, serializedType);
    appendScalarBytes(aad, rawSize);
    appendScalarBytes(aad, flags);
    return aad;
}

Result<Nonce, String> deriveEntryNonce(const EncryptionKey &key,
                                       const std::array<std::uint8_t, 32> &entryHash,
                                       StrRef bundleName) {
    Vector<std::uint8_t> info;
    auto labelLength = std::char_traits<char>::length(EncryptionNonceLabel);
    info.reserve(labelLength + bundleName.size());
    info.insert(info.end(), EncryptionNonceLabel, EncryptionNonceLabel + labelLength);
    info.insert(info.end(), bundleName.begin(), bundleName.end());

    auto derived = hkdf(DigestAlgorithm::SHA256, key.data(), key.size(), entryHash.data(),
                        entryHash.size(), info.data(), info.size(), Nonce::NonceSize);
    if(derived.isErr()) {
        return Result<Nonce, String>::err("Failed to derive entry nonce: " +
                                          derived.error().message);
    }

    auto nonce = Nonce::fromBytes(derived.value().data(), derived.value().size());
    if(nonce.isErr()) {
        return Result<Nonce, String>::err("Failed to build entry nonce: " +
                                          nonce.error().message);
    }

    return Result<Nonce, String>::ok(std::move(nonce.value()));
}

} // namespace

struct AssetBundle::Impl {
    String bundlePath;
    bool signedBundle = false;
    bool compressedBundle = false;
    bool encryptedBundle = false;
    Vector<std::uint8_t> key;
    Vector<RuntimeAssetEntry> entries;
    MapVec<String, size_t> entryIndex;
};

namespace {

Result<void *, String> indexEntry(AssetBundle::Impl &impl, RuntimeAssetEntry &&entry) {
    auto inserted = impl.entryIndex.emplace(entry.name, impl.entries.size());
    if(!inserted.second) {
        return Result<void *, String>::err("Duplicate asset name in bundle: " + entry.name);
    }

    impl.entries.push_back(std::move(entry));
    return Result<void *, String>::ok(nullptr);
}

Result<void *, String> loadBundleV2Metadata(std::ifstream &in, AssetBundle::Impl &impl) {
    assetc::BundleHeader header {};
    if(!readExact(in, reinterpret_cast<char *>(&header), sizeof(header))) {
        return Result<void *, String>::err("Failed to read bundle header.");
    }

    if(!assetc::hasBundleMagic(header.magic)) {
        return Result<void *, String>::err("Bundle header is missing the expected magic bytes.");
    }

    if(header.version != assetc::BundleVersion) {
        return Result<void *, String>::err("Unsupported asset bundle version: " +
                                           std::to_string(header.version));
    }

    impl.signedBundle =
        (header.flags & static_cast<std::uint16_t>(assetc::BundleFlags::Signed)) != 0;
    impl.compressedBundle =
        (header.flags & static_cast<std::uint16_t>(assetc::BundleFlags::Compressed)) != 0;
    impl.encryptedBundle =
        (header.flags & static_cast<std::uint16_t>(assetc::BundleFlags::Encrypted)) != 0;

    if(impl.encryptedBundle && impl.key.empty()) {
        auto keyLoad = tryLoadCompanionKey(impl.bundlePath);
        if(keyLoad.isErr()) {
            return Result<void *, String>::err(keyLoad.error());
        }
        impl.key = std::move(keyLoad.value());
    }

    Vector<assetc::AssetEntry> entries;
    entries.resize(header.entryCount);
    if(!entries.empty()) {
        std::streamsize entriesSize = 0;
        if(!toStreamSize(static_cast<std::uint64_t>(entries.size() * sizeof(assetc::AssetEntry)),
                         entriesSize)) {
            return Result<void *, String>::err("Asset entry table is too large to read.");
        }
        if(!readExact(in, reinterpret_cast<char *>(entries.data()), entriesSize)) {
            return Result<void *, String>::err("Failed to read asset entry table.");
        }
    }

    Vector<char> stringTable;
    stringTable.resize(header.stringTableSize);
    if(!stringTable.empty()) {
        std::streamsize stringTableSize = 0;
        if(!toStreamSize(header.stringTableSize, stringTableSize)) {
            return Result<void *, String>::err("Bundle string table is too large to read.");
        }
        if(!readExact(in, stringTable.data(), stringTableSize)) {
            return Result<void *, String>::err("Failed to read bundle string table.");
        }
    }

    impl.entries.reserve(entries.size());
    for(const auto &sourceEntry : entries) {
        auto nameEnd = static_cast<std::uint64_t>(sourceEntry.nameOffset) +
                       static_cast<std::uint64_t>(sourceEntry.nameLength);
        if(nameEnd > stringTable.size()) {
            return Result<void *, String>::err("Bundle contains an out-of-range asset name.");
        }

        auto dataEnd = sourceEntry.dataOffset + sourceEntry.storedSize;
        if(dataEnd > header.dataRegionSize) {
            return Result<void *, String>::err("Bundle contains an out-of-range asset payload.");
        }

        RuntimeAssetEntry entry {};
        entry.name = String(stringTable.data() + sourceEntry.nameOffset,
                            sourceEntry.nameLength);
        entry.type = assetTypeFromSerializedValue(sourceEntry.assetType);
        entry.fileOffset = header.dataRegionOffset + sourceEntry.dataOffset;
        entry.rawSize = sourceEntry.rawSize;
        entry.storedSize = sourceEntry.storedSize;
        entry.flags = sourceEntry.flags;
        entry.hasHash = true;
        std::copy(std::begin(sourceEntry.entryHash), std::end(sourceEntry.entryHash),
                  entry.entryHash.begin());

        auto indexed = indexEntry(impl, std::move(entry));
        if(indexed.isErr()) {
            return indexed;
        }
    }

    return Result<void *, String>::ok(nullptr);
}

class SliceStreambuf : public std::streambuf {
public:
    SliceStreambuf(String path, std::streamoff sliceStart, std::streamoff sliceSize)
        : path_(std::move(path)),
          start_(sliceStart),
          size_(sliceSize),
          bufferStartPos_(0),
          file_(path_, std::ios::binary | std::ios::in) {
        if(file_.is_open()) {
            file_.seekg(start_, std::ios::beg);
        }
    }

    bool ok() const {
        return file_.is_open() && !file_.fail();
    }

protected:
    int_type underflow() override {
        if(!file_.is_open()) {
            return traits_type::eof();
        }
        std::streamoff bufferLen = (eback() == nullptr) ? 0 : (egptr() - eback());
        std::streamoff nextPos = bufferStartPos_ + bufferLen;
        if(nextPos >= size_) {
            return traits_type::eof();
        }
        file_.clear();
        file_.seekg(start_ + nextPos, std::ios::beg);
        if(file_.fail()) {
            return traits_type::eof();
        }
        std::streamoff remaining = size_ - nextPos;
        auto bufBytes = static_cast<std::streamoff>(sizeof(buffer_));
        auto toRead = static_cast<std::streamsize>(std::min(remaining, bufBytes));
        file_.read(buffer_, toRead);
        auto got = file_.gcount();
        if(got <= 0) {
            return traits_type::eof();
        }
        bufferStartPos_ = nextPos;
        setg(buffer_, buffer_, buffer_ + got);
        return traits_type::to_int_type(static_cast<unsigned char>(buffer_[0]));
    }

    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                     std::ios_base::openmode which) override {
        if((which & std::ios_base::in) == 0) {
            return pos_type(off_type(-1));
        }
        std::streamoff consumed = (eback() == nullptr) ? 0 : (gptr() - eback());
        std::streamoff currentPos = bufferStartPos_ + consumed;
        std::streamoff newPos = 0;
        if(dir == std::ios_base::beg) {
            newPos = static_cast<std::streamoff>(off);
        } else if(dir == std::ios_base::cur) {
            newPos = currentPos + static_cast<std::streamoff>(off);
        } else if(dir == std::ios_base::end) {
            newPos = size_ + static_cast<std::streamoff>(off);
        } else {
            return pos_type(off_type(-1));
        }
        if(newPos < 0 || newPos > size_) {
            return pos_type(off_type(-1));
        }
        // Drop the get area; next underflow() will refill from newPos.
        setg(nullptr, nullptr, nullptr);
        bufferStartPos_ = newPos;
        return pos_type(newPos);
    }

    pos_type seekpos(pos_type pos, std::ios_base::openmode which) override {
        return seekoff(off_type(pos), std::ios_base::beg, which);
    }

private:
    String path_;
    std::streamoff start_;
    std::streamoff size_;
    std::streamoff bufferStartPos_;
    std::ifstream file_;
    char buffer_[4096];
};

class SliceIstream : public std::istream {
public:
    explicit SliceIstream(UniqueHandle<SliceStreambuf> buf)
        : std::istream(buf.get()), buf_(std::move(buf)) {
        if(!buf_->ok()) {
            setstate(std::ios::failbit);
        }
    }

private:
    UniqueHandle<SliceStreambuf> buf_;
};

Result<Vector<std::uint8_t>, String> readStoredBytes(const AssetBundle::Impl &impl,
                                                     const RuntimeAssetEntry &entry) {
    std::ifstream in(impl.bundlePath, std::ios::binary | std::ios::in);
    if(!in.is_open()) {
        return Result<Vector<std::uint8_t>, String>::err("Failed to open asset bundle: " +
                                                         impl.bundlePath);
    }

    std::streamoff offset = 0;
    if(!toStreamOffset(entry.fileOffset, offset)) {
        return Result<Vector<std::uint8_t>, String>::err(
            "Asset payload offset is too large to read.");
    }

    in.seekg(offset, std::ios::beg);
    if(!in.good()) {
        return Result<Vector<std::uint8_t>, String>::err("Failed to seek to asset payload: " +
                                                         entry.name);
    }

    size_t storedSize = 0;
    if(!toSizeT(entry.storedSize, storedSize)) {
        return Result<Vector<std::uint8_t>, String>::err("Asset payload is too large to load.");
    }

    Vector<std::uint8_t> bytes;
    bytes.resize(storedSize);
    if(!bytes.empty()) {
        std::streamsize readSize = 0;
        if(!toStreamSize(entry.storedSize, readSize)) {
            return Result<Vector<std::uint8_t>, String>::err("Asset payload is too large to load.");
        }
        if(!readExact(in, reinterpret_cast<char *>(bytes.data()), readSize)) {
            return Result<Vector<std::uint8_t>, String>::err("Failed to read asset payload: " +
                                                             entry.name);
        }
    }

    return Result<Vector<std::uint8_t>, String>::ok(std::move(bytes));
}

Result<AssetBundle::Impl *, String> openBundleImpl(FS::Path path, ArrayRef<std::uint8_t> key) {
    auto bundlePath = path.absPath();
    std::ifstream in(bundlePath, std::ios::binary | std::ios::in);
    if(!in.is_open()) {
        return Result<AssetBundle::Impl *, String>::err("Failed to open asset bundle: " + bundlePath);
    }

    auto *impl = new AssetBundle::Impl();
    impl->bundlePath = bundlePath;
    impl->key.assign(key.begin(), key.end());

    std::array<std::uint8_t, sizeof(assetc::BundleMagic)> magic {};
    if(!readExact(in, reinterpret_cast<char *>(magic.data()),
                  static_cast<std::streamsize>(magic.size()))) {
        delete impl;
        return Result<AssetBundle::Impl *, String>::err("Failed to read asset bundle header.");
    }

    in.clear();
    in.seekg(0, std::ios::beg);

    if(!assetc::hasBundleMagic(magic.data())) {
        delete impl;
        return Result<AssetBundle::Impl *, String>::err(
            "Unsupported legacy asset bundle format. Rebuild this bundle with omega-assetc.");
    }

    Result<void *, String> loadResult = loadBundleV2Metadata(in, *impl);
    if(loadResult.isErr()) {
        auto error = loadResult.error();
        delete impl;
        return Result<AssetBundle::Impl *, String>::err(std::move(error));
    }

    return Result<AssetBundle::Impl *, String>::ok(impl);
}

} // namespace

const char *assetTypeName(AssetType type) {
    switch(type) {
        case AssetType::Raw:
            return "Raw";
        case AssetType::Image:
            return "Image";
        case AssetType::Font:
            return "Font";
        case AssetType::Shader:
            return "Shader";
        case AssetType::Text:
            return "Text";
        case AssetType::Audio:
            return "Audio";
        case AssetType::Binary:
            return "Binary";
        case AssetType::Model:
            return "Model";
        case AssetType::Material:
            return "Material";
        case AssetType::Scene:
            return "Scene";
        case AssetType::Animation:
            return "Animation";
        case AssetType::Skeleton:
            return "Skeleton";
    }
    return "Raw";
}

AssetBundle::AssetBundle(Impl *p):impl(p) {

}

AssetBundle::AssetBundle(AssetBundle &&other) noexcept:impl(other.impl) {
    other.impl = nullptr;
}

AssetBundle & AssetBundle::operator=(AssetBundle &&other) noexcept {
    if(this == &other) {
        return *this;
    }

    delete impl;
    impl = other.impl;
    other.impl = nullptr;
    return *this;
}

AssetBundle::~AssetBundle() {
    delete impl;
}

Result<AssetBundle, String> AssetBundle::open(FS::Path path) {
    Vector<std::uint8_t> emptyKey;
    auto openResult = openBundleImpl(path, emptyKey);
    if(openResult.isErr()) {
        return Result<AssetBundle, String>::err(openResult.error());
    }
    return Result<AssetBundle, String>::ok(AssetBundle(openResult.value()));
}

Result<AssetBundle, String> AssetBundle::open(FS::Path path, ArrayRef<std::uint8_t> key) {
    if(key.size() != 32) {
        return Result<AssetBundle, String>::err(
            "Asset bundle keys must be exactly 32 bytes.");
    }

    auto openResult = openBundleImpl(path, key);
    if(openResult.isErr()) {
        return Result<AssetBundle, String>::err(openResult.error());
    }
    return Result<AssetBundle, String>::ok(AssetBundle(openResult.value()));
}

size_t AssetBundle::entryCount() const {
    if(impl == nullptr) {
        return 0;
    }
    return impl->entries.size();
}

Optional<AssetInfo> AssetBundle::info(StrRef name) const {
    if(impl == nullptr) {
        return std::nullopt;
    }

    auto it = impl->entryIndex.find(stringFromRef(name));
    if(it == impl->entryIndex.end()) {
        return std::nullopt;
    }

    auto &entry = impl->entries[it->second];
    size_t rawSize = 0;
    if(!toSizeT(entry.rawSize, rawSize)) {
        return std::nullopt;
    }

    return AssetInfo {entry.name, entry.type, rawSize};
}

bool AssetBundle::contains(StrRef name) const {
    return info(name).has_value();
}

Vector<AssetInfo> AssetBundle::entries() const {
    Vector<AssetInfo> out;
    if(impl == nullptr) {
        return out;
    }

    out.reserve(impl->entries.size());
    for(const auto &entry : impl->entries) {
        size_t rawSize = 0;
        if(!toSizeT(entry.rawSize, rawSize)) {
            continue;
        }
        out.push_back({entry.name, entry.type, rawSize});
    }
    return out;
}

Result<Vector<std::uint8_t>, String> AssetBundle::load(StrRef name) const {
    if(impl == nullptr) {
        return Result<Vector<std::uint8_t>, String>::err("Asset bundle is not open.");
    }

    auto it = impl->entryIndex.find(stringFromRef(name));
    if(it == impl->entryIndex.end()) {
        return Result<Vector<std::uint8_t>, String>::err("Asset not found: " + stringFromRef(name));
    }

    auto &entry = impl->entries[it->second];
    auto isCompressed =
        (entry.flags & static_cast<std::uint32_t>(assetc::AssetEntryFlags::Compressed)) != 0;
    auto isEncrypted =
        (entry.flags & static_cast<std::uint32_t>(assetc::AssetEntryFlags::Encrypted)) != 0;

    if(isCompressed) {
        return Result<Vector<std::uint8_t>, String>::err(
            "Compressed asset bundles are not implemented yet.");
    }

    auto bytesResult = readStoredBytes(*impl, entry);
    if(bytesResult.isErr()) {
        return bytesResult;
    }

    auto bytes = std::move(bytesResult.value());
    if(isEncrypted) {
        if(impl->key.size() != EncryptionKey::KeySize) {
            return Result<Vector<std::uint8_t>, String>::err(
                "Encrypted asset bundle requires a 32-byte key.");
        }

        if(bytes.size() < 16) {
            return Result<Vector<std::uint8_t>, String>::err(
                "Encrypted asset payload is truncated: " + entry.name);
        }

        auto key = EncryptionKey::fromBytes(impl->key.data(), impl->key.size());
        if(key.isErr()) {
            return Result<Vector<std::uint8_t>, String>::err(
                "Failed to load bundle key: " + key.error().message);
        }

        auto nonce = deriveEntryNonce(key.value(), entry.entryHash, entry.name);
        if(nonce.isErr()) {
            return Result<Vector<std::uint8_t>, String>::err(
                "While decrypting \"" + entry.name + "\": " + nonce.error());
        }

        EncryptedData encrypted {};
        encrypted.ciphertext.assign(bytes.begin(), bytes.end() - 16);
        std::copy(bytes.end() - 16, bytes.end(), encrypted.tag.begin());

        auto aad = buildEntryAad(entry.name, entry.type, entry.rawSize, entry.flags);
        auto decrypted = decrypt(key.value(), nonce.value(), encrypted, aad.data(), aad.size());
        if(decrypted.isErr()) {
            return Result<Vector<std::uint8_t>, String>::err(
                "While decrypting \"" + entry.name + "\": " + decrypted.error().message);
        }

        bytes = std::move(decrypted.value());
    }

    if(bytes.size() != entry.rawSize) {
        return Result<Vector<std::uint8_t>, String>::err(
            "Asset entry requires decoding that is not implemented yet: " + entry.name);
    }

    if(entry.hasHash) {
        auto hashResult = sha256(bytes);
        if(hashResult.isErr()) {
            return Result<Vector<std::uint8_t>, String>::err(
                "While verifying \"" + entry.name + "\": " + hashResult.error());
        }

        if(!constantTimeEquals(makeArrayRef(hashResult.value().data(),
                                           hashResult.value().data() + hashResult.value().size()),
                               makeArrayRef(entry.entryHash.data(),
                                            entry.entryHash.data() + entry.entryHash.size()))) {
            return Result<Vector<std::uint8_t>, String>::err(
                "Asset entry hash verification failed: " + entry.name);
        }
    }

    return Result<Vector<std::uint8_t>, String>::ok(std::move(bytes));
}

Result<UniqueHandle<std::istream>, String> AssetBundle::stream(StrRef name) const {
    using ResultT = Result<UniqueHandle<std::istream>, String>;

    if(impl == nullptr) {
        return ResultT::err("Asset bundle is not open.");
    }

    auto it = impl->entryIndex.find(stringFromRef(name));
    if(it == impl->entryIndex.end()) {
        return ResultT::err("Asset not found: " + stringFromRef(name));
    }

    auto &entry = impl->entries[it->second];
    auto isCompressed =
        (entry.flags & static_cast<std::uint32_t>(assetc::AssetEntryFlags::Compressed)) != 0;
    auto isEncrypted =
        (entry.flags & static_cast<std::uint32_t>(assetc::AssetEntryFlags::Encrypted)) != 0;

    if(isCompressed || isEncrypted) {
        return ResultT::err(
            "streaming encrypted/compressed entries not yet supported; use load(): " + entry.name);
    }

    std::streamoff sliceStart = 0;
    if(!toStreamOffset(entry.fileOffset, sliceStart)) {
        return ResultT::err("Asset payload offset is too large to stream.");
    }
    std::streamoff sliceSize = 0;
    if(!toStreamOffset(entry.storedSize, sliceSize)) {
        return ResultT::err("Asset payload is too large to stream.");
    }

    auto buf = std::unique_ptr<SliceStreambuf>(
        new SliceStreambuf(impl->bundlePath, sliceStart, sliceSize));
    if(!buf->ok()) {
        return ResultT::err("Failed to open asset bundle for streaming: " + impl->bundlePath);
    }

    UniqueHandle<std::istream> stream(new SliceIstream(std::move(buf)));
    return ResultT::ok(std::move(stream));
}

Result<String, String> AssetBundle::loadText(StrRef name) const {
    auto assetInfo = info(name);
    if(assetInfo.has_value() &&
       assetInfo->type != AssetType::Text &&
       assetInfo->type != AssetType::Raw) {
        return Result<String, String>::err(
            "Asset is not tagged as text: " + assetInfo->name);
    }

    auto bytesResult = load(name);
    if(bytesResult.isErr()) {
        return Result<String, String>::err(bytesResult.error());
    }

    auto &bytes = bytesResult.value();
    return Result<String, String>::ok(String(bytes.begin(), bytes.end()));
}

OmegaCommon::Map<OmegaCommon::String, AssetLibrary::AssetBuffer> AssetLibrary::assets_res;

void AssetLibrary::loadAssetFile(OmegaCommon::FS::Path &path) {
    auto bundleResult = AssetBundle::open(path);
    if(bundleResult.isErr()) {
        return;
    }

    auto bundle = std::move(bundleResult.value());
    for(const auto &entry : bundle.entries()) {
        auto bytesResult = bundle.load(entry.name);
        if(bytesResult.isErr()) {
            continue;
        }

        auto bytes = std::move(bytesResult.value());
        auto *data = new Byte[bytes.size()];
        if(!bytes.empty()) {
            std::copy(bytes.begin(), bytes.end(), data);
        }

        AssetBuffer buffer {};
        buffer.filesize = bytes.size();
        buffer.data = data;
        buffer.type = entry.type;

        auto inserted = assets_res.insert(std::make_pair(entry.name, std::move(buffer)));
        if(!inserted.second) {
            delete[] data;
        }
    }
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
void loadAssetFile(OmegaCommon::FS::Path path) {
    AssetLibrary::loadAssetFile(path);
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

} // namespace OmegaCommon
