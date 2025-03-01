#pragma once

#include <DS/XChaCha20.hxx>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <random>
#include <stdexcept>
#include <algorithm>

namespace AstralDB {
class EncryptedString final {
    std::string EncryptedData_;
    std::array<uint8_t, 32> EncryptionKey_;
public:
    EncryptedString(std::string_view Input, std::string_view Key) {
        if(Key.size() != 32) throw std::invalid_argument("Key must be exactly 32 bytes");
        std::copy(Key.begin(), Key.end(), EncryptionKey_.begin());
        EncryptInput(Input);
    }

    EncryptedString(std::string_view Input) {
        std::random_device Rd;
        for(auto &Byte : EncryptionKey_)
            Byte = static_cast<uint8_t>(Rd());
        EncryptInput(Input);
    }

    EncryptedString(std::string_view Data, bool AlreadyEncrypted) {
        if(AlreadyEncrypted) {
            std::copy(Data.begin(), Data.end(), EncryptedData_.begin());
        } else {
            std::array<uint8_t, 24> Nonce{};
            std::copy(Data.begin(), Data.begin() + 24, Nonce.begin());
            std::vector<uint8_t> Ciphertext(Data.begin() + 24, Data.end());
            XChaCha20 Cipher(EncryptionKey_, Nonce);
            std::vector<uint8_t> Plaintext;
            Cipher.Decrypt(Ciphertext, Plaintext);
            std::copy(Plaintext.begin(), Plaintext.end(), EncryptedData_.begin());
        }
    }

    EncryptedString(const EncryptedString &) = delete;
    EncryptedString &operator=(const EncryptedString &) = delete;

    const std::array<uint8_t, 32> &EncryptionKey() const {
        return EncryptionKey_;
    }

    std::string Encrypted() const {
        return EncryptedData_;
    }

    std::string Decrypted() const {
        if(EncryptedData_.size() < 24) return "";
        std::array<uint8_t, 24> Nonce{};
        std::copy(EncryptedData_.begin(), EncryptedData_.begin() + 24, Nonce.begin());
        std::vector<uint8_t> Ciphertext(EncryptedData_.begin() + 24, EncryptedData_.end());
        XChaCha20 Cipher(EncryptionKey_, Nonce);
        std::vector<uint8_t> Plaintext;
        Cipher.Decrypt(Ciphertext, Plaintext);
        return std::string(Plaintext.begin(), Plaintext.end());
    }
private:
    void EncryptInput(std::string_view Input) {
        std::array<uint8_t, 24> Nonce{};
        {
            std::random_device Rd;
            for(auto &Byte : Nonce) Byte = static_cast<uint8_t>(Rd());
        }
        std::vector<uint8_t> Plaintext(Input.begin(), Input.end());
        XChaCha20 Cipher(EncryptionKey_, Nonce);
        std::vector<uint8_t> Ciphertext;
        Cipher.Encrypt(Plaintext, Ciphertext);
        EncryptedData_.resize(Nonce.size() + Ciphertext.size());
        std::copy(Nonce.begin(), Nonce.end(), EncryptedData_.begin());
        std::copy(Ciphertext.begin(), Ciphertext.end(), EncryptedData_.begin() + Nonce.size());
    }
};
}