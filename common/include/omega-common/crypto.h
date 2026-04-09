#include "common.h"

#ifndef OMEGA_COMMON_CRYPTO_H
#define OMEGA_COMMON_CRYPTO_H

#include <array>
#include <cstdint>
#include <memory>

namespace OmegaCommon {

    // ==== Digest and Core Crypto (Phase 5) ====

    enum class DigestAlgorithm {
        SHA256,
        SHA512
    };

    struct CryptoError {
        int code = 0;
        String message;
    };

    struct OMEGACOMMON_EXPORT DigestResult {
        Vector<std::uint8_t> bytes;
        String hex() const;
    };

    OMEGACOMMON_EXPORT Result<Vector<std::uint8_t>, CryptoError> randomBytes(size_t n);

    OMEGACOMMON_EXPORT Result<DigestResult, CryptoError> digest(DigestAlgorithm alg, ArrayRef<std::uint8_t> data);

    OMEGACOMMON_EXPORT Result<DigestResult, CryptoError> digest(DigestAlgorithm alg, StrRef text);

    OMEGACOMMON_EXPORT Result<Vector<std::uint8_t>, CryptoError> hmac(DigestAlgorithm alg, ArrayRef<std::uint8_t> key, ArrayRef<std::uint8_t> data);

    OMEGACOMMON_EXPORT bool constantTimeEquals(ArrayRef<std::uint8_t> a, ArrayRef<std::uint8_t> b);

    // ==== Secure Memory ====

    /// Zeroes memory securely (not optimized away). Backed by OPENSSL_cleanse.
    OMEGACOMMON_EXPORT void secureZero(void *ptr, size_t len);

    /// Allocator that securely zeroes memory before deallocation.
    template<typename T>
    struct SecureAllocator {
        using value_type = T;
        SecureAllocator() noexcept = default;
        template<typename U> SecureAllocator(const SecureAllocator<U> &) noexcept {}
        T *allocate(std::size_t n) { return static_cast<T *>(::operator new(n * sizeof(T))); }
        void deallocate(T *p, std::size_t n) noexcept {
            secureZero(p, n * sizeof(T));
            ::operator delete(p);
        }
        template<typename U> bool operator==(const SecureAllocator<U> &) const noexcept { return true; }
        template<typename U> bool operator!=(const SecureAllocator<U> &) const noexcept { return false; }
    };

    template<typename T>
    using SecureVector = std::vector<T, SecureAllocator<T>>;

    // ==== AES-256-GCM Authenticated Encryption ====

    /// 256-bit symmetric encryption key with secure zeroing on destruction.
    class OMEGACOMMON_EXPORT EncryptionKey {
        SecureVector<std::uint8_t> data_;
        explicit EncryptionKey(SecureVector<std::uint8_t> d);
    public:
        static constexpr size_t KeySize = 32;

        static Result<EncryptionKey, CryptoError> generate();
        static Result<EncryptionKey, CryptoError> fromBytes(const std::uint8_t *data, size_t len);

        const std::uint8_t *data() const;
        size_t size() const;

        EncryptionKey(const EncryptionKey &) = delete;
        EncryptionKey &operator=(const EncryptionKey &) = delete;
        EncryptionKey(EncryptionKey &&) noexcept = default;
        EncryptionKey &operator=(EncryptionKey &&) noexcept = default;
    };

    /// 96-bit nonce / IV for AES-GCM. Caller must never reuse a nonce with the same key.
    struct OMEGACOMMON_EXPORT Nonce {
        static constexpr size_t NonceSize = 12;
        std::array<std::uint8_t, NonceSize> bytes;

        static Result<Nonce, CryptoError> generate();
        static Result<Nonce, CryptoError> fromBytes(const std::uint8_t *data, size_t len);
    };

    /// Ciphertext plus GCM authentication tag.
    struct EncryptedData {
        Vector<std::uint8_t> ciphertext;
        std::array<std::uint8_t, 16> tag;
    };

    /// AES-256-GCM encrypt. AAD (additional authenticated data) is optional.
    OMEGACOMMON_EXPORT Result<EncryptedData, CryptoError> encrypt(
        const EncryptionKey &key, const Nonce &nonce,
        const std::uint8_t *plaintext, size_t plaintextLen,
        const std::uint8_t *aad = nullptr, size_t aadLen = 0);

    /// AES-256-GCM decrypt. Returns error if authentication fails (tampered data).
    OMEGACOMMON_EXPORT Result<Vector<std::uint8_t>, CryptoError> decrypt(
        const EncryptionKey &key, const Nonce &nonce,
        const EncryptedData &enc,
        const std::uint8_t *aad = nullptr, size_t aadLen = 0);

    // ==== Key Derivation ====

    /// HKDF (RFC 5869) extract-and-expand. Salt and info may be nullptr.
    OMEGACOMMON_EXPORT Result<Vector<std::uint8_t>, CryptoError> hkdf(
        DigestAlgorithm alg,
        const std::uint8_t *ikm, size_t ikmLen,
        const std::uint8_t *salt, size_t saltLen,
        const std::uint8_t *info, size_t infoLen,
        size_t outputLen);

    /// PBKDF2 (RFC 8018) password-based key derivation.
    OMEGACOMMON_EXPORT Result<Vector<std::uint8_t>, CryptoError> pbkdf2(
        DigestAlgorithm alg, StrRef password,
        const std::uint8_t *salt, size_t saltLen,
        unsigned iterations, size_t outputLen);

    // ==== Digital Signatures (Ed25519) ====

    struct VerifyingKeyImpl;
    struct SigningKeyImpl;

    /// Ed25519 public verification key. Move-only.
    class OMEGACOMMON_EXPORT VerifyingKey {
        std::unique_ptr<VerifyingKeyImpl> impl_;
        friend class SigningKey;
        explicit VerifyingKey(std::unique_ptr<VerifyingKeyImpl> impl);
    public:
        ~VerifyingKey();
        VerifyingKey(const VerifyingKey &) = delete;
        VerifyingKey &operator=(const VerifyingKey &) = delete;
        VerifyingKey(VerifyingKey &&) noexcept;
        VerifyingKey &operator=(VerifyingKey &&) noexcept;

        static Result<VerifyingKey, CryptoError> fromPem(StrRef pem);
        Result<String, CryptoError> toPem() const;
        Result<bool, CryptoError> verify(const std::uint8_t *message, size_t messageLen,
            const std::uint8_t *signature, size_t signatureLen) const;
    };

    /// Ed25519 private signing key. Move-only, secure zeroing on destruction.
    class OMEGACOMMON_EXPORT SigningKey {
        std::unique_ptr<SigningKeyImpl> impl_;
        friend class Certificate;
        explicit SigningKey(std::unique_ptr<SigningKeyImpl> impl);
    public:
        ~SigningKey();
        SigningKey(const SigningKey &) = delete;
        SigningKey &operator=(const SigningKey &) = delete;
        SigningKey(SigningKey &&) noexcept;
        SigningKey &operator=(SigningKey &&) noexcept;

        static Result<SigningKey, CryptoError> generate();
        static Result<SigningKey, CryptoError> fromPem(StrRef pem);
        Result<String, CryptoError> toPem() const;
        Result<VerifyingKey, CryptoError> verifyingKey() const;
        Result<Vector<std::uint8_t>, CryptoError> sign(const std::uint8_t *message, size_t messageLen) const;
    };

    // ==== X.509 / PKI ====

    struct CertificateImpl;
    struct CertificateStoreImpl;

    /// X.509 certificate. Move-only, wraps an opaque OpenSSL handle.
    class OMEGACOMMON_EXPORT Certificate {
        std::unique_ptr<CertificateImpl> impl_;
        friend class CertificateStore;
        friend class TlsContext;
        friend class TlsStream;
        explicit Certificate(std::unique_ptr<CertificateImpl> impl);
    public:
        ~Certificate();
        Certificate(const Certificate &) = delete;
        Certificate &operator=(const Certificate &) = delete;
        Certificate(Certificate &&) noexcept;
        Certificate &operator=(Certificate &&) noexcept;

        static Result<Certificate, CryptoError> fromPem(StrRef pem);
        static Result<Certificate, CryptoError> fromDer(const std::uint8_t *data, size_t len);
        /// Generate a self-signed certificate using the given Ed25519 signing key.
        static Result<Certificate, CryptoError> selfSigned(
            const SigningKey &key, StrRef commonName, unsigned validDays);

        Result<String, CryptoError> toPem() const;
        String subject() const;
        String issuer() const;
        String serialNumber() const;
        int64_t notBefore() const;
        int64_t notAfter() const;
        bool isExpired() const;
    };

    /// Trusted certificate store for verification. Move-only.
    class OMEGACOMMON_EXPORT CertificateStore {
        std::unique_ptr<CertificateStoreImpl> impl_;
        friend class TlsContext;
        explicit CertificateStore(std::unique_ptr<CertificateStoreImpl> impl);
    public:
        ~CertificateStore();
        CertificateStore(const CertificateStore &) = delete;
        CertificateStore &operator=(const CertificateStore &) = delete;
        CertificateStore(CertificateStore &&) noexcept;
        CertificateStore &operator=(CertificateStore &&) noexcept;

        /// Create an empty store.
        static Result<CertificateStore, CryptoError> create();
        /// Load the operating system's trusted root certificates.
        static Result<CertificateStore, CryptoError> system();

        void addCertificate(const Certificate &cert);
        Result<bool, CryptoError> verify(const Certificate &leaf) const;
        Result<bool, CryptoError> verifyChain(const Certificate &leaf,
            const Certificate *intermediates, size_t intermediateCount) const;
    };

    // ==== TLS ====

#ifdef _WIN32
    using SocketHandle = std::uintptr_t;
#else
    using SocketHandle = int;
#endif

    struct TlsContextImpl;
    struct TlsStreamImpl;

    /// TLS-wrapped stream over an existing socket. Move-only.
    class OMEGACOMMON_EXPORT TlsStream {
        std::unique_ptr<TlsStreamImpl> impl_;
        friend class TlsContext;
        explicit TlsStream(std::unique_ptr<TlsStreamImpl> impl);
    public:
        ~TlsStream();
        TlsStream(const TlsStream &) = delete;
        TlsStream &operator=(const TlsStream &) = delete;
        TlsStream(TlsStream &&) noexcept;
        TlsStream &operator=(TlsStream &&) noexcept;

        Result<size_t, CryptoError> read(std::uint8_t *buf, size_t len);
        Result<size_t, CryptoError> write(const std::uint8_t *buf, size_t len);
        void shutdown();

        Optional<Certificate> peerCertificate() const;
        String version() const;
        String cipherName() const;
    };

    /// TLS context (wraps SSL_CTX). Configures TLS parameters; creates streams.
    class OMEGACOMMON_EXPORT TlsContext {
        std::unique_ptr<TlsContextImpl> impl_;
        explicit TlsContext(std::unique_ptr<TlsContextImpl> impl);
    public:
        ~TlsContext();
        TlsContext(const TlsContext &) = delete;
        TlsContext &operator=(const TlsContext &) = delete;
        TlsContext(TlsContext &&) noexcept;
        TlsContext &operator=(TlsContext &&) noexcept;

        /// Create a TLS 1.2+ client context with system trust store.
        static Result<TlsContext, CryptoError> client();
        /// Create a TLS server context. certPem/keyPem accept any key type (RSA, ECDSA, Ed25519).
        static Result<TlsContext, CryptoError> server(StrRef certPem, StrRef keyPem);

        void setVerifyPeer(bool enable);
        void setCertificateStore(const CertificateStore &store);
        void addChainCertificate(const Certificate &cert);

        /// Perform a TLS client handshake over fd. hostname is used for SNI and verification.
        Result<TlsStream, CryptoError> connect(SocketHandle fd, StrRef hostname);
        /// Perform a TLS server handshake over fd.
        Result<TlsStream, CryptoError> accept(SocketHandle fd);
    };

}

#endif
