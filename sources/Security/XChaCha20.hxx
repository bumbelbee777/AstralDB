#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace AstralDB {
class XChaCha20 {
public:
    // Constructor accepts a 32-byte key and a 24-byte nonce.
    // Optionally, a starting counter (default is 0) can be provided.
    XChaCha20(const std::array<uint8_t, 32> &key, const std::array<uint8_t, 24> &nonce, uint32_t counter = 0)
        : Key(key), Nonce(nonce), Counter(counter), Initialized(false) {}

    void Encrypt(const std::vector<uint8_t>& Input, std::vector<uint8_t>& Output) {
        Init();
        Output.resize(Input.size());
        size_t Offset = 0;

        while(Offset < Input.size()) {
            uint32_t Block[16];
            // Generate a 64-byte keystream block.
            ChaCha20Block(SubKey, Counter, ChaChaNonce, Block);
            Counter++; // Increment counter for each block

            size_t BlockSize = std::min(static_cast<size_t>(64), Input.size() - Offset);
            uint8_t* Keystream = reinterpret_cast<uint8_t*>(Block);
            for(size_t i = 0; i < BlockSize; i++)
                Output[Offset + i] = Input[Offset + i] ^ Keystream[i];
            Offset += BlockSize;
        }
    }

    void Decrypt(const std::vector<uint8_t>& Input, std::vector<uint8_t>& Output) {
        Encrypt(Input, Output);
    }

private:
    std::array<uint8_t, 32> Key;
    std::array<uint8_t, 24> Nonce;
    uint32_t Counter;
    uint32_t SubKey[8];      // Derived from HChaCha20
    uint32_t ChaChaNonce[3]; // For ChaCha20: two words from Nonce and one zero word.
    bool Initialized;

    // One-time initialization: derive the SubKey and ChaChaNonce.
    void Init() {
        if (Initialized)
            return;
        uint32_t KeyWords[8];
        for (int i = 0; i < 8; i++) {
            KeyWords[i] = Load32LE(Key.data() + i * 4);
        }
        uint32_t NonceWords[4];
        for (int i = 0; i < 4; i++) {
            NonceWords[i] = Load32LE(Nonce.data() + i * 4);
        }
        HChaCha20(KeyWords, NonceWords, SubKey);

        // Use the remaining 8 bytes of the nonce as the first two words of the ChaCha20 nonce.
        ChaChaNonce[0] = Load32LE(Nonce.data() + 16);
        ChaChaNonce[1] = Load32LE(Nonce.data() + 20);
        ChaChaNonce[2] = 0; // Per XChaCha20 spec, the final 32 bits are zero.
        Initialized = true;
    }

    static uint32_t Load32LE(const uint8_t* Src) {
        return (static_cast<uint32_t>(Src[0])      ) |
               (static_cast<uint32_t>(Src[1]) <<  8) |
               (static_cast<uint32_t>(Src[2]) << 16) |
               (static_cast<uint32_t>(Src[3]) << 24);
    }

    static inline uint32_t Rotate(uint32_t V, int C) {
        return (V << C) | (V >> (32 - C));
    }

    // QuarterRound operation.
    // Mutates the four 32-bit words passed by reference.
    static void QuarterRound(uint32_t &A, uint32_t &B, uint32_t &C, uint32_t &D) {
        A += B; D ^= A; D = Rotate(D, 16);
        C += D; B ^= C; B = Rotate(B, 12);
        A += B; D ^= A; D = Rotate(D, 8);
        C += D; B ^= C; B = Rotate(B, 7);
    }

    // ChaCha20Block: generates a 64-byte keystream block.
    // Given a key, a counter, and a nonce (three 32-bit words),
    // it produces 16 words of keystream in Out.
    static void ChaCha20Block(const uint32_t Key[8], uint32_t Counter, const uint32_t Nonce[3], uint32_t Out[16]) {
        uint32_t State[16] = {
            0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
            Key[0],     Key[1],     Key[2],     Key[3],
            Key[4],     Key[5],     Key[6],     Key[7],
            Counter,    Nonce[0],   Nonce[1],   Nonce[2]
        };

        uint32_t Working[16];
        std::memcpy(Working, State, sizeof(State));

        for (int i = 0; i < 10; i++) {
            // Column rounds.
            QuarterRound(Working[0], Working[4], Working[8],  Working[12]);
            QuarterRound(Working[1], Working[5], Working[9],  Working[13]);
            QuarterRound(Working[2], Working[6], Working[10], Working[14]);
            QuarterRound(Working[3], Working[7], Working[11], Working[15]);
            // Diagonal rounds.
            QuarterRound(Working[0], Working[5], Working[10], Working[15]);
            QuarterRound(Working[1], Working[6], Working[11], Working[12]);
            QuarterRound(Working[2], Working[7], Working[8],  Working[13]);
            QuarterRound(Working[3], Working[4], Working[9],  Working[14]);
        }
        // Add the original state to the working state to produce the keystream block.
        for(int i = 0; i < 16; i++)
            Out[i] = Working[i] + State[i];
    }

    static void HChaCha20(const uint32_t Key[8], const uint32_t Nonce[4], uint32_t SubKey[8]) {
        uint32_t State[16] = {
            0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
            Key[0],     Key[1],     Key[2],     Key[3],
            Key[4],     Key[5],     Key[6],     Key[7],
            Nonce[0],   Nonce[1],   Nonce[2],   Nonce[3]
        };

        // Perform 20 rounds (10 double rounds) on the state.
        for (int i = 0; i < 10; i++) {
            // Column rounds.
            QuarterRound(State[0], State[4], State[8],  State[12]);
            QuarterRound(State[1], State[5], State[9],  State[13]);
            QuarterRound(State[2], State[6], State[10], State[14]);
            QuarterRound(State[3], State[7], State[11], State[15]);
            // Diagonal rounds.
            QuarterRound(State[0], State[5], State[10], State[15]);
            QuarterRound(State[1], State[6], State[11], State[12]);
            QuarterRound(State[2], State[7], State[8],  State[13]);
            QuarterRound(State[3], State[4], State[9],  State[14]);
        }
        SubKey[0] = State[0];
        SubKey[1] = State[1];
        SubKey[2] = State[2];
        SubKey[3] = State[3];
        SubKey[4] = State[12];
        SubKey[5] = State[13];
        SubKey[6] = State[14];
        SubKey[7] = State[15];
    }
};
}