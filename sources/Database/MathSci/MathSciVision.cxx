#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#include <std_image/stb_image.h>

#include <Database/MathSci/MathSciVision.hxx>

#include <Database/MathSci/MathSciSimdUtil.hxx>
#include <Database/Types/AdvancedTypes.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace AstralDB {
namespace MathSciVision {
namespace {

std::optional<double> ToNum(std::string_view S) {
	if(S.empty())
		return std::nullopt;
	try {
		return std::stod(std::string(S));
	} catch(...) {
		return std::nullopt;
	}
}

std::optional<std::size_t> ToSize(std::string_view S) {
	const auto N = ToNum(S);
	if(!N || *N < 0.0)
		return std::nullopt;
	return static_cast<std::size_t>(*N);
}

bool IsHexDigit(char C) { return std::isxdigit(static_cast<unsigned char>(C)) != 0; }

std::optional<std::vector<unsigned char>> DecodeHex(std::string_view Hex) {
	if(Hex.size() % 2 != 0)
		return std::nullopt;
	std::vector<unsigned char> Out;
	Out.reserve(Hex.size() / 2);
	for(std::size_t I = 0; I + 1 < Hex.size(); I += 2) {
		if(!IsHexDigit(Hex[I]) || !IsHexDigit(Hex[I + 1]))
			return std::nullopt;
		const auto Byte = static_cast<unsigned char>(std::stoul(std::string(Hex.substr(I, 2)), nullptr, 16));
		Out.push_back(Byte);
	}
	return Out;
}

std::optional<std::vector<unsigned char>> BlobToBytes(std::string_view Blob) {
	if(Blob.size() >= 2 && Blob[0] == 'H' && Blob[1] == ':')
		return DecodeHex(Blob.substr(2));
	const bool AllHex = !Blob.empty() && std::all_of(Blob.begin(), Blob.end(), IsHexDigit);
	if(AllHex && (Blob.size() % 2) == 0)
		return DecodeHex(Blob);
	std::vector<unsigned char> Out(Blob.begin(), Blob.end());
	return Out;
}

float SampleBilinear(const ImageF32 &Img, float X, float Y, std::size_t C) {
	const float Xc = std::clamp(X, 0.f, static_cast<float>(Img.W - 1));
	const float Yc = std::clamp(Y, 0.f, static_cast<float>(Img.H - 1));
	const std::size_t X0 = static_cast<std::size_t>(Xc);
	const std::size_t Y0 = static_cast<std::size_t>(Yc);
	const std::size_t X1 = std::min(X0 + 1, Img.W - 1);
	const std::size_t Y1 = std::min(Y0 + 1, Img.H - 1);
	const float Tx = Xc - static_cast<float>(X0);
	const float Ty = Yc - static_cast<float>(Y0);
	const auto At = [&](std::size_t Px, std::size_t Py) {
		return Img.Data[(Py * Img.W + Px) * Img.C + C];
	};
	const float V00 = At(X0, Y0);
	const float V10 = At(X1, Y0);
	const float V01 = At(X0, Y1);
	const float V11 = At(X1, Y1);
	const float V0 = V00 + Tx * (V10 - V00);
	const float V1 = V01 + Tx * (V11 - V01);
	return V0 + Ty * (V1 - V0);
}

} // namespace

std::optional<ImageF32> ParseImageCell(std::string_view Cell) {
	if(Cell.size() < 6 || Cell[0] != 'I' || Cell[1] != '[')
		return std::nullopt;
	const std::size_t C1 = Cell.find(',');
	const std::size_t C2 = Cell.find(',', C1 == std::string::npos ? 0 : C1 + 1);
	const std::size_t Close = Cell.find(']');
	if(C1 == std::string::npos || C2 == std::string::npos || Close == std::string::npos || Close + 2 >= Cell.size() ||
	   Cell[Close + 1] != ':')
		return std::nullopt;
	const auto W = ToSize(Cell.substr(2, C1 - 2));
	const auto H = ToSize(Cell.substr(C1 + 1, C2 - C1 - 1));
	const auto C = ToSize(Cell.substr(C2 + 1, Close - C2 - 1));
	if(!W || !H || !C || *C == 0 || *C > MaxImageChannels)
		return std::nullopt;
	const std::size_t Pix = *W * *H * *C;
	if(Pix == 0 || Pix > MaxImagePixels)
		return std::nullopt;
	ImageF32 Img{*W, *H, *C, {}};
	Img.Data.resize(Pix);
	std::string_view Body = Cell.substr(Close + 2);
	std::size_t Pos = 0;
	for(std::size_t I = 0; I < Pix; ++I) {
		if(Pos >= Body.size())
			return std::nullopt;
		std::size_t End = Pos;
		while(End < Body.size() && Body[End] != ',')
			++End;
		const auto V = ToNum(Body.substr(Pos, End - Pos));
		if(!V)
			return std::nullopt;
		Img.Data[I] = static_cast<float>(*V);
		Pos = End + (End < Body.size() ? 1 : 0);
	}
	return Img;
}

std::string FormatImageCell(const ImageF32 &Img) {
	std::string O;
	O.reserve(Img.Data.size() * 8 + 24);
	O += "I[";
	O += std::to_string(Img.W);
	O += ',';
	O += std::to_string(Img.H);
	O += ',';
	O += std::to_string(Img.C);
	O += "]:";
	char Buf[32];
	for(std::size_t I = 0; I < Img.Data.size(); ++I) {
		if(I)
			O += ',';
		std::snprintf(Buf, sizeof(Buf), "%.6g", Img.Data[I]);
		O += Buf;
	}
	return O;
}

std::optional<ImageF32> LoadFromBlob(std::string_view Blob) {
	const auto Bytes = BlobToBytes(Blob);
	if(!Bytes || Bytes->empty())
		return std::nullopt;
	int W = 0;
	int H = 0;
	int C = 0;
	unsigned char *Pixels =
	    stbi_load_from_memory(Bytes->data(), static_cast<int>(Bytes->size()), &W, &H, &C, 0);
	if(!Pixels || W <= 0 || H <= 0 || C <= 0)
		return std::nullopt;
	const std::size_t UW = static_cast<std::size_t>(W);
	const std::size_t UH = static_cast<std::size_t>(H);
	const std::size_t UC = static_cast<std::size_t>(C);
	if(UW * UH * UC > MaxImagePixels || UC > MaxImageChannels) {
		stbi_image_free(Pixels);
		return std::nullopt;
	}
	ImageF32 Img{UW, UH, UC, {}};
	Img.Data.resize(UW * UH * UC);
	for(std::size_t I = 0; I < Img.Data.size(); ++I)
		Img.Data[I] = static_cast<float>(Pixels[I]) / 255.f;
	stbi_image_free(Pixels);
	return Img;
}

std::optional<ImageF32> GrayFromImage(const ImageF32 &Img) {
	if(Img.W == 0 || Img.H == 0 || Img.C == 0)
		return std::nullopt;
	ImageF32 Out{Img.W, Img.H, 1, {}};
	Out.Data.resize(Img.W * Img.H);
	for(std::size_t Y = 0; Y < Img.H; ++Y) {
		for(std::size_t X = 0; X < Img.W; ++X) {
			const std::size_t Base = (Y * Img.W + X) * Img.C;
			float G = 0.f;
			if(Img.C >= 3)
				G = 0.299f * Img.Data[Base] + 0.587f * Img.Data[Base + 1] + 0.114f * Img.Data[Base + 2];
			else
				G = Img.Data[Base];
			Out.Data[Y * Img.W + X] = G;
		}
	}
	return Out;
}

std::optional<ImageF32> ResizeBilinear(const ImageF32 &Img, std::size_t NewW, std::size_t NewH) {
	if(Img.W == 0 || Img.H == 0 || NewW == 0 || NewH == 0 || NewW * NewH > MaxImagePixels)
		return std::nullopt;
	ImageF32 Out{NewW, NewH, Img.C, {}};
	Out.Data.resize(NewW * NewH * Img.C);
	const float Sx = Img.W > 1 ? static_cast<float>(Img.W - 1) / static_cast<float>(NewW - 1) : 0.f;
	const float Sy = Img.H > 1 ? static_cast<float>(Img.H - 1) / static_cast<float>(NewH - 1) : 0.f;
	for(std::size_t Y = 0; Y < NewH; ++Y) {
		for(std::size_t X = 0; X < NewW; ++X) {
			const float SrcX = NewW > 1 ? static_cast<float>(X) * Sx : 0.f;
			const float SrcY = NewH > 1 ? static_cast<float>(Y) * Sy : 0.f;
			for(std::size_t C = 0; C < Img.C; ++C) {
				Out.Data[(Y * NewW + X) * Img.C + C] = SampleBilinear(Img, SrcX, SrcY, C);
			}
		}
	}
	return Out;
}

std::vector<ImageF32> ExtractPatches(const ImageF32 &Img, std::size_t PatchW, std::size_t PatchH, std::size_t Stride) {
	std::vector<ImageF32> Patches;
	if(PatchW == 0 || PatchH == 0 || Stride == 0 || PatchW > Img.W || PatchH > Img.H)
		return Patches;
	for(std::size_t Y = 0; Y + PatchH <= Img.H; Y += Stride) {
		for(std::size_t X = 0; X + PatchW <= Img.W; X += Stride) {
			ImageF32 P{PatchW, PatchH, Img.C, {}};
			P.Data.resize(PatchW * PatchH * Img.C);
			for(std::size_t Py = 0; Py < PatchH; ++Py) {
				for(std::size_t Px = 0; Px < PatchW; ++Px) {
					const std::size_t Src = ((Y + Py) * Img.W + (X + Px)) * Img.C;
					const std::size_t Dst = (Py * PatchW + Px) * Img.C;
					for(std::size_t C = 0; C < Img.C; ++C)
						P.Data[Dst + C] = Img.Data[Src + C];
				}
			}
			Patches.push_back(std::move(P));
		}
	}
	return Patches;
}

std::vector<float> HogLiteFeatures(const ImageF32 &Img, std::size_t CellSize) {
	std::vector<float> Feat;
	if(CellSize == 0)
		return Feat;
	const auto Gray = GrayFromImage(Img);
	if(!Gray)
		return Feat;
	const std::size_t Bins = 6;
	for(std::size_t Cy = 0; Cy + CellSize <= Gray->H; Cy += CellSize) {
		for(std::size_t Cx = 0; Cx + CellSize <= Gray->W; Cx += CellSize) {
			std::vector<float> Hist(Bins, 0.f);
			float MagSum = 0.f;
			for(std::size_t Y = Cy + 1; Y + 1 < Cy + CellSize && Y < Gray->H; ++Y) {
				for(std::size_t X = Cx + 1; X + 1 < Cx + CellSize && X < Gray->W; ++X) {
					const float Gx = Gray->Data[Y * Gray->W + X + 1] - Gray->Data[Y * Gray->W + X - 1];
					const float Gy = Gray->Data[(Y + 1) * Gray->W + X] - Gray->Data[(Y - 1) * Gray->W + X];
					const float Mag = std::sqrt(Gx * Gx + Gy * Gy);
					MagSum += Mag;
					float Ang = std::atan2(Gy, Gx);
					if(Ang < 0.f)
						Ang += static_cast<float>(2.0 * 3.141592653589793);
					const std::size_t Bin =
					    static_cast<std::size_t>(std::min(static_cast<float>(Bins - 1),
					                                      Ang / static_cast<float>(3.141592653589793) * static_cast<float>(Bins)));
					Hist[Bin] += Mag;
				}
			}
			for(float H : Hist)
				Feat.push_back(H);
			Feat.push_back(MagSum);
		}
	}
	return Feat;
}

std::vector<float> FlattenImage(const ImageF32 &Img) { return Img.Data; }

std::optional<std::string> LoadCellFromReal(const std::string &Blob) {
	const auto Img = LoadFromBlob(Blob);
	if(!Img)
		return std::nullopt;
	return FormatImageCell(*Img);
}

std::optional<std::string> GrayCellFromReal(const std::string &ImgCell) {
	const auto Img = ParseImageCell(ImgCell);
	if(!Img)
		return std::nullopt;
	const auto Gray = GrayFromImage(*Img);
	if(!Gray)
		return std::nullopt;
	return FormatImageCell(*Gray);
}

std::optional<std::string> ResizeCellFromReal(const std::string &ImgCell, const std::string &NewW,
                                              const std::string &NewH) {
	const auto Img = ParseImageCell(ImgCell);
	const auto W = ToSize(NewW);
	const auto H = ToSize(NewH);
	if(!Img || !W || !H)
		return std::nullopt;
	const auto Res = ResizeBilinear(*Img, *W, *H);
	if(!Res)
		return std::nullopt;
	return FormatImageCell(*Res);
}

std::optional<std::string> PatchesCellFromReal(const std::string &ImgCell, const std::string &PatchW,
                                               const std::string &PatchH, const std::string &Stride) {
	const auto Img = ParseImageCell(ImgCell);
	const auto Pw = ToSize(PatchW);
	const auto Ph = ToSize(PatchH);
	const auto St = ToSize(Stride);
	if(!Img || !Pw || !Ph || !St)
		return std::nullopt;
	const auto Patches = ExtractPatches(*Img, *Pw, *Ph, *St);
	if(Patches.empty())
		return std::nullopt;
	std::vector<std::string> Cells;
	Cells.reserve(Patches.size());
	for(const auto &P : Patches)
		Cells.push_back(FormatImageCell(P));
	return AdvancedTypes::FormatListCell(Cells);
}

std::optional<std::string> HogLiteCellFromReal(const std::string &ImgCell, const std::string &CellSize) {
	const auto Img = ParseImageCell(ImgCell);
	const auto Cs = ToSize(CellSize);
	if(!Img || !Cs)
		return std::nullopt;
	const auto Feat = HogLiteFeatures(*Img, *Cs);
	if(Feat.empty())
		return std::nullopt;
	std::vector<double> D(Feat.begin(), Feat.end());
	return MathSciSimdUtil::FormatListCellFromDoubles(D);
}

std::optional<std::string> FlattenCellFromReal(const std::string &ImgCell) {
	const auto Img = ParseImageCell(ImgCell);
	if(!Img)
		return std::nullopt;
	std::vector<double> D(Img->Data.begin(), Img->Data.end());
	return MathSciSimdUtil::FormatListCellFromDoubles(D);
}

} // namespace MathSciVision
} // namespace AstralDB
