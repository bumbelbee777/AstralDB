#pragma once

#include <Security/XChaCha20.hxx>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <random>
#include <algorithm>

namespace AstralDB {
class EncryptedString final {
    std::string EncryptedData_;
public:
    EncryptedString(std::string_view Input) {
        std::array<uint8_t, 24> Nonce{};
        {
            std::random_device Rd;
            for (auto &Byte : Nonce) {
                Byte = static_cast<uint8_t>(Rd());
            }
        }
        std::array<uint8_t, 32> Key = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
        };
        std::vector<uint8_t> Plaintext(Input.begin(), Input.end());
        XChaCha20 Cipher(Key, Nonce);
        std::vector<uint8_t> Ciphertext;
        Cipher.Encrypt(Plaintext, Ciphertext);
        EncryptedData_.resize(Nonce.size() + Ciphertext.size());
        std::copy(Nonce.begin(), Nonce.end(), EncryptedData_.begin());
        std::copy(Ciphertext.begin(), Ciphertext.end(), EncryptedData_.begin() + Nonce.size());
    }

    std::string Decrypt() const {
        if(EncryptedData_.size() < 24) return "";
        std::array<uint8_t, 24> Nonce{};
        std::copy(EncryptedData_.begin(), EncryptedData_.begin() + 24, Nonce.begin());
        std::vector<uint8_t> Ciphertext(EncryptedData_.begin() + 24, EncryptedData_.end());
        std::array<uint8_t, 32> Key = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
        };
        XChaCha20 Cipher(Key, Nonce);
        std::vector<uint8_t> Plaintext;
        Cipher.Decrypt(Ciphertext, Plaintext);
        return std::string(Plaintext.begin(), Plaintext.end());
    }

    EncryptedString(const EncryptedString &) = delete;
    EncryptedString &operator=(const EncryptedString &) = delete;
};
}