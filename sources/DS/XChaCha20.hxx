#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace AstralDB {
class XChaCha20 {
public:
    XChaCha20(const std::array<uint8_t, 32> &key, const std::array<uint8_t, 24> &nonce, uint32_t counter = 0)
        : Key(key), Nonce(nonce), Counter(counter), Initialized(false) {}

    void Encrypt(const std::vector<uint8_t>& Input, std::vector<uint8_t>& Output) {
        Init();
        Output.resize(Input.size());
        size_t Offset = 0;
        while(Offset < Input.size()) {
            uint32_t Block[16];
            ChaCha20Block(SubKey, Counter, ChaChaNonce, Block);
            Counter++;
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
    uint32_t SubKey[8];
    uint32_t ChaChaNonce[3];
    bool Initialized;

    void Init() {
        if(Initialized) return;
        uint32_t KeyWords[8];
        for(int i = 0; i < 8; i++)
            KeyWords[i] = Load32LE(Key.data() + i * 4);
        uint32_t NonceWords[4];
        for(int i = 0; i < 4; i++)
            NonceWords[i] = Load32LE(Nonce.data() + i * 4);
        HChaCha20(KeyWords, NonceWords, SubKey);
        ChaChaNonce[0] = Load32LE(Nonce.data() + 16);
        ChaChaNonce[1] = Load32LE(Nonce.data() + 20);
        ChaChaNonce[2] = 0;
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

    static void QuarterRound(uint32_t &A, uint32_t &B, uint32_t &C, uint32_t &D) {
        A += B; D ^= A; D = Rotate(D, 16);
        C += D; B ^= C; B = Rotate(B, 12);
        A += B; D ^= A; D = Rotate(D, 8);
        C += D; B ^= C; B = Rotate(B, 7);
    }

    static void ChaCha20Block(const uint32_t Key[8], uint32_t Counter, const uint32_t Nonce[3], uint32_t Out[16]) {
        uint32_t State[16] = {
            0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
            Key[0],     Key[1],     Key[2],     Key[3],
            Key[4],     Key[5],     Key[6],     Key[7],
            Counter,    Nonce[0],   Nonce[1],   Nonce[2]
        };
        uint32_t Working[16];
        std::memcpy(Working, State, sizeof(State));
        for(int i = 0; i < 10; i++) {
            QuarterRound(Working[0], Working[4], Working[8],  Working[12]);
            QuarterRound(Working[1], Working[5], Working[9],  Working[13]);
            QuarterRound(Working[2], Working[6], Working[10], Working[14]);
            QuarterRound(Working[3], Working[7], Working[11], Working[15]);
            QuarterRound(Working[0], Working[5], Working[10], Working[15]);
            QuarterRound(Working[1], Working[6], Working[11], Working[12]);
            QuarterRound(Working[2], Working[7], Working[8],  Working[13]);
            QuarterRound(Working[3], Working[4], Working[9],  Working[14]);
        }
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
        for(int i = 0; i < 10; i++) {
            QuarterRound(State[0], State[4], State[8],  State[12]);
            QuarterRound(State[1], State[5], State[9],  State[13]);
            QuarterRound(State[2], State[6], State[10], State[14]);
            QuarterRound(State[3], State[7], State[11], State[15]);
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