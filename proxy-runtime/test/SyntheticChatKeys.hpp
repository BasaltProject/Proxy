#pragma once

#include <basalt/wire/auth/SessionKeyPair.hpp>
#include <basalt/wire/auth/SignatureValidator.hpp>
#include <basalt/wire/chat/ChatMessage.hpp>
#include <basalt/wire/protocol/play/ChatPackets.hpp>

#include <array>
#include <cstdint>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace basalt::proxy::runtime::test {

/// A throwaway RSA key pair that signs the way Mojang and a player do; nothing here is a real key.
class SyntheticKey {
public:
    explicit SyntheticKey(unsigned bits) {
        mbedtls_pk_init(&key_);
        if (mbedtls_pk_setup(&key_, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0 ||
            mbedtls_rsa_gen_key(mbedtls_pk_rsa(key_), random, nullptr, bits, 65537) != 0)
            throw std::runtime_error("synthetic key generation failed");
    }

    SyntheticKey(const SyntheticKey&) = delete;
    SyntheticKey& operator=(const SyntheticKey&) = delete;

    ~SyntheticKey() { mbedtls_pk_free(&key_); }

    [[nodiscard]] std::vector<std::uint8_t> publicDer() const {
        std::array<std::uint8_t, 512> scratch{};
        auto* context = const_cast<mbedtls_pk_context*>(&key_);
        const int written = mbedtls_pk_write_pubkey_der(context, scratch.data(), scratch.size());
        if (written <= 0)
            throw std::runtime_error("synthetic key export failed");
        return {scratch.end() - written, scratch.end()};
    }

    [[nodiscard]] std::vector<std::uint8_t> sign(mbedtls_md_type_t digest,
                                                 std::span<const std::uint8_t> payload) const {
        std::array<std::uint8_t, 64> hash{};
        const auto* info = mbedtls_md_info_from_type(digest);
        if (mbedtls_md(info, payload.data(), payload.size(), hash.data()) != 0)
            throw std::runtime_error("synthetic digest failed");
        std::array<std::uint8_t, 256> signature{};
        std::size_t length = 0;
        auto* context = const_cast<mbedtls_pk_context*>(&key_);
        if (mbedtls_pk_sign(context, digest, hash.data(), mbedtls_md_get_size(info), signature.data(), signature.size(),
                            &length, random, nullptr) != 0)
            throw std::runtime_error("synthetic signing failed");
        return {signature.begin(), signature.begin() + static_cast<std::ptrdiff_t>(length)};
    }

private:
    static int random(void*, unsigned char* data, std::size_t size) {
        return wire::auth::randomBytes(std::span{data, size}) ? 0 : -1;
    }

    mbedtls_pk_context key_{};
};

/// A services key and a player key it vouches for, and a way to sign the player's messages in order.
class SyntheticPlayer {
public:
    explicit SyntheticPlayer(wire::common::Uuid profile) : profile_(profile) {
        std::vector<wire::auth::RsaPublicKey> keys;
        keys.push_back(*wire::auth::RsaPublicKey::parseDer(services_.publicDer()));
        keySet_ = std::make_shared<const wire::auth::ServicesKeySet>(
            wire::auth::ServicesKeySet::ofProfileKeys(std::move(keys)));
    }

    [[nodiscard]] std::shared_ptr<const wire::auth::ServicesKeySet> keys() const { return keySet_; }

    [[nodiscard]] wire::protocol::play::ChatSessionUpdatePacket session(bool validKeySignature = true) const {
        wire::protocol::play::ChatSessionUpdatePacket packet;
        packet.session.sessionId = wire::common::Uuid{0x1234, 0x5678};
        packet.session.expiresAt = 4102444800000;
        packet.session.publicKey = player_.publicDer();
        const auto payload =
            wire::auth::profileKeyPayload(profile_, packet.session.expiresAt, packet.session.publicKey);
        const SyntheticKey& signer = validKeySignature ? services_ : player_;
        packet.session.keySignature = signer.sign(MBEDTLS_MD_SHA1, payload);
        return packet;
    }

    [[nodiscard]] wire::protocol::play::MessageSignature sign(std::int32_t index, const wire::chat::SignedBody& body) {
        wire::chat::PlayerChatMessage message;
        message.sender = profile_;
        message.sessionId = wire::common::Uuid{0x1234, 0x5678};
        message.index = index;
        message.body = body;
        const auto bytes = player_.sign(MBEDTLS_MD_SHA256, message.signedPayload());
        wire::protocol::play::MessageSignature signature{};
        std::copy(bytes.begin(), bytes.end(), signature.begin());
        return signature;
    }

private:
    wire::common::Uuid profile_;
    SyntheticKey services_{1024};
    SyntheticKey player_{2048};
    std::shared_ptr<const wire::auth::ServicesKeySet> keySet_;
};

} // namespace basalt::proxy::runtime::test
