#pragma once

#include <cstddef>
#include <cstdint>

namespace AstralDB {
namespace MathSciPrimitiveMicrokernels {

/** Symmetric scale \c max(|x|)/127 for INT8 quantization. */
float QuantizeScaleSymF32(const float *Values, std::size_t Count) noexcept;

/** Symmetric scale \c max(|x|)/7 for INT4 quantization. */
float QuantizeScaleSymI4F32(const float *Values, std::size_t Count) noexcept;

void QuantizeSymF32ToI8(const float *In, std::int8_t *Out, std::size_t Count, float Scale) noexcept;
void DequantizeI8ToF32(const std::int8_t *In, float *Out, std::size_t Count, float Scale) noexcept;

/** Pack two signed 4-bit codes per byte (low nibble first). \p Count is float count. */
void QuantizeSymF32ToI4Packed(const float *In, std::uint8_t *Out, std::size_t Count, float Scale) noexcept;
void DequantizeI4PackedToF32(const std::uint8_t *In, float *Out, std::size_t Count, float Scale) noexcept;

float DotProductF32(const float *A, const float *B, std::size_t Count) noexcept;
float SumF32(const float *Values, std::size_t Count) noexcept;

} // namespace MathSciPrimitiveMicrokernels
} // namespace AstralDB
