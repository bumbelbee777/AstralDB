#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace MathSciVision {

constexpr std::size_t MaxImagePixels = 4096 * 4096;
constexpr std::size_t MaxImageChannels = 4;

struct ImageF32 {
	std::size_t W = 0;
	std::size_t H = 0;
	std::size_t C = 0;
	std::vector<float> Data;
};

/** Parse {@c I[w,h,c]:…} wire cell (normalized floats). */
std::optional<ImageF32> ParseImageCell(std::string_view Cell);

/** Encode {@c I[w,h,c]:…} from normalized floats. */
std::string FormatImageCell(const ImageF32 &Img);

/** Decode hex {@c H:…} or raw-byte blob via stb_image; returns {@c I[w,h,c]:…}. */
std::optional<ImageF32> LoadFromBlob(std::string_view Blob);

std::optional<ImageF32> GrayFromImage(const ImageF32 &Img);
std::optional<ImageF32> ResizeBilinear(const ImageF32 &Img, std::size_t NewW, std::size_t NewH);
std::vector<ImageF32> ExtractPatches(const ImageF32 &Img, std::size_t PatchW, std::size_t PatchH, std::size_t Stride);
std::vector<float> HogLiteFeatures(const ImageF32 &Img, std::size_t CellSize);
std::vector<float> FlattenImage(const ImageF32 &Img);

std::optional<std::string> LoadCellFromReal(const std::string &Blob);
std::optional<std::string> GrayCellFromReal(const std::string &Img);
std::optional<std::string> ResizeCellFromReal(const std::string &Img, const std::string &NewW, const std::string &NewH);
std::optional<std::string> PatchesCellFromReal(const std::string &Img, const std::string &PatchW,
                                               const std::string &PatchH, const std::string &Stride);
std::optional<std::string> HogLiteCellFromReal(const std::string &Img, const std::string &CellSize);
std::optional<std::string> FlattenCellFromReal(const std::string &Img);

} // namespace MathSciVision
} // namespace AstralDB
