#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <random>
#include <cmath>
#include <cstdint>

#if defined(__MINGW32__) || (__MINGW64__) || (__WIN32__) || (__WIN64__) || (_MSC_VER) || (__MSVC__)
static constexpr double M_PI = 3.14159265358979323846; //Since MSVC doesn't have M_PI for whatever god forsaken reason
#endif

namespace AstralDB {
class EncryptedString final {
    std::string EncryptedData_;
    std::vector<uint8_t> Keys_;
    std::vector<uint8_t> Offsets_;
    std::vector<uint8_t> Rotations_;

    static uint8_t RotateLeft(uint8_t Value, uint8_t Shift) {
        return static_cast<uint8_t>((Value << Shift) | (Value >> (8 - Shift)));
    }

    static uint8_t RotateRight(uint8_t Value, uint8_t Shift) {
        return static_cast<uint8_t>((Value >> Shift) | (Value << (8 - Shift)));
    }
public:
    //This is a custom yet really naïve encryption algorithm since I can't be asked on overengineering this
    //Each character is processed with a random key (for XOR) and a random angle, which is then used to compute an offset
    //Then we scale up the sine of said angle to the range of a byte [0, 255] then XOR'd with a prime
    //Finally, we ROL the XOR-result (whose amount is derived from the key and index)
    EncryptedString(std::string_view Input, bool AlreadyEncrypted=false) {
        if(AlreadyEncrypted) {
            EncryptedData_ = Input;
            return;
        }
    
        std::random_device RandomDevice;
        std::mt19937 Engine(RandomDevice());
        std::uniform_int_distribution<> KeyDistribution(0, 255);
        std::uniform_real_distribution<double> AngleDistribution(0.0, 2 * M_PI);
        
        EncryptedData_.resize(Input.size());
        Keys_.resize(Input.size());
        Offsets_.resize(Input.size());
        Rotations_.resize(Input.size());
        
        const uint8_t Prime = 97;
        
        for(size_t Index = 0; Index < Input.size(); ++Index) {
            uint16_t Key = static_cast<uint16_t>(KeyDistribution(Engine)) 
                    | (static_cast<uint16_t>(KeyDistribution(Engine)) << 8);
            Keys_[Index] = Key;
            double Angle = AngleDistribution(Engine);
            uint8_t Offset = (static_cast<uint8_t>(std::floor(std::abs(std::sin(Angle + Index)) * 255)) ^ Prime) % 251;
            Offsets_[Index] = Offset;
            uint8_t Rotation = static_cast<uint8_t>((Key * Index + 13) % 8);
            Rotations_[Index] = Rotation;
            uint8_t Plaintext = static_cast<uint8_t>(Input[Index]);
            uint8_t Step1 = Plaintext ^ Key;
            uint8_t Step2 = static_cast<uint8_t>((Step1 + Offset) & 0xFF);
            uint8_t Cipher = RotateLeft(Step2, Rotation);
            EncryptedData_[Index] = static_cast<char>(Cipher);
        }
    }

    std::string_view Encrypted() const {
        return EncryptedData_;
    }

    std::string Decrypted() const {
        std::string Output;
        Output.resize(EncryptedData_.size());
        // Reverse the encryption process for each character:
        // 1. Rotate right by the stored rotation value (computed as (Key * Index + 13) % 8).
        // 2. Subtract the offset modulo 256.
        // 3. XOR with the stored key to recover the original plaintext.
        for(size_t Index = 0; Index < EncryptedData_.size(); ++Index) {
            uint8_t Rotation = Rotations_[Index];
            uint8_t RotatedValue = RotateRight(static_cast<uint8_t>(EncryptedData_[Index]), Rotation);
            uint8_t ValueAfterOffset = static_cast<uint8_t>((RotatedValue + 256 - Offsets_[Index]) & 0xFF);
            uint8_t Plaintext = ValueAfterOffset ^ Keys_[Index];
            Output[Index] = static_cast<char>(Plaintext);
        }
        return Output;
    }

    EncryptedString(const EncryptedString &) = delete;
    EncryptedString &operator=(const EncryptedString &) = delete;
};
}