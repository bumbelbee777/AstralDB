#include <DS/SimdJsonExtract.hxx>

#include <IO/SIMD.hxx>

#include <algorithm>
#include <cctype>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace AstralDB {

namespace {

bool IsSpace(const char C) { return C == ' ' || C == '\t' || C == '\n' || C == '\r'; }

void SkipWs(std::string_view S, std::size_t &I) {
#if defined(__AVX2__)
	while(I + 32 <= S.size()) {
		const __m256i V = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(S.data() + I));
		const __m256i Sp = _mm256_set1_epi8(' ');
		const __m256i Tab = _mm256_set1_epi8('\t');
		const __m256i Nl = _mm256_set1_epi8('\n');
		const __m256i Cr = _mm256_set1_epi8('\r');
		__m256i M = _mm256_cmpeq_epi8(V, Sp);
		M = _mm256_or_si256(M, _mm256_cmpeq_epi8(V, Tab));
		M = _mm256_or_si256(M, _mm256_cmpeq_epi8(V, Nl));
		M = _mm256_or_si256(M, _mm256_cmpeq_epi8(V, Cr));
		const int Mask = _mm256_movemask_epi8(M);
		if(Mask != static_cast<int>(0xFFFFFFFFu)) {
			I += static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(~Mask)));
			return;
		}
		I += 32;
	}
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
	while(I + 16 <= S.size()) {
		const uint8x16_t V = vld1q_u8(reinterpret_cast<const uint8_t *>(S.data() + I));
		uint8x16_t M = vceqq_u8(V, vdupq_n_u8(' '));
		M = vorrq_u8(M, vceqq_u8(V, vdupq_n_u8('\t')));
		M = vorrq_u8(M, vceqq_u8(V, vdupq_n_u8('\n')));
		M = vorrq_u8(M, vceqq_u8(V, vdupq_n_u8('\r')));
		const std::uint32_t Mask = Simd::NeonMovemaskEq(M);
		if(Mask != 0xFFFFu) {
			I += static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>((~Mask) & 0xFFFFu)));
			return;
		}
		I += 16;
	}
#endif
	while(I < S.size() && IsSpace(S[I]))
		++I;
}

#if defined(__AVX2__)

std::size_t FindCharSimd(std::string_view S, const std::size_t Start, const char Needle) {
	const __m256i N = _mm256_set1_epi8(Needle);
	std::size_t I = Start;
	for(; I + 32 <= S.size(); I += 32) {
		const __m256i V = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(S.data() + I));
		const __m256i Eq = _mm256_cmpeq_epi8(V, N);
		const int Mask = _mm256_movemask_epi8(Eq);
		if(Mask != 0)
			return I + static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(Mask)));
	}
	for(; I < S.size(); ++I) {
		if(S[I] == Needle)
			return I;
	}
	return S.size();
}

#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)

std::size_t FindCharSimd(std::string_view S, const std::size_t Start, const char Needle) {
	const uint8x16_t N = vdupq_n_u8(static_cast<uint8_t>(Needle));
	std::size_t I = Start;
	for(; I + 16 <= S.size(); I += 16) {
		const uint8x16_t V = vld1q_u8(reinterpret_cast<const uint8_t *>(S.data() + I));
		const std::uint32_t Mask = Simd::NeonMovemaskEq(vceqq_u8(V, N));
		if(Mask != 0)
			return I + static_cast<std::size_t>(__builtin_ctz(Mask));
	}
	for(; I < S.size(); ++I) {
		if(S[I] == Needle)
			return I;
	}
	return S.size();
}

#else

std::size_t FindCharSimd(std::string_view S, const std::size_t Start, const char Needle) {
	for(std::size_t I = Start; I < S.size(); ++I) {
		if(S[I] == Needle)
			return I;
	}
	return S.size();
}

#endif

bool ParseStringRaw(std::string_view S, std::size_t &I, std::string &Out) {
	if(I >= S.size() || S[I] != '"')
		return false;
	++I;
	Out.clear();
	while(I < S.size()) {
		const char C = S[I++];
		if(C == '\\') {
			if(I >= S.size())
				return false;
			Out.push_back(S[I++]);
			continue;
		}
		if(C == '"')
			return true;
		Out.push_back(C);
	}
	return false;
}

bool SkipString(std::string_view S, std::size_t &I) {
	if(I >= S.size() || S[I] != '"')
		return false;
	++I;
	while(I < S.size()) {
		const char C = S[I++];
		if(C == '\\') {
			if(I >= S.size())
				return false;
			++I;
			continue;
		}
		if(C == '"')
			return true;
	}
	return false;
}

bool SkipNumber(std::string_view S, std::size_t &I) {
	if(I >= S.size())
		return false;
	if(S[I] == '-')
		++I;
	while(I < S.size() && (std::isdigit(static_cast<unsigned char>(S[I])) || S[I] == '.' || S[I] == 'e' ||
	                       S[I] == 'E' || S[I] == '+' || S[I] == '-'))
		++I;
	return true;
}

bool SkipLiteral(std::string_view S, std::size_t &I, std::string_view Lit) {
	if(I + Lit.size() > S.size() || S.substr(I, Lit.size()) != Lit)
		return false;
	I += Lit.size();
	return true;
}

bool SkipValue(std::string_view S, std::size_t &I);

bool SkipArray(std::string_view S, std::size_t &I) {
	if(I >= S.size() || S[I] != '[')
		return false;
	++I;
	SkipWs(S, I);
	if(I < S.size() && S[I] == ']') {
		++I;
		return true;
	}
	while(I < S.size()) {
		if(!SkipValue(S, I))
			return false;
		SkipWs(S, I);
		if(I >= S.size())
			return false;
		if(S[I] == ']') {
			++I;
			return true;
		}
		if(S[I] != ',')
			return false;
		++I;
		SkipWs(S, I);
	}
	return false;
}

bool SkipObject(std::string_view S, std::size_t &I) {
	if(I >= S.size() || S[I] != '{')
		return false;
	++I;
	SkipWs(S, I);
	if(I < S.size() && S[I] == '}') {
		++I;
		return true;
	}
	while(I < S.size()) {
		if(!SkipString(S, I))
			return false;
		SkipWs(S, I);
		if(I >= S.size() || S[I] != ':')
			return false;
		++I;
		if(!SkipValue(S, I))
			return false;
		SkipWs(S, I);
		if(I >= S.size())
			return false;
		if(S[I] == '}') {
			++I;
			return true;
		}
		if(S[I] != ',')
			return false;
		++I;
		SkipWs(S, I);
	}
	return false;
}

bool SkipValue(std::string_view S, std::size_t &I) {
	SkipWs(S, I);
	if(I >= S.size())
		return false;
	switch(S[I]) {
	case '"':
		return SkipString(S, I);
	case '{':
		return SkipObject(S, I);
	case '[':
		return SkipArray(S, I);
	case 't':
		return SkipLiteral(S, I, "true");
	case 'f':
		return SkipLiteral(S, I, "false");
	case 'n':
		return SkipLiteral(S, I, "null");
	default:
		return SkipNumber(S, I);
	}
}

bool ExtractScalarToString(std::string_view S, std::size_t &I, std::string &Out) {
	SkipWs(S, I);
	if(I >= S.size())
		return false;
	if(S[I] == '"')
		return ParseStringRaw(S, I, Out);
	if(S[I] == 't' && I + 4 <= S.size() && S.substr(I, 4) == "true") {
		I += 4;
		Out = "true";
		return true;
	}
	if(S[I] == 'f' && I + 5 <= S.size() && S.substr(I, 5) == "false") {
		I += 5;
		Out = "false";
		return true;
	}
	if(S[I] == 'n' && I + 4 <= S.size() && S.substr(I, 4) == "null") {
		I += 4;
		Out.clear();
		return true;
	}
	const std::size_t Start = I;
	if(!SkipNumber(S, I))
		return false;
	Out.assign(S.substr(Start, I - Start));
	return true;
}

bool FindObjectFieldValue(std::string_view S, std::string_view Key, std::size_t ObjStart, std::size_t ObjEnd,
                            std::string &Out) {
	std::size_t I = ObjStart;
	while(I < ObjEnd) {
		SkipWs(S, I);
		if(I >= ObjEnd || S[I] == '}')
			break;
		std::string FieldKey;
		if(!ParseStringRaw(S, I, FieldKey))
			return false;
		SkipWs(S, I);
		if(I >= ObjEnd || S[I] != ':')
			return false;
		++I;
		if(FieldKey == Key) {
			if(!ExtractScalarToString(S, I, Out)) {
				const std::size_t ValStart = I;
				if(!SkipValue(S, I))
					return false;
				Out.assign(S.substr(ValStart, I - ValStart));
				while(!Out.empty() && IsSpace(Out.back()))
					Out.pop_back();
			}
			return true;
		}
		if(!SkipValue(S, I))
			return false;
		SkipWs(S, I);
		if(I < ObjEnd && S[I] == ',')
			++I;
	}
	return false;
}

bool ExtractNestedPath(std::string_view Json, std::string_view Path, std::string &Out) {
	std::size_t PathPos = 0;
	std::string_view Cur = Json;
	while(true) {
		const std::size_t Dot = Path.find('.', PathPos);
		const std::string_view Key =
		    Path.substr(PathPos, Dot == std::string_view::npos ? Path.size() - PathPos : Dot - PathPos);
		if(Key.empty())
			return false;
		std::size_t I = 0;
		SkipWs(Cur, I);
		if(I >= Cur.size() || Cur[I] != '{')
			return false;
		const std::size_t ObjStart = I + 1;
		const std::size_t Close = FindCharSimd(Cur, ObjStart, '}');
		if(Close >= Cur.size())
			return false;
		std::string Val;
		if(!FindObjectFieldValue(Cur, Key, ObjStart, Close, Val))
			return false;
		if(Dot == std::string_view::npos) {
			Out = std::move(Val);
			return true;
		}
		Cur = Val;
		PathPos = Dot + 1;
	}
}

bool ValidateJson(std::string_view S) {
	std::size_t I = 0;
	SkipWs(S, I);
	if(!SkipValue(S, I))
		return false;
	SkipWs(S, I);
	return I == S.size();
}

} // namespace

bool SimdJsonExtract::Extract(const std::string_view Json, const std::string_view Path, std::string &Out) {
	if(Json.empty() || Path.empty())
		return false;
	std::size_t I = 0;
	SkipWs(Json, I);
	if(I >= Json.size() || Json[I] != '{')
		return false;
	return ExtractNestedPath(Json, Path, Out);
}

void SimdJsonExtract::ExtractBatch(const std::vector<std::string_view> &Jsons, const std::string_view Path,
                                   std::vector<std::string> &Out) {
	Out.resize(Jsons.size());
	for(std::size_t I = 0; I < Jsons.size(); ++I) {
		std::string Val;
		if(!Extract(Jsons[I], Path, Val))
			Val.clear();
		Out[I] = std::move(Val);
	}
}

bool SimdJsonExtract::Valid(const std::string_view Json) noexcept {
	try {
		return ValidateJson(Json);
	} catch(...) {
		return false;
	}
}

} // namespace AstralDB
