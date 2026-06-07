#include <DS/SimdXmlExtract.hxx>

#include <IO/SIMD.hxx>

#include <algorithm>
#include <cctype>
#include <vector>

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
		__m256i M = _mm256_cmpeq_epi8(V, _mm256_set1_epi8(' '));
		M = _mm256_or_si256(M, _mm256_cmpeq_epi8(V, _mm256_set1_epi8('\t')));
		M = _mm256_or_si256(M, _mm256_cmpeq_epi8(V, _mm256_set1_epi8('\n')));
		M = _mm256_or_si256(M, _mm256_cmpeq_epi8(V, _mm256_set1_epi8('\r')));
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

bool SkipXmlDeclOrComment(std::string_view S, std::size_t &I) {
	if(I + 4 <= S.size() && S.substr(I, 4) == "<!--") {
		const std::size_t End = S.find("-->", I + 4);
		if(End == std::string_view::npos)
			return false;
		I = End + 3;
		return true;
	}
	if(I + 2 <= S.size() && S[I] == '<' && S[I + 1] == '?') {
		const std::size_t End = S.find("?>", I + 2);
		if(End == std::string_view::npos)
			return false;
		I = End + 2;
		return true;
	}
	if(I + 9 <= S.size() && S.substr(I, 9) == "<![CDATA[") {
		const std::size_t End = S.find("]]>", I + 9);
		if(End == std::string_view::npos)
			return false;
		I = End + 3;
		return true;
	}
	return false;
}

bool ReadTagName(std::string_view S, std::size_t &I, std::string &Out) {
	Out.clear();
	while(I < S.size() && !IsSpace(S[I]) && S[I] != '>' && S[I] != '/' && S[I] != '=')
		Out.push_back(S[I++]);
	return !Out.empty();
}

bool SkipQuoted(std::string_view S, std::size_t &I) {
	if(I >= S.size())
		return false;
	const char Q = S[I];
	if(Q != '"' && Q != '\'')
		return false;
	++I;
	while(I < S.size()) {
		if(S[I] == Q) {
			++I;
			return true;
		}
		++I;
	}
	return false;
}

bool SkipAttributes(std::string_view S, std::size_t &I) {
	while(I < S.size() && S[I] != '>' && S[I] != '/') {
		SkipWs(S, I);
		if(I >= S.size() || S[I] == '>' || S[I] == '/')
			break;
		while(I < S.size() && S[I] != '=' && !IsSpace(S[I]))
			++I;
		if(I >= S.size() || S[I] != '=')
			return false;
		++I;
		SkipWs(S, I);
		if(!SkipQuoted(S, I))
			return false;
	}
	return true;
}

bool ValidateXmlDocument(std::string_view S) {
	std::vector<std::string> Stack;
	std::size_t I = 0;
	while(I < S.size()) {
		while(SkipXmlDeclOrComment(S, I))
			;
		if(I >= S.size())
			break;
		if(S[I] != '<')
			return false;
		const std::size_t TagStart = I;
		++I;
		const bool Closing = I < S.size() && S[I] == '/';
		if(Closing)
			++I;
		std::string Tag;
		if(!ReadTagName(S, I, Tag))
			return false;
		if(Closing) {
			SkipWs(S, I);
			if(I >= S.size() || S[I] != '>')
				return false;
			++I;
			if(Stack.empty() || Stack.back() != Tag)
				return false;
			Stack.pop_back();
			continue;
		}
		if(!SkipAttributes(S, I))
			return false;
		if(I < S.size() && S[I] == '/') {
			++I;
			if(I >= S.size() || S[I] != '>')
				return false;
			++I;
			continue;
		}
		if(I >= S.size() || S[I] != '>')
			return false;
		++I;
		Stack.push_back(std::move(Tag));
		(void)TagStart;
	}
	return Stack.empty();
}

bool ExtractTagInner(std::string_view Xml, std::string_view Tag, std::string &Out) {
	const std::string Open = std::string("<") + std::string(Tag) + ">";
	const std::string Close = std::string("</") + std::string(Tag) + ">";
	const std::size_t O = FindCharSimd(Xml, 0, '<');
	std::size_t Pos = O;
	while(Pos < Xml.size()) {
		const std::size_t Hit = Xml.find(Open, Pos);
		if(Hit == std::string_view::npos)
			return false;
		const std::size_t Start = Hit + Open.size();
		const std::size_t C = Xml.find(Close, Start);
		if(C == std::string_view::npos)
			return false;
		Out.assign(Xml.substr(Start, C - Start));
		return true;
	}
	return false;
}

bool ExtractAttrValue(std::string_view Xml, std::string_view Tag, std::string_view Attr, std::string &Out) {
	const std::string Open = std::string("<") + std::string(Tag);
	const std::size_t O = Xml.find(Open);
	if(O == std::string_view::npos)
		return false;
	std::size_t I = O + Open.size();
	while(I < Xml.size() && Xml[I] != '>' && Xml[I] != '/') {
		SkipWs(Xml, I);
		std::size_t NameStart = I;
		while(I < Xml.size() && Xml[I] != '=' && !IsSpace(Xml[I]))
			++I;
		const std::string_view Name = Xml.substr(NameStart, I - NameStart);
		if(Name == Attr) {
			if(I >= Xml.size() || Xml[I] != '=')
				return false;
			++I;
			SkipWs(Xml, I);
			if(I >= Xml.size() || (Xml[I] != '"' && Xml[I] != '\''))
				return false;
			const char Q = Xml[I++];
			Out.clear();
			while(I < Xml.size() && Xml[I] != Q)
				Out.push_back(Xml[I++]);
			return true;
		}
		while(I < Xml.size() && Xml[I] != ' ' && Xml[I] != '>' && Xml[I] != '/')
			++I;
		if(I < Xml.size() && Xml[I] == '=') {
			++I;
			SkipWs(Xml, I);
			if(!SkipQuoted(Xml, I))
				return false;
		}
	}
	return false;
}

} // namespace

bool SimdXmlExtract::Extract(const std::string_view Xml, const std::string_view Path, std::string &Out) {
	if(Xml.empty() || Path.empty())
		return false;
	if(Path.size() > 1 && Path[0] == '@') {
		const std::size_t Dot = Path.find('.');
		if(Dot == std::string_view::npos)
			return false;
		const std::string_view Tag = Path.substr(0, Dot);
		const std::string_view Attr = Path.substr(Dot + 1);
		return ExtractAttrValue(Xml, Tag, Attr, Out);
	}
	const std::size_t Dot = Path.find('.');
	if(Dot == std::string_view::npos)
		return ExtractTagInner(Xml, Path, Out);
	std::string Inner;
	if(!ExtractTagInner(Xml, Path.substr(0, Dot), Inner))
		return false;
	return Extract(Inner, Path.substr(Dot + 1), Out);
}

bool SimdXmlExtract::Valid(const std::string_view Xml) noexcept {
	try {
		if(Xml.empty())
			return false;
		return ValidateXmlDocument(Xml);
	} catch(...) {
		return false;
	}
}

} // namespace AstralDB
