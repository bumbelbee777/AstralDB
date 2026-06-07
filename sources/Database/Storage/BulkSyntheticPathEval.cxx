#include <Database/Storage/BulkSyntheticPathEval.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/SemistructuredLut.hxx>
#include <DS/SimdJsonExtract.hxx>
#include <DS/SimdXmlExtract.hxx>
#include <IO/Job.hxx>
#include <Database/Text/PatternMatch.hxx>

#include <cstring>
#include <future>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace AstralDB {
namespace {

std::uint64_t SplitMix64(std::uint64_t X) noexcept {
	X += 0x9e3779b97f4a7c15ULL;
	X = (X ^ (X >> 30)) * 0xbf58476d1ce4e5b9ULL;
	X = (X ^ (X >> 27)) * 0x94d049bb133111ebULL;
	return X ^ (X >> 31);
}

static const char *kPaymentMethods[] = {"credit_card", "paypal", "cash", "wire", "crypto"};
static constexpr std::uint16_t kThemeLens[2] = {4, 5};
static const char *kThemes[2] = {"dark", "light"};
static constexpr std::uint16_t kBoolLens[2] = {5, 4};
static const char *kBools[2] = {"false", "true"};

std::string_view NormalizeJsonPath(std::string_view Path) noexcept {
	if(Path.size() >= 2 && Path[0] == '$' && Path[1] == '.')
		return Path.substr(2);
	if(!Path.empty() && Path[0] == '$')
		return Path.substr(1);
	return Path;
}

std::vector<std::string_view> SplitDotPath(std::string_view Path) {
	std::vector<std::string_view> Segs;
	while(!Path.empty()) {
		const std::size_t Dot = Path.find('.');
		const std::size_t Len = Dot == std::string_view::npos ? Path.size() : Dot;
		if(Len > 0)
			Segs.push_back(Path.substr(0, Len));
		if(Dot == std::string_view::npos)
			break;
		Path.remove_prefix(Dot + 1);
	}
	return Segs;
}

bool SegsEqual(const std::vector<std::string_view> &Segs,
               std::initializer_list<std::string_view> Expected) noexcept {
	if(Segs.size() != Expected.size())
		return false;
	std::size_t I = 0;
	for(const std::string_view E : Expected) {
		if(Segs[I] != E)
			return false;
		++I;
	}
	return true;
}

std::vector<std::string_view> SplitXmlPath(std::string_view Path) {
	std::vector<std::string_view> Segs;
	while(!Path.empty()) {
		if(Path[0] != '/')
			break;
		Path.remove_prefix(1);
		if(Path.empty())
			break;
		const std::size_t Slash = Path.find('/');
		const std::size_t Len = Slash == std::string_view::npos ? Path.size() : Slash;
		if(Len > 0)
			Segs.push_back(Path.substr(0, Len));
		if(Slash == std::string_view::npos)
			break;
		Path.remove_prefix(Slash + 1);
	}
	return Segs;
}

} // namespace

std::uint64_t BulkSyntheticRowSeed(const int64_t RowId, const std::uint64_t Salt) noexcept {
	return SplitMix64(static_cast<std::uint64_t>(RowId) ^ Salt);
}

BulkSyntheticJsonPathPlan PlanBulkSyntheticJsonPath(const std::string_view Path) noexcept {
	BulkSyntheticJsonPathPlan Plan;
	const std::vector<std::string_view> Segs = SplitDotPath(NormalizeJsonPath(Path));
	if(SegsEqual(Segs, {"active"}))
		Plan.Leaf = BulkSyntheticJsonLeaf::ActiveBool;
	else if(SegsEqual(Segs, {"payment_method"}))
		Plan.Leaf = BulkSyntheticJsonLeaf::PaymentMethod;
	else if(SegsEqual(Segs, {"preferences", "theme"}))
		Plan.Leaf = BulkSyntheticJsonLeaf::PreferencesTheme;
	else if(SegsEqual(Segs, {"preferences", "notifications", "email"}))
		Plan.Leaf = BulkSyntheticJsonLeaf::PreferencesNotificationsEmail;
	return Plan;
}

BulkSyntheticXmlPathPlan PlanBulkSyntheticXmlPath(const std::string_view Path) noexcept {
	BulkSyntheticXmlPathPlan Plan;
	const std::vector<std::string_view> Segs = SplitXmlPath(Path);
	if(SegsEqual(Segs, {"customer", "settings", "language"}))
		Plan.Leaf = BulkSyntheticXmlLeaf::SettingsLanguage;
	return Plan;
}

bool BulkSyntheticJsonExtractPlanned(const int64_t RowId, const BulkSyntheticJsonPathPlan &Plan,
                                     const std::string_view Path, std::string &Out) noexcept {
	switch(Plan.Leaf) {
	case BulkSyntheticJsonLeaf::ActiveBool:
		Out = (BulkSyntheticRowSeed(RowId, 0xAC71E5A5ULL) % 1000) < 670 ? "true" : "false";
		return true;
	case BulkSyntheticJsonLeaf::PaymentMethod: {
		const std::size_t Idx = static_cast<std::size_t>(
		    BulkSyntheticRowSeed(RowId, 0) % (sizeof(kPaymentMethods) / sizeof(kPaymentMethods[0])));
		Out.assign(kPaymentMethods[Idx], std::char_traits<char>::length(kPaymentMethods[Idx]));
		return true;
	}
	case BulkSyntheticJsonLeaf::PreferencesTheme:
		Out = (BulkSyntheticRowSeed(RowId, 0x7E454A5ULL) & 1) ? "dark" : "light";
		return true;
	case BulkSyntheticJsonLeaf::PreferencesNotificationsEmail:
		Out = ((BulkSyntheticRowSeed(RowId, 0xE0A11A5ULL) & 3) != 0) ? "true" : "false";
		return true;
	case BulkSyntheticJsonLeaf::Unknown:
		break;
	}
	const std::string Json = BulkSyntheticJsonCell(RowId, 0);
	return SimdJsonExtract::Extract(Json, NormalizeJsonPath(Path), Out);
}

bool BulkSyntheticXmlExtractPlanned(const int64_t RowId, const BulkSyntheticXmlPathPlan &Plan,
                                    const std::string_view Path, std::string &Out) noexcept {
	(void)RowId;
	if(Plan.Leaf == BulkSyntheticXmlLeaf::SettingsLanguage) {
		Out = "en";
		return true;
	}
	const std::string Xml = BulkSyntheticXmlCell(RowId);
	return SimdXmlExtract::Extract(Xml, Path, Out);
}

void BulkSyntheticJsonFillStrip(const int64_t *RowIds, const std::size_t Count, const std::string_view Path,
                                FormatDoubleSimd::FormattedDoubleColumn &Out) noexcept {
	const BulkSyntheticJsonPathPlan Plan = PlanBulkSyntheticJsonPath(Path);
	Out.Clear();
	Out.Reserve(Count, 12);
	if(Plan.Leaf == BulkSyntheticJsonLeaf::Unknown) {
		for(std::size_t I = 0; I < Count; ++I) {
			std::string Cell;
			const std::string Json = BulkSyntheticJsonCell(RowIds[I], 0);
			if(SimdJsonExtract::Extract(Json, NormalizeJsonPath(Path), Cell))
				SemistructuredLut::AppendStripCell(Out, Cell.data(), static_cast<std::uint16_t>(Cell.size()));
			else
				SemistructuredLut::AppendStripCell(Out, nullptr, 0);
		}
		Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
		return;
	}
	if(Plan.Leaf == BulkSyntheticJsonLeaf::ActiveBool) {
		for(std::size_t I = 0; I < Count; ++I) {
			const std::uint32_t B = static_cast<std::uint32_t>((BulkSyntheticRowSeed(RowIds[I], 0xAC71E5A5ULL) % 1000) < 670);
			SemistructuredLut::AppendStripCell(Out, kBools[B], kBoolLens[B]);
		}
	} else if(Plan.Leaf == BulkSyntheticJsonLeaf::PreferencesTheme) {
		std::size_t I = 0;
#if defined(__AVX2__)
		for(; I + 8 <= Count; I += 8) {
			for(unsigned J = 0; J < 8; ++J) {
				const std::uint32_t T = static_cast<std::uint32_t>(BulkSyntheticRowSeed(RowIds[I + J], 0x7E454A5ULL) & 1u);
				SemistructuredLut::AppendStripCell(Out, kThemes[T], kThemeLens[T]);
			}
		}
#endif
		for(; I < Count; ++I) {
			const std::uint32_t T = static_cast<std::uint32_t>(BulkSyntheticRowSeed(RowIds[I], 0x7E454A5ULL) & 1u);
			SemistructuredLut::AppendStripCell(Out, kThemes[T], kThemeLens[T]);
		}
	} else if(Plan.Leaf == BulkSyntheticJsonLeaf::PreferencesNotificationsEmail) {
		for(std::size_t I = 0; I < Count; ++I) {
			const std::uint32_t B = static_cast<std::uint32_t>((BulkSyntheticRowSeed(RowIds[I], 0xE0A11A5ULL) & 3) != 0);
			SemistructuredLut::AppendStripCell(Out, kBools[B], kBoolLens[B]);
		}
	} else if(Plan.Leaf == BulkSyntheticJsonLeaf::PaymentMethod) {
		for(std::size_t I = 0; I < Count; ++I) {
			const std::size_t Idx = static_cast<std::size_t>(
			    BulkSyntheticRowSeed(RowIds[I], 0) % (sizeof(kPaymentMethods) / sizeof(kPaymentMethods[0])));
			const char *Pm = kPaymentMethods[Idx];
			SemistructuredLut::AppendStripCell(Out, Pm, static_cast<std::uint16_t>(std::strlen(Pm)));
		}
	}
	Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
}

void BulkSyntheticXmlFillStrip(const int64_t *RowIds, const std::size_t Count, const std::string_view Path,
                               FormatDoubleSimd::FormattedDoubleColumn &Out) noexcept {
	(void)RowIds;
	const BulkSyntheticXmlPathPlan Plan = PlanBulkSyntheticXmlPath(Path);
	Out.Clear();
	Out.Reserve(Count, 4);
	if(Plan.Leaf == BulkSyntheticXmlLeaf::SettingsLanguage) {
		for(std::size_t I = 0; I < Count; ++I)
			SemistructuredLut::AppendStripCell(Out, "en", 2);
		Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
		return;
	}
	for(std::size_t I = 0; I < Count; ++I) {
		std::string Cell;
		if(BulkSyntheticXmlExtractPlanned(RowIds[I], Plan, Path, Cell))
			SemistructuredLut::AppendStripCell(Out, Cell.data(), static_cast<std::uint16_t>(Cell.size()));
		else
			SemistructuredLut::AppendStripCell(Out, nullptr, 0);
	}
	Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
}

namespace {

void RegexpFillBand(const int64_t *RowIds, const std::size_t Count, std::string_view Pattern,
                    FormatDoubleSimd::FormattedDoubleColumn &Out) {
	const std::string Pat(Pattern);
	Out.Clear();
	Out.Reserve(Count, 24);
	for(std::size_t I = 0; I < Count; ++I) {
		const std::string Text = BulkSyntheticBioText(RowIds[I]);
		const auto Got = SqlRegexpExtract(Text, Pat, 1, false);
		if(Got)
			SemistructuredLut::AppendStripCell(Out, Got->data(), static_cast<std::uint16_t>(Got->size()));
		else
			SemistructuredLut::AppendStripCell(Out, nullptr, 0);
	}
	Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
}

} // namespace

void BulkSyntheticRegexpFillStrip(const int64_t *RowIds, const std::size_t Count, const std::string_view Pattern,
                                  FormatDoubleSimd::FormattedDoubleColumn &Out) noexcept {
	Out.Clear();
	Out.Reserve(Count, 24);
	const bool Parallel = Count >= 8'192 && JobSystem::Instance().IsRunning();
	if(!Parallel) {
		RegexpFillBand(RowIds, Count, Pattern, Out);
		Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
		return;
	}
	const std::size_t Workers = std::min<std::size_t>(12, std::max<std::size_t>(4, Count / 8'192));
	const std::size_t Band = (Count + Workers - 1) / Workers;
	std::vector<FormatDoubleSimd::FormattedDoubleColumn> Parts(Workers);
	std::vector<std::future<void>> Futs;
	Futs.reserve(Workers);
	std::size_t Launched = 0;
	for(std::size_t W = 0; W < Workers; ++W) {
		const std::size_t Begin = W * Band;
		if(Begin >= Count)
			break;
		const std::size_t End = std::min(Count, Begin + Band);
		const std::size_t Widx = Launched++;
		Futs.push_back(JobSystem::Instance().SubmitAsync([RowIds, Begin, End, Pattern, &Parts, Widx]() {
			RegexpFillBand(RowIds + Begin, End - Begin, Pattern, Parts[Widx]);
		}));
	}
	for(std::future<void> &F : Futs)
		F.wait();
	for(std::size_t W = 0; W < Launched; ++W) {
		const FormatDoubleSimd::FormattedDoubleColumn &Part = Parts[W];
		for(std::size_t I = 0; I < Part.Lengths.size(); ++I) {
			const std::uint32_t Off = Part.Offsets[I];
			const std::uint16_t Len = Part.Lengths[I];
			SemistructuredLut::AppendStripCell(Out, Part.Chars.data() + Off, Len);
		}
	}
	Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
}

} // namespace AstralDB
