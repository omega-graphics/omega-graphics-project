#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/crypto.h>
#include <openssl/params.h>
#include <openssl/core_names.h>
#include <openssl/kdf.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/ssl.h>
#include <openssl/bn.h>

#include <ctime>
#include <cstring>

#ifdef _WIN32
#include <wincrypt.h>
#else
#include <unistd.h>
#endif

#include "omega-common/crypto.h"

namespace OmegaCommon {

    // ================================================================
    // Helpers
    // ================================================================

    static CryptoError lastOpenSSLError() {
        CryptoError err;
        unsigned long code = ERR_get_error();
        err.code = static_cast<int>(code);
        char buf[256];
        ERR_error_string_n(code, buf, sizeof(buf));
        err.message = buf;
        return err;
    }

    static const EVP_MD *digestMD(DigestAlgorithm alg) {
        switch (alg) {
            case DigestAlgorithm::SHA256: return EVP_sha256();
            case DigestAlgorithm::SHA512: return EVP_sha512();
        }
        return EVP_sha256();
    }

    static const char *digestName(DigestAlgorithm alg) {
        switch (alg) {
            case DigestAlgorithm::SHA256: return "SHA256";
            case DigestAlgorithm::SHA512: return "SHA512";
        }
        return "SHA256";
    }

    static String bioToString(BIO *bio) {
        char *data = nullptr;
        long len = BIO_get_mem_data(bio, &data);
        if (len <= 0 || !data) return {};
        return String(data, static_cast<size_t>(len));
    }

    static int64_t asn1TimeToEpoch(const ASN1_TIME *t) {
        struct tm tm = {};
        if (ASN1_TIME_to_tm(t, &tm) != 1)
            return 0;
#ifdef _WIN32
        return static_cast<int64_t>(_mkgmtime(&tm));
#else
        return static_cast<int64_t>(timegm(&tm));
#endif
    }

    // ================================================================
    // DigestResult (Phase 5 — unchanged)
    // ================================================================

    String DigestResult::hex() const {
        static const char hexChars[] = "0123456789abcdef";
        String result;
        result.reserve(bytes.size() * 2);
        for (auto b : bytes) {
            result.push_back(hexChars[(b >> 4) & 0x0F]);
            result.push_back(hexChars[b & 0x0F]);
        }
        return result;
    }

    // ================================================================
    // Core Crypto (Phase 5 — unchanged)
    // ================================================================

    Result<Vector<std::uint8_t>, CryptoError> randomBytes(size_t n) {
        Vector<std::uint8_t> buf(n);
        if (RAND_bytes(buf.data(), static_cast<int>(n)) != 1)
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        return Result<Vector<std::uint8_t>, CryptoError>::ok(std::move(buf));
    }

    Result<DigestResult, CryptoError> digest(DigestAlgorithm alg, ArrayRef<std::uint8_t> data) {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx)
            return Result<DigestResult, CryptoError>::err(lastOpenSSLError());

        const EVP_MD *md = digestMD(alg);

        if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
            EVP_MD_CTX_free(ctx);
            return Result<DigestResult, CryptoError>::err(lastOpenSSLError());
        }

        if (EVP_DigestUpdate(ctx, data.begin(), data.size()) != 1) {
            EVP_MD_CTX_free(ctx);
            return Result<DigestResult, CryptoError>::err(lastOpenSSLError());
        }

        unsigned char out[EVP_MAX_MD_SIZE];
        unsigned int outLen = 0;
        if (EVP_DigestFinal_ex(ctx, out, &outLen) != 1) {
            EVP_MD_CTX_free(ctx);
            return Result<DigestResult, CryptoError>::err(lastOpenSSLError());
        }

        EVP_MD_CTX_free(ctx);

        DigestResult result;
        result.bytes.assign(out, out + outLen);
        return Result<DigestResult, CryptoError>::ok(std::move(result));
    }

    Result<DigestResult, CryptoError> digest(DigestAlgorithm alg, StrRef text) {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx)
            return Result<DigestResult, CryptoError>::err(lastOpenSSLError());

        const EVP_MD *md = digestMD(alg);

        if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
            EVP_MD_CTX_free(ctx);
            return Result<DigestResult, CryptoError>::err(lastOpenSSLError());
        }

        if (EVP_DigestUpdate(ctx, text.data(), text.size()) != 1) {
            EVP_MD_CTX_free(ctx);
            return Result<DigestResult, CryptoError>::err(lastOpenSSLError());
        }

        unsigned char out[EVP_MAX_MD_SIZE];
        unsigned int outLen = 0;
        if (EVP_DigestFinal_ex(ctx, out, &outLen) != 1) {
            EVP_MD_CTX_free(ctx);
            return Result<DigestResult, CryptoError>::err(lastOpenSSLError());
        }

        EVP_MD_CTX_free(ctx);

        DigestResult result;
        result.bytes.assign(out, out + outLen);
        return Result<DigestResult, CryptoError>::ok(std::move(result));
    }

    Result<Vector<std::uint8_t>, CryptoError> hmac(DigestAlgorithm alg, ArrayRef<std::uint8_t> key, ArrayRef<std::uint8_t> data) {
        EVP_MAC *mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
        if (!mac)
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());

        EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
        if (!ctx) {
            EVP_MAC_free(mac);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        const char *dgstName = digestName(alg);
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, const_cast<char *>(dgstName), 0),
            OSSL_PARAM_construct_end()
        };

        if (EVP_MAC_init(ctx, key.begin(), key.size(), params) != 1) {
            EVP_MAC_CTX_free(ctx);
            EVP_MAC_free(mac);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        if (EVP_MAC_update(ctx, data.begin(), data.size()) != 1) {
            EVP_MAC_CTX_free(ctx);
            EVP_MAC_free(mac);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        size_t outLen = EVP_MAC_CTX_get_mac_size(ctx);
        Vector<std::uint8_t> result(outLen);

        if (EVP_MAC_final(ctx, result.data(), &outLen, result.size()) != 1) {
            EVP_MAC_CTX_free(ctx);
            EVP_MAC_free(mac);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
        result.resize(outLen);
        return Result<Vector<std::uint8_t>, CryptoError>::ok(std::move(result));
    }

    bool constantTimeEquals(ArrayRef<std::uint8_t> a, ArrayRef<std::uint8_t> b) {
        if (a.size() != b.size())
            return false;
        return CRYPTO_memcmp(a.begin(), b.begin(), a.size()) == 0;
    }

    // ================================================================
    // Secure Memory
    // ================================================================

    void secureZero(void *ptr, size_t len) {
        OPENSSL_cleanse(ptr, len);
    }

    // ================================================================
    // EncryptionKey
    // ================================================================

    EncryptionKey::EncryptionKey(SecureVector<std::uint8_t> d) : data_(std::move(d)) {}

    const std::uint8_t *EncryptionKey::data() const { return data_.data(); }
    size_t EncryptionKey::size() const { return data_.size(); }

    Result<EncryptionKey, CryptoError> EncryptionKey::generate() {
        SecureVector<std::uint8_t> buf(KeySize);
        if (RAND_bytes(buf.data(), static_cast<int>(KeySize)) != 1)
            return Result<EncryptionKey, CryptoError>::err(lastOpenSSLError());
        return Result<EncryptionKey, CryptoError>::ok(EncryptionKey(std::move(buf)));
    }

    Result<EncryptionKey, CryptoError> EncryptionKey::fromBytes(const std::uint8_t *data, size_t len) {
        if (len != KeySize)
            return Result<EncryptionKey, CryptoError>::err(CryptoError{-1, "EncryptionKey must be 32 bytes"});
        SecureVector<std::uint8_t> buf(data, data + len);
        return Result<EncryptionKey, CryptoError>::ok(EncryptionKey(std::move(buf)));
    }

    // ================================================================
    // Nonce
    // ================================================================

    Result<Nonce, CryptoError> Nonce::generate() {
        Nonce n;
        if (RAND_bytes(n.bytes.data(), static_cast<int>(NonceSize)) != 1)
            return Result<Nonce, CryptoError>::err(lastOpenSSLError());
        return Result<Nonce, CryptoError>::ok(std::move(n));
    }

    Result<Nonce, CryptoError> Nonce::fromBytes(const std::uint8_t *data, size_t len) {
        if (len != NonceSize)
            return Result<Nonce, CryptoError>::err(CryptoError{-1, "Nonce must be 12 bytes"});
        Nonce n;
        std::memcpy(n.bytes.data(), data, NonceSize);
        return Result<Nonce, CryptoError>::ok(std::move(n));
    }

    // ================================================================
    // AES-256-GCM Encrypt / Decrypt
    // ================================================================

    Result<EncryptedData, CryptoError> encrypt(
        const EncryptionKey &key, const Nonce &nonce,
        const std::uint8_t *plaintext, size_t plaintextLen,
        const std::uint8_t *aad, size_t aadLen)
    {
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (!ctx)
            return Result<EncryptedData, CryptoError>::err(lastOpenSSLError());

        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<EncryptedData, CryptoError>::err(lastOpenSSLError());
        }

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(Nonce::NonceSize), nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<EncryptedData, CryptoError>::err(lastOpenSSLError());
        }

        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.bytes.data()) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<EncryptedData, CryptoError>::err(lastOpenSSLError());
        }

        int tmpLen = 0;

        if (aad && aadLen > 0) {
            if (EVP_EncryptUpdate(ctx, nullptr, &tmpLen, aad, static_cast<int>(aadLen)) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return Result<EncryptedData, CryptoError>::err(lastOpenSSLError());
            }
        }

        EncryptedData out;
        out.ciphertext.resize(plaintextLen);

        if (EVP_EncryptUpdate(ctx, out.ciphertext.data(), &tmpLen, plaintext, static_cast<int>(plaintextLen)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<EncryptedData, CryptoError>::err(lastOpenSSLError());
        }
        int ciphertextLen = tmpLen;

        if (EVP_EncryptFinal_ex(ctx, out.ciphertext.data() + ciphertextLen, &tmpLen) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<EncryptedData, CryptoError>::err(lastOpenSSLError());
        }
        ciphertextLen += tmpLen;
        out.ciphertext.resize(static_cast<size_t>(ciphertextLen));

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out.tag.data()) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<EncryptedData, CryptoError>::err(lastOpenSSLError());
        }

        EVP_CIPHER_CTX_free(ctx);
        return Result<EncryptedData, CryptoError>::ok(std::move(out));
    }

    Result<Vector<std::uint8_t>, CryptoError> decrypt(
        const EncryptionKey &key, const Nonce &nonce,
        const EncryptedData &enc,
        const std::uint8_t *aad, size_t aadLen)
    {
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (!ctx)
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());

        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(Nonce::NonceSize), nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.bytes.data()) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        int tmpLen = 0;

        if (aad && aadLen > 0) {
            if (EVP_DecryptUpdate(ctx, nullptr, &tmpLen, aad, static_cast<int>(aadLen)) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
            }
        }

        Vector<std::uint8_t> out(enc.ciphertext.size());

        if (EVP_DecryptUpdate(ctx, out.data(), &tmpLen,
                enc.ciphertext.data(), static_cast<int>(enc.ciphertext.size())) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }
        int plaintextLen = tmpLen;

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                const_cast<std::uint8_t *>(enc.tag.data())) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        if (EVP_DecryptFinal_ex(ctx, out.data() + plaintextLen, &tmpLen) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return Result<Vector<std::uint8_t>, CryptoError>::err(
                CryptoError{-1, "GCM authentication failed: ciphertext or AAD has been tampered with"});
        }
        plaintextLen += tmpLen;
        out.resize(static_cast<size_t>(plaintextLen));

        EVP_CIPHER_CTX_free(ctx);
        return Result<Vector<std::uint8_t>, CryptoError>::ok(std::move(out));
    }

    // ================================================================
    // Key Derivation — HKDF
    // ================================================================

    Result<Vector<std::uint8_t>, CryptoError> hkdf(
        DigestAlgorithm alg,
        const std::uint8_t *ikm, size_t ikmLen,
        const std::uint8_t *salt, size_t saltLen,
        const std::uint8_t *info, size_t infoLen,
        size_t outputLen)
    {
        EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
        if (!kdf)
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());

        EVP_KDF_CTX *kctx = EVP_KDF_CTX_new(kdf);
        if (!kctx) {
            EVP_KDF_free(kdf);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        const char *dgst = digestName(alg);
        OSSL_PARAM params[6];
        int i = 0;
        params[i++] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
            const_cast<char *>(dgst), 0);
        params[i++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
            const_cast<std::uint8_t *>(ikm), ikmLen);
        if (salt && saltLen > 0)
            params[i++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                const_cast<std::uint8_t *>(salt), saltLen);
        if (info && infoLen > 0)
            params[i++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
                const_cast<std::uint8_t *>(info), infoLen);
        params[i++] = OSSL_PARAM_construct_end();

        Vector<std::uint8_t> out(outputLen);
        if (EVP_KDF_derive(kctx, out.data(), outputLen, params) != 1) {
            EVP_KDF_CTX_free(kctx);
            EVP_KDF_free(kdf);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        EVP_KDF_CTX_free(kctx);
        EVP_KDF_free(kdf);
        return Result<Vector<std::uint8_t>, CryptoError>::ok(std::move(out));
    }

    // ================================================================
    // Key Derivation — PBKDF2
    // ================================================================

    Result<Vector<std::uint8_t>, CryptoError> pbkdf2(
        DigestAlgorithm alg, StrRef password,
        const std::uint8_t *salt, size_t saltLen,
        unsigned iterations, size_t outputLen)
    {
        EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "PBKDF2", nullptr);
        if (!kdf)
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());

        EVP_KDF_CTX *kctx = EVP_KDF_CTX_new(kdf);
        if (!kctx) {
            EVP_KDF_free(kdf);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        const char *dgst = digestName(alg);
        unsigned int iter = iterations;
        OSSL_PARAM params[5];
        int i = 0;
        params[i++] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
            const_cast<char *>(dgst), 0);
        params[i++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
            const_cast<char *>(password.data()), password.size());
        params[i++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
            const_cast<std::uint8_t *>(salt), saltLen);
        params[i++] = OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_ITER, &iter);
        params[i++] = OSSL_PARAM_construct_end();

        Vector<std::uint8_t> out(outputLen);
        if (EVP_KDF_derive(kctx, out.data(), outputLen, params) != 1) {
            EVP_KDF_CTX_free(kctx);
            EVP_KDF_free(kdf);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        EVP_KDF_CTX_free(kctx);
        EVP_KDF_free(kdf);
        return Result<Vector<std::uint8_t>, CryptoError>::ok(std::move(out));
    }

    // ================================================================
    // VerifyingKey (Ed25519)
    // ================================================================

    struct VerifyingKeyImpl {
        EVP_PKEY *pkey;
        explicit VerifyingKeyImpl(EVP_PKEY *p) : pkey(p) {}
        ~VerifyingKeyImpl() { if (pkey) EVP_PKEY_free(pkey); }
    };

    VerifyingKey::VerifyingKey(std::unique_ptr<VerifyingKeyImpl> impl) : impl_(std::move(impl)) {}
    VerifyingKey::~VerifyingKey() = default;
    VerifyingKey::VerifyingKey(VerifyingKey &&) noexcept = default;
    VerifyingKey &VerifyingKey::operator=(VerifyingKey &&) noexcept = default;

    Result<VerifyingKey, CryptoError> VerifyingKey::fromPem(StrRef pem) {
        BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
        if (!bio)
            return Result<VerifyingKey, CryptoError>::err(lastOpenSSLError());

        EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey)
            return Result<VerifyingKey, CryptoError>::err(lastOpenSSLError());

        return Result<VerifyingKey, CryptoError>::ok(
            VerifyingKey(std::make_unique<VerifyingKeyImpl>(pkey)));
    }

    Result<String, CryptoError> VerifyingKey::toPem() const {
        BIO *bio = BIO_new(BIO_s_mem());
        if (!bio)
            return Result<String, CryptoError>::err(lastOpenSSLError());

        if (PEM_write_bio_PUBKEY(bio, impl_->pkey) != 1) {
            BIO_free(bio);
            return Result<String, CryptoError>::err(lastOpenSSLError());
        }

        String result = bioToString(bio);
        BIO_free(bio);
        return Result<String, CryptoError>::ok(std::move(result));
    }

    Result<bool, CryptoError> VerifyingKey::verify(
        const std::uint8_t *message, size_t messageLen,
        const std::uint8_t *signature, size_t signatureLen) const
    {
        EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
        if (!mdctx)
            return Result<bool, CryptoError>::err(lastOpenSSLError());

        if (EVP_DigestVerifyInit(mdctx, nullptr, nullptr, nullptr, impl_->pkey) != 1) {
            EVP_MD_CTX_free(mdctx);
            return Result<bool, CryptoError>::err(lastOpenSSLError());
        }

        int rc = EVP_DigestVerify(mdctx, signature, signatureLen, message, messageLen);
        EVP_MD_CTX_free(mdctx);

        if (rc == 1) return Result<bool, CryptoError>::ok(true);
        if (rc == 0) return Result<bool, CryptoError>::ok(false);
        return Result<bool, CryptoError>::err(lastOpenSSLError());
    }

    // ================================================================
    // SigningKey (Ed25519)
    // ================================================================

    struct SigningKeyImpl {
        EVP_PKEY *pkey;
        explicit SigningKeyImpl(EVP_PKEY *p) : pkey(p) {}
        ~SigningKeyImpl() { if (pkey) EVP_PKEY_free(pkey); }
    };

    SigningKey::SigningKey(std::unique_ptr<SigningKeyImpl> impl) : impl_(std::move(impl)) {}
    SigningKey::~SigningKey() = default;
    SigningKey::SigningKey(SigningKey &&) noexcept = default;
    SigningKey &SigningKey::operator=(SigningKey &&) noexcept = default;

    Result<SigningKey, CryptoError> SigningKey::generate() {
        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(nullptr, "ED25519", nullptr);
        if (!pctx)
            return Result<SigningKey, CryptoError>::err(lastOpenSSLError());

        if (EVP_PKEY_keygen_init(pctx) != 1) {
            EVP_PKEY_CTX_free(pctx);
            return Result<SigningKey, CryptoError>::err(lastOpenSSLError());
        }

        EVP_PKEY *pkey = nullptr;
        if (EVP_PKEY_generate(pctx, &pkey) != 1) {
            EVP_PKEY_CTX_free(pctx);
            return Result<SigningKey, CryptoError>::err(lastOpenSSLError());
        }
        EVP_PKEY_CTX_free(pctx);

        return Result<SigningKey, CryptoError>::ok(
            SigningKey(std::make_unique<SigningKeyImpl>(pkey)));
    }

    Result<SigningKey, CryptoError> SigningKey::fromPem(StrRef pem) {
        BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
        if (!bio)
            return Result<SigningKey, CryptoError>::err(lastOpenSSLError());

        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey)
            return Result<SigningKey, CryptoError>::err(lastOpenSSLError());

        return Result<SigningKey, CryptoError>::ok(
            SigningKey(std::make_unique<SigningKeyImpl>(pkey)));
    }

    Result<String, CryptoError> SigningKey::toPem() const {
        BIO *bio = BIO_new(BIO_s_mem());
        if (!bio)
            return Result<String, CryptoError>::err(lastOpenSSLError());

        if (PEM_write_bio_PrivateKey(bio, impl_->pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            BIO_free(bio);
            return Result<String, CryptoError>::err(lastOpenSSLError());
        }

        String result = bioToString(bio);
        BIO_free(bio);
        return Result<String, CryptoError>::ok(std::move(result));
    }

    Result<VerifyingKey, CryptoError> SigningKey::verifyingKey() const {
        BIO *bio = BIO_new(BIO_s_mem());
        if (!bio)
            return Result<VerifyingKey, CryptoError>::err(lastOpenSSLError());

        if (PEM_write_bio_PUBKEY(bio, impl_->pkey) != 1) {
            BIO_free(bio);
            return Result<VerifyingKey, CryptoError>::err(lastOpenSSLError());
        }

        EVP_PKEY *pubkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pubkey)
            return Result<VerifyingKey, CryptoError>::err(lastOpenSSLError());

        return Result<VerifyingKey, CryptoError>::ok(
            VerifyingKey(std::make_unique<VerifyingKeyImpl>(pubkey)));
    }

    Result<Vector<std::uint8_t>, CryptoError> SigningKey::sign(
        const std::uint8_t *message, size_t messageLen) const
    {
        EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
        if (!mdctx)
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());

        if (EVP_DigestSignInit(mdctx, nullptr, nullptr, nullptr, impl_->pkey) != 1) {
            EVP_MD_CTX_free(mdctx);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        // First call: determine signature length
        size_t sigLen = 0;
        if (EVP_DigestSign(mdctx, nullptr, &sigLen, message, messageLen) != 1) {
            EVP_MD_CTX_free(mdctx);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        Vector<std::uint8_t> sig(sigLen);
        if (EVP_DigestSign(mdctx, sig.data(), &sigLen, message, messageLen) != 1) {
            EVP_MD_CTX_free(mdctx);
            return Result<Vector<std::uint8_t>, CryptoError>::err(lastOpenSSLError());
        }

        EVP_MD_CTX_free(mdctx);
        sig.resize(sigLen);
        return Result<Vector<std::uint8_t>, CryptoError>::ok(std::move(sig));
    }

    // ================================================================
    // Certificate (X.509)
    // ================================================================

    struct CertificateImpl {
        X509 *x509;
        explicit CertificateImpl(X509 *c) : x509(c) {}
        ~CertificateImpl() { if (x509) X509_free(x509); }
    };

    Certificate::Certificate(std::unique_ptr<CertificateImpl> impl) : impl_(std::move(impl)) {}
    Certificate::~Certificate() = default;
    Certificate::Certificate(Certificate &&) noexcept = default;
    Certificate &Certificate::operator=(Certificate &&) noexcept = default;

    Result<Certificate, CryptoError> Certificate::fromPem(StrRef pem) {
        BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
        if (!bio)
            return Result<Certificate, CryptoError>::err(lastOpenSSLError());

        X509 *x509 = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!x509)
            return Result<Certificate, CryptoError>::err(lastOpenSSLError());

        return Result<Certificate, CryptoError>::ok(
            Certificate(std::make_unique<CertificateImpl>(x509)));
    }

    Result<Certificate, CryptoError> Certificate::fromDer(const std::uint8_t *data, size_t len) {
        const unsigned char *p = data;
        X509 *x509 = d2i_X509(nullptr, &p, static_cast<long>(len));
        if (!x509)
            return Result<Certificate, CryptoError>::err(lastOpenSSLError());

        return Result<Certificate, CryptoError>::ok(
            Certificate(std::make_unique<CertificateImpl>(x509)));
    }

    Result<Certificate, CryptoError> Certificate::selfSigned(
        const SigningKey &key, StrRef commonName, unsigned validDays)
    {
        X509 *cert = X509_new();
        if (!cert)
            return Result<Certificate, CryptoError>::err(lastOpenSSLError());

        EVP_PKEY *pkey = key.impl_->pkey;

        X509_set_version(cert, 2); // v3

        // Random 128-bit serial number
        BIGNUM *serial = BN_new();
        if (!serial || !BN_rand(serial, 128, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY)) {
            BN_free(serial);
            X509_free(cert);
            return Result<Certificate, CryptoError>::err(lastOpenSSLError());
        }
        BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(cert));
        BN_free(serial);

        // Validity
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), static_cast<long>(validDays) * 86400L);

        // Subject = Issuer (self-signed)
        X509_NAME *name = X509_NAME_new();
        String cn(commonName.data(), commonName.size());
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
            reinterpret_cast<const unsigned char *>(cn.c_str()), -1, -1, 0);
        X509_set_subject_name(cert, name);
        X509_set_issuer_name(cert, name);
        X509_NAME_free(name);

        // Public key
        X509_set_pubkey(cert, pkey);

        // Basic Constraints: CA:FALSE
        X509V3_CTX v3ctx;
        X509V3_set_ctx_nodb(&v3ctx);
        X509V3_set_ctx(&v3ctx, cert, cert, nullptr, nullptr, 0);
        X509_EXTENSION *ext = X509V3_EXT_nconf_nid(nullptr, &v3ctx, NID_basic_constraints, "CA:FALSE");
        if (ext) {
            X509_add_ext(cert, ext, -1);
            X509_EXTENSION_free(ext);
        }

        // Sign with EdDSA (NULL md for Ed25519)
        if (X509_sign(cert, pkey, nullptr) == 0) {
            X509_free(cert);
            return Result<Certificate, CryptoError>::err(lastOpenSSLError());
        }

        return Result<Certificate, CryptoError>::ok(
            Certificate(std::make_unique<CertificateImpl>(cert)));
    }

    Result<String, CryptoError> Certificate::toPem() const {
        BIO *bio = BIO_new(BIO_s_mem());
        if (!bio)
            return Result<String, CryptoError>::err(lastOpenSSLError());

        if (PEM_write_bio_X509(bio, impl_->x509) != 1) {
            BIO_free(bio);
            return Result<String, CryptoError>::err(lastOpenSSLError());
        }

        String result = bioToString(bio);
        BIO_free(bio);
        return Result<String, CryptoError>::ok(std::move(result));
    }

    String Certificate::subject() const {
        const X509_NAME *name = X509_get_subject_name(impl_->x509);
        if (!name) return {};
        BIO *bio = BIO_new(BIO_s_mem());
        if (!bio) return {};
        X509_NAME_print_ex(bio, name, 0, XN_FLAG_ONELINE & ~ASN1_STRFLGS_ESC_MSB);
        String result = bioToString(bio);
        BIO_free(bio);
        return result;
    }

    String Certificate::issuer() const {
        const X509_NAME *name = X509_get_issuer_name(impl_->x509);
        if (!name) return {};
        BIO *bio = BIO_new(BIO_s_mem());
        if (!bio) return {};
        X509_NAME_print_ex(bio, name, 0, XN_FLAG_ONELINE & ~ASN1_STRFLGS_ESC_MSB);
        String result = bioToString(bio);
        BIO_free(bio);
        return result;
    }

    String Certificate::serialNumber() const {
        const ASN1_INTEGER *serial = X509_get0_serialNumber(impl_->x509);
        if (!serial) return {};
        BIGNUM *bn = ASN1_INTEGER_to_BN(serial, nullptr);
        if (!bn) return {};
        char *hex = BN_bn2hex(bn);
        BN_free(bn);
        if (!hex) return {};
        String result(hex);
        OPENSSL_free(hex);
        return result;
    }

    int64_t Certificate::notBefore() const {
        return asn1TimeToEpoch(X509_get0_notBefore(impl_->x509));
    }

    int64_t Certificate::notAfter() const {
        return asn1TimeToEpoch(X509_get0_notAfter(impl_->x509));
    }

    bool Certificate::isExpired() const {
        return X509_cmp_current_time(X509_get0_notAfter(impl_->x509)) <= 0;
    }

    // ================================================================
    // CertificateStore
    // ================================================================

    struct CertificateStoreImpl {
        X509_STORE *store;
        explicit CertificateStoreImpl(X509_STORE *s) : store(s) {}
        ~CertificateStoreImpl() { if (store) X509_STORE_free(store); }
    };

    CertificateStore::CertificateStore(std::unique_ptr<CertificateStoreImpl> impl) : impl_(std::move(impl)) {}
    CertificateStore::~CertificateStore() = default;
    CertificateStore::CertificateStore(CertificateStore &&) noexcept = default;
    CertificateStore &CertificateStore::operator=(CertificateStore &&) noexcept = default;

    Result<CertificateStore, CryptoError> CertificateStore::create() {
        X509_STORE *store = X509_STORE_new();
        if (!store)
            return Result<CertificateStore, CryptoError>::err(lastOpenSSLError());
        return Result<CertificateStore, CryptoError>::ok(
            CertificateStore(std::make_unique<CertificateStoreImpl>(store)));
    }

    Result<CertificateStore, CryptoError> CertificateStore::system() {
        X509_STORE *store = X509_STORE_new();
        if (!store)
            return Result<CertificateStore, CryptoError>::err(lastOpenSSLError());

#ifdef _WIN32
        // Load from Windows certificate store
        HCERTSTORE hStore = CertOpenSystemStoreW(0, L"ROOT");
        if (hStore) {
            PCCERT_CONTEXT pCert = nullptr;
            while ((pCert = CertEnumCertificatesInStore(hStore, pCert)) != nullptr) {
                const unsigned char *der = pCert->pbCertEncoded;
                X509 *x509 = d2i_X509(nullptr, &der, static_cast<long>(pCert->cbCertEncoded));
                if (x509) {
                    X509_STORE_add_cert(store, x509);
                    X509_free(x509);
                }
            }
            CertCloseStore(hStore, 0);
        }
#else
        // Try common CA bundle paths, then fall back to OpenSSL defaults
        static const char *caPaths[] = {
            "/etc/ssl/cert.pem",                    // macOS, FreeBSD
            "/etc/ssl/certs/ca-certificates.crt",   // Debian, Ubuntu
            "/etc/pki/tls/certs/ca-bundle.crt",     // RHEL, CentOS, Fedora
            "/etc/ssl/ca-bundle.pem",               // openSUSE
            nullptr
        };

        bool loaded = false;
        for (const char **p = caPaths; *p; ++p) {
            if (X509_STORE_load_locations(store, *p, nullptr) == 1) {
                loaded = true;
                break;
            }
        }

        if (!loaded)
            X509_STORE_set_default_paths(store);
#endif

        return Result<CertificateStore, CryptoError>::ok(
            CertificateStore(std::make_unique<CertificateStoreImpl>(store)));
    }

    void CertificateStore::addCertificate(const Certificate &cert) {
        X509_up_ref(cert.impl_->x509);
        X509_STORE_add_cert(impl_->store, cert.impl_->x509);
    }

    Result<bool, CryptoError> CertificateStore::verify(const Certificate &leaf) const {
        return verifyChain(leaf, nullptr, 0);
    }

    Result<bool, CryptoError> CertificateStore::verifyChain(
        const Certificate &leaf,
        const Certificate *intermediates, size_t intermediateCount) const
    {
        X509_STORE_CTX *ctx = X509_STORE_CTX_new();
        if (!ctx)
            return Result<bool, CryptoError>::err(lastOpenSSLError());

        STACK_OF(X509) *chain = nullptr;
        if (intermediates && intermediateCount > 0) {
            chain = sk_X509_new_null();
            for (size_t i = 0; i < intermediateCount; ++i)
                sk_X509_push(chain, intermediates[i].impl_->x509);
        }

        if (X509_STORE_CTX_init(ctx, impl_->store, leaf.impl_->x509, chain) != 1) {
            if (chain) sk_X509_free(chain);
            X509_STORE_CTX_free(ctx);
            return Result<bool, CryptoError>::err(lastOpenSSLError());
        }

        int rc = X509_verify_cert(ctx);
        int errCode = X509_STORE_CTX_get_error(ctx);

        if (chain) sk_X509_free(chain);
        X509_STORE_CTX_free(ctx);

        if (rc == 1)
            return Result<bool, CryptoError>::ok(true);
        if (rc == 0)
            return Result<bool, CryptoError>::ok(false);

        CryptoError err;
        err.code = errCode;
        err.message = X509_verify_cert_error_string(errCode);
        return Result<bool, CryptoError>::err(std::move(err));
    }

    // ================================================================
    // TlsContext
    // ================================================================

    struct TlsContextImpl {
        SSL_CTX *ctx;
        explicit TlsContextImpl(SSL_CTX *c) : ctx(c) {}
        ~TlsContextImpl() { if (ctx) SSL_CTX_free(ctx); }
    };

    TlsContext::TlsContext(std::unique_ptr<TlsContextImpl> impl) : impl_(std::move(impl)) {}
    TlsContext::~TlsContext() = default;
    TlsContext::TlsContext(TlsContext &&) noexcept = default;
    TlsContext &TlsContext::operator=(TlsContext &&) noexcept = default;

    Result<TlsContext, CryptoError> TlsContext::client() {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx)
            return Result<TlsContext, CryptoError>::err(lastOpenSSLError());

        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

        return Result<TlsContext, CryptoError>::ok(
            TlsContext(std::make_unique<TlsContextImpl>(ctx)));
    }

    Result<TlsContext, CryptoError> TlsContext::server(StrRef certPem, StrRef keyPem) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx)
            return Result<TlsContext, CryptoError>::err(lastOpenSSLError());

        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

        // Load certificate from PEM
        BIO *certBio = BIO_new_mem_buf(certPem.data(), static_cast<int>(certPem.size()));
        if (!certBio) {
            SSL_CTX_free(ctx);
            return Result<TlsContext, CryptoError>::err(lastOpenSSLError());
        }
        X509 *cert = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
        BIO_free(certBio);
        if (!cert) {
            SSL_CTX_free(ctx);
            return Result<TlsContext, CryptoError>::err(lastOpenSSLError());
        }
        if (SSL_CTX_use_certificate(ctx, cert) != 1) {
            X509_free(cert);
            SSL_CTX_free(ctx);
            return Result<TlsContext, CryptoError>::err(lastOpenSSLError());
        }
        X509_free(cert);

        // Load private key from PEM
        BIO *keyBio = BIO_new_mem_buf(keyPem.data(), static_cast<int>(keyPem.size()));
        if (!keyBio) {
            SSL_CTX_free(ctx);
            return Result<TlsContext, CryptoError>::err(lastOpenSSLError());
        }
        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
        BIO_free(keyBio);
        if (!pkey) {
            SSL_CTX_free(ctx);
            return Result<TlsContext, CryptoError>::err(lastOpenSSLError());
        }
        if (SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
            EVP_PKEY_free(pkey);
            SSL_CTX_free(ctx);
            return Result<TlsContext, CryptoError>::err(lastOpenSSLError());
        }
        EVP_PKEY_free(pkey);

        // Verify cert/key match
        if (SSL_CTX_check_private_key(ctx) != 1) {
            SSL_CTX_free(ctx);
            return Result<TlsContext, CryptoError>::err(
                CryptoError{-1, "Certificate and private key do not match"});
        }

        return Result<TlsContext, CryptoError>::ok(
            TlsContext(std::make_unique<TlsContextImpl>(ctx)));
    }

    void TlsContext::setVerifyPeer(bool enable) {
        SSL_CTX_set_verify(impl_->ctx, enable ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
    }

    void TlsContext::setCertificateStore(const CertificateStore &store) {
        X509_STORE_up_ref(store.impl_->store);
        SSL_CTX_set_cert_store(impl_->ctx, store.impl_->store);
    }

    void TlsContext::addChainCertificate(const Certificate &cert) {
        X509_up_ref(cert.impl_->x509);
        SSL_CTX_add_extra_chain_cert(impl_->ctx, cert.impl_->x509);
    }

    Result<TlsStream, CryptoError> TlsContext::connect(SocketHandle fd, StrRef hostname) {
        SSL *ssl = SSL_new(impl_->ctx);
        if (!ssl)
            return Result<TlsStream, CryptoError>::err(lastOpenSSLError());

        SSL_set_fd(ssl, static_cast<int>(fd));

        // SNI
        String host(hostname.data(), hostname.size());
        SSL_set_tlsext_host_name(ssl, host.c_str());

        // Hostname verification
        SSL_set1_host(ssl, host.c_str());

        if (SSL_connect(ssl) != 1) {
            CryptoError err = lastOpenSSLError();
            SSL_free(ssl);
            return Result<TlsStream, CryptoError>::err(std::move(err));
        }

        return Result<TlsStream, CryptoError>::ok(
            TlsStream(std::make_unique<TlsStreamImpl>(ssl)));
    }

    Result<TlsStream, CryptoError> TlsContext::accept(SocketHandle fd) {
        SSL *ssl = SSL_new(impl_->ctx);
        if (!ssl)
            return Result<TlsStream, CryptoError>::err(lastOpenSSLError());

        SSL_set_fd(ssl, static_cast<int>(fd));

        if (SSL_accept(ssl) != 1) {
            CryptoError err = lastOpenSSLError();
            SSL_free(ssl);
            return Result<TlsStream, CryptoError>::err(std::move(err));
        }

        return Result<TlsStream, CryptoError>::ok(
            TlsStream(std::make_unique<TlsStreamImpl>(ssl)));
    }

    // ================================================================
    // TlsStream
    // ================================================================

    struct TlsStreamImpl {
        SSL *ssl;
        explicit TlsStreamImpl(SSL *s) : ssl(s) {}
        ~TlsStreamImpl() { if (ssl) SSL_free(ssl); }
    };

    TlsStream::TlsStream(std::unique_ptr<TlsStreamImpl> impl) : impl_(std::move(impl)) {}
    TlsStream::~TlsStream() = default;
    TlsStream::TlsStream(TlsStream &&) noexcept = default;
    TlsStream &TlsStream::operator=(TlsStream &&) noexcept = default;

    Result<size_t, CryptoError> TlsStream::read(std::uint8_t *buf, size_t len) {
        int n = SSL_read(impl_->ssl, buf, static_cast<int>(len));
        if (n <= 0) {
            int sslErr = SSL_get_error(impl_->ssl, n);
            if (sslErr == SSL_ERROR_ZERO_RETURN)
                return Result<size_t, CryptoError>::ok(0); // clean shutdown
            return Result<size_t, CryptoError>::err(
                CryptoError{sslErr, "SSL_read failed"});
        }
        return Result<size_t, CryptoError>::ok(static_cast<size_t>(n));
    }

    Result<size_t, CryptoError> TlsStream::write(const std::uint8_t *buf, size_t len) {
        int n = SSL_write(impl_->ssl, buf, static_cast<int>(len));
        if (n <= 0) {
            int sslErr = SSL_get_error(impl_->ssl, n);
            return Result<size_t, CryptoError>::err(
                CryptoError{sslErr, "SSL_write failed"});
        }
        return Result<size_t, CryptoError>::ok(static_cast<size_t>(n));
    }

    void TlsStream::shutdown() {
        SSL_shutdown(impl_->ssl);
    }

    Optional<Certificate> TlsStream::peerCertificate() const {
        X509 *peer = SSL_get1_peer_certificate(impl_->ssl);
        if (!peer) return {};
        return Certificate(std::make_unique<CertificateImpl>(peer));
    }

    String TlsStream::version() const {
        const char *v = SSL_get_version(impl_->ssl);
        return v ? String(v) : String{};
    }

    String TlsStream::cipherName() const {
        const char *c = SSL_get_cipher_name(impl_->ssl);
        return c ? String(c) : String{};
    }

}
