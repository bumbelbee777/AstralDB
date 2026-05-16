#pragma once

#include <array>
#include <cstdint>

namespace AstralDB {

/** Shared XChaCha20 key for on-disk database snapshots and WAL ciphertext (must match historically). */
inline constexpr std::array<uint8_t, 32> kAtRestXChaChaKey{};

}
