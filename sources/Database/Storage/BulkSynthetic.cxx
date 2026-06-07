#include <Database/Storage/BulkSynthetic.hxx>

#include <Database/Types/AdvancedTypes.hxx>

#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/PredicateKind.hxx>

#include <DS/JSON.hxx>

#include <Database/Execution/PlanTypes.hxx>



#include <cmath>

#include <cctype>

#include <functional>

#include <sstream>



namespace AstralDB {

namespace {



std::string UpperToken(std::string S) {

	for(char &C : S)

		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));

	return S;

}



bool TypeTokenContains(std::string_view U, std::string_view Needle) {

	return U.find(Needle) != std::string_view::npos;

}



uint64_t SplitMix64(uint64_t X) noexcept {

	X += 0x9e3779b97f4a7c15ULL;

	X = (X ^ (X >> 30)) * 0xbf58476d1ce4e5b9ULL;

	X = (X ^ (X >> 27)) * 0x94d049bb133111ebULL;

	return X ^ (X >> 31);

}



uint64_t RowSeed(int64_t RowId, uint64_t Salt) noexcept {

	return SplitMix64(static_cast<uint64_t>(RowId) ^ Salt);

}

static const char *kPaymentMethods[] = {"credit_card", "paypal", "cash", "wire", "crypto"};

static const char *kCountries[] = {"US", "UK", "DE", "FR", "JP", "CA", "AU", "BR"};

static const char *kCategories[] = {"electronics", "books", "food", "apparel", "home", "sports", "toys"};

static const char *kReviewPositive[] = {"good excellent purchase", "great value fast shipping", "highly recommend"};

static const char *kReviewNeutral[] = {"average review text", "acceptable product", "as described"};

std::string BulkSyntheticJsonCellImpl(int64_t RowId, uint64_t Salt) {
	const bool Active = (RowSeed(RowId, 0xAC71E5A5ULL) % 1000) < 670;
	const std::size_t Idx = static_cast<std::size_t>(RowSeed(RowId, Salt) % (sizeof(kPaymentMethods) / sizeof(kPaymentMethods[0])));
	std::ostringstream O;
	O << R"({"active":)" << (Active ? "true" : "false") << R"(,"payment_method":")" << kPaymentMethods[Idx]
	  << R"(","preferences":{"theme":")" << ((RowSeed(RowId, 0x7E454A5ULL) & 1) ? "dark" : "light")
	  << R"(","notifications":{"email":)" << (((RowSeed(RowId, 0xE0A11A5ULL) & 3) != 0) ? "true" : "false") << "}}}";
	return std::move(O).str();
}

std::string BulkSyntheticXmlCellImpl(int64_t /*RowId*/) {
	return R"(<customer><settings><language>en</language></settings></customer>)";
}

std::string BulkSyntheticBioTextImpl(int64_t RowId) {
	static const char *kSqlKeywords[] = {"SELECT", "INSERT", "UPDATE", "DELETE"};
	if((RowSeed(RowId, 0x53414C54ULL) % 17) < 4) {
		const char *Kw = kSqlKeywords[static_cast<std::size_t>(RowSeed(RowId, 0xB10ULL) % 4)];
		return std::string("Biography mentioning ") + Kw + " operations for user " + std::to_string(RowId);
	}
	return "Regular customer biography entry number " + std::to_string(RowId);
}

} // namespace

std::string BulkSyntheticJsonCell(const int64_t RowId, const uint64_t Salt) {
	return BulkSyntheticJsonCellImpl(RowId, Salt);
}

std::string BulkSyntheticXmlCell(const int64_t RowId) { return BulkSyntheticXmlCellImpl(RowId); }

std::string BulkSyntheticBioText(const int64_t RowId) { return BulkSyntheticBioTextImpl(RowId); }

std::string BulkSyntheticIsoTimestamp(int64_t EpochSec) {
	const int64_t DaySec = 86400;
	const int64_t Days = EpochSec / DaySec;
	int64_t Rem = EpochSec % DaySec;
	if(Rem < 0)
		Rem += DaySec;
	const int64_t Hour = Rem / 3600;
	Rem %= 3600;
	const int64_t Min = Rem / 60;
	const int64_t Sec = Rem % 60;
	int64_t Z = Days;
	int64_t Y = 1970;
	while(true) {
		const bool Leap = (Y % 4 == 0 && Y % 100 != 0) || (Y % 400 == 0);
		const int64_t YearDays = Leap ? 366 : 365;
		if(Z < YearDays)
			break;
		Z -= YearDays;
		++Y;
	}
	static const int MonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int Mo = 1;
	for(int I = 0; I < 12; ++I) {
		int Dm = MonthDays[I];
		if(I == 1 && ((Y % 4 == 0 && Y % 100 != 0) || (Y % 400 == 0)))
			++Dm;
		if(Z < Dm)
			break;
		Z -= Dm;
		++Mo;
	}
	const int Day = static_cast<int>(Z + 1);
	std::ostringstream O;
	O << Y << '-' << (Mo < 10 ? "0" : "") << Mo << '-' << (Day < 10 ? "0" : "") << Day << ' ' << (Hour < 10 ? "0" : "")
	  << Hour << ':' << (Min < 10 ? "0" : "") << Min << ':' << (Sec < 10 ? "0" : "") << Sec;
	return std::move(O).str();
}

std::string BulkSyntheticCountry(int64_t RowId) {
	return std::string(BulkSyntheticCountryNameByIndex(BulkSyntheticCountryIndex(RowId)));
}

std::string BulkSyntheticCategory(int64_t RowId) {
	return std::string(BulkSyntheticCategoryNameByIndex(BulkSyntheticCategoryIndex(RowId)));
}

std::uint8_t BulkSyntheticCountryIndex(const int64_t RowId) noexcept {
	const auto R = RowSeed(RowId, 0xC0577A5ULL);
	return static_cast<std::uint8_t>(R % BulkSyntheticCountryCount);
}

std::uint8_t BulkSyntheticCategoryIndex(const int64_t RowId) noexcept {
	const auto R = RowSeed(RowId, 0xCA7E60A5ULL);
	return static_cast<std::uint8_t>(R % BulkSyntheticCategoryCount);
}

std::string_view BulkSyntheticCountryNameByIndex(const std::uint8_t Index) noexcept {
	return kCountries[static_cast<std::size_t>(Index) % BulkSyntheticCountryCount];
}

std::string_view BulkSyntheticCategoryNameByIndex(const std::uint8_t Index) noexcept {
	return kCategories[static_cast<std::size_t>(Index) % BulkSyntheticCategoryCount];
}



namespace {



std::string BulkSyntheticReviewText(int64_t RowId) {

	const auto R = RowSeed(RowId, 0xBEA1E7ULL);

	if((R & 3) == 0)

		return kReviewPositive[static_cast<std::size_t>((R >> 4) % 3)];

	return kReviewNeutral[static_cast<std::size_t>((R >> 4) % 3)];

}



} // namespace



namespace {

double DecimalFromWholeFrac(int64_t Whole, int64_t Frac) noexcept {
	if(Frac == 0)
		return static_cast<double>(Whole);
	if(Frac < 10)
		return static_cast<double>(Whole) + static_cast<double>(Frac) * 0.1;
	return static_cast<double>(Whole) + static_cast<double>(Frac) * 0.01;
}

} // namespace

double BulkSyntheticDecimalFromRowId(int64_t RowId) noexcept {
	const int64_t Whole = RowId % 10000;
	const int64_t Frac = (RowId / 100) % 100;
	return DecimalFromWholeFrac(Whole, Frac);
}

bool BulkSyntheticFkPartitionsSingletonPerRow(int64_t Step, int64_t PartitionMod) noexcept {
	return Step == 1 && PartitionMod > 0;
}

void BulkSyntheticFillDecimalByRowRange(int64_t StartId, int64_t Step, std::size_t Count, double *Out) noexcept {
	if(Count == 0 || !Out)
		return;
	if(Step == 1) {
		int64_t Sub = StartId % 100;
		int64_t Whole = StartId % 10000;
		int64_t Frac = (StartId / 100) % 100;
		for(std::size_t I = 0; I < Count; ++I) {
			Out[I] = DecimalFromWholeFrac(Whole, Frac);
			++Sub;
			++Whole;
			if(Whole >= 10000)
				Whole = 0;
			if(Sub >= 100) {
				Sub = 0;
				++Frac;
				if(Frac >= 100)
					Frac = 0;
			}
		}
		return;
	}
	for(std::size_t I = 0; I < Count; ++I) {
		const int64_t RowId = StartId + static_cast<int64_t>(I) * Step;
		Out[I] = BulkSyntheticDecimalFromRowId(RowId);
	}
}

double BulkSyntheticSumDecimalByRowProgression(const int64_t StartRowId, const int64_t Step,
                                               const std::size_t Count) noexcept {
	if(Count == 0)
		return 0.0;
	if(Count <= 32 || Step == 1) {
		double Sum = 0.0;
		for(std::size_t I = 0; I < Count; ++I)
			Sum += BulkSyntheticDecimalFromRowId(StartRowId + static_cast<int64_t>(I) * Step);
		return Sum;
	}
	double Sum = 0.0;
	std::size_t I = 0;
	for(; I + 4 <= Count; I += 4) {
		Sum += BulkSyntheticDecimalFromRowId(StartRowId + static_cast<int64_t>(I) * Step);
		Sum += BulkSyntheticDecimalFromRowId(StartRowId + static_cast<int64_t>(I + 1) * Step);
		Sum += BulkSyntheticDecimalFromRowId(StartRowId + static_cast<int64_t>(I + 2) * Step);
		Sum += BulkSyntheticDecimalFromRowId(StartRowId + static_cast<int64_t>(I + 3) * Step);
	}
	for(; I < Count; ++I)
		Sum += BulkSyntheticDecimalFromRowId(StartRowId + static_cast<int64_t>(I) * Step);
	return Sum;
}



int64_t BulkSyntheticRowIdAt(const ColumnarTable &Col, std::size_t RowIndex) noexcept {

	return Col.BulkStartId + static_cast<int64_t>(RowIndex) * Col.BulkStep;

}

int64_t BulkSyntheticMonthBucketFromRowId(const int64_t RowId) noexcept {
	return (1'704'067'200LL + RowId) / (30LL * 86400LL);
}

bool BulkSyntheticTryInt64Key(const Database::Column &Col, int64_t RowId, int64_t &Out) noexcept {

	const auto Kind = ClassifyBulkColumn(Col, 0, 1);

	switch(Kind) {

	case BulkSyntheticValueKind::PrimaryKey:

		Out = RowId;

		return true;

	case BulkSyntheticValueKind::ForeignKey: {

		const int64_t Mod = BulkSyntheticFkModulus(Col);

		if(Mod <= 0)

			return false;

		Out = ((RowId - 1) % Mod) + 1;

		return true;

	}

	case BulkSyntheticValueKind::Integer:
		Out = RowId;
		return true;

	default:

		return false;

	}

}



bool BulkSyntheticIsHeavyColumn(const Database::Column &Col) noexcept {
	if(ClassifyBulkColumn(Col, 0, 1) == BulkSyntheticValueKind::Advanced)
		return true;
	const SqlStorageKind Storage = ClassifySqlStorage(Col);
	return Storage == SqlStorageKind::Json || Storage == SqlStorageKind::Xml;
}



std::size_t BulkSyntheticColumnIndex(const std::vector<Database::Column> &Schema, std::string_view ColName) noexcept {

	for(std::size_t I = 0; I < Schema.size(); ++I) {

		if(Schema[I].Name == ColName)

			return I;

	}

	return Schema.size();

}



int64_t BulkSyntheticFkModulus(const Database::Column &Col) noexcept {

	if(Col.DeclaredFk.has_value()) {

		const int64_t Seed = static_cast<int64_t>(std::hash<std::string>{}(Col.DeclaredFk->ReferencedTable) ^

		                                          std::hash<std::string>{}(Col.DeclaredFk->ReferencedColumn));

		return 4093 + (Seed % 8191);

	}

	return 997;

}



BulkSyntheticValueKind ClassifyBulkColumn(const Database::Column &Col, std::size_t ColIndex, std::size_t ColCount) {

	if(Col.IsPrimaryKey || Col.IsIdentity)

		return BulkSyntheticValueKind::PrimaryKey;

	if(Col.DeclaredFk.has_value())

		return BulkSyntheticValueKind::ForeignKey;

	if(const auto Desc = AdvancedTypes::ParseTypeSpelling(Col.DefaultValue))

		return BulkSyntheticValueKind::Advanced;

	const std::string U = UpperToken(Col.DefaultValue);

	if(TypeTokenContains(U, "INT") || U == "SERIAL" || U == "BIGSERIAL")

		return ColIndex == 0 ? BulkSyntheticValueKind::PrimaryKey : BulkSyntheticValueKind::Integer;

	if(TypeTokenContains(U, "DECIMAL") || TypeTokenContains(U, "NUMERIC") || TypeTokenContains(U, "DOUBLE") ||

	   TypeTokenContains(U, "REAL") || TypeTokenContains(U, "FLOAT"))

		return BulkSyntheticValueKind::Decimal;

	if(TypeTokenContains(U, "TIMESTAMP") || TypeTokenContains(U, "DATE") || TypeTokenContains(U, "TIME"))

		return BulkSyntheticValueKind::Timestamp;

	(void)ColCount;

	return BulkSyntheticValueKind::Text;

}



std::string BulkSyntheticCellString(const Database::Column &Col, const BulkSyntheticContext &Ctx) {
	const uint64_t ColSalt = static_cast<uint64_t>(std::hash<std::string>{}(Col.Name));
	switch(ClassifySqlStorage(Col)) {
	case SqlStorageKind::Json:
		return BulkSyntheticJsonCell(Ctx.RowId, ColSalt);
	case SqlStorageKind::Xml:
		return BulkSyntheticXmlCell(Ctx.RowId);
	case SqlStorageKind::Text: {
		if((RowSeed(Ctx.RowId, 0x53414C54ULL) % 17) < 4)
			return BulkSyntheticBioText(Ctx.RowId);
		const uint64_t R = RowSeed(Ctx.RowId, ColSalt);
		const unsigned Tier = static_cast<unsigned>(R % 5);
		if(Tier == 0)
			return BulkSyntheticCountry(Ctx.RowId);
		if(Tier == 1)
			return BulkSyntheticBioText(Ctx.RowId);
		if(Tier == 2)
			return BulkSyntheticReviewText(Ctx.RowId);
		return std::string("v_") + std::to_string(Ctx.RowId ^ static_cast<int64_t>(Ctx.ColIndex));
	}
	default:
		break;
	}

	const auto Kind = ClassifyBulkColumn(Col, Ctx.ColIndex, Ctx.ColCount);

	switch(Kind) {

	case BulkSyntheticValueKind::PrimaryKey:

		return std::to_string(Ctx.RowId);

	case BulkSyntheticValueKind::ForeignKey: {

		const int64_t Mod = BulkSyntheticFkModulus(Col);

		const int64_t V = Mod > 0 ? ((Ctx.RowId - 1) % Mod) + 1 : Ctx.RowId;

		return std::to_string(V);

	}

	case BulkSyntheticValueKind::Integer:
		return std::to_string(Ctx.RowId + static_cast<int64_t>(Ctx.ColIndex));

	case BulkSyntheticValueKind::Decimal: {

		std::ostringstream O;

		O << BulkSyntheticDecimalFromRowId(Ctx.RowId);

		return O.str();

	}

	case BulkSyntheticValueKind::Timestamp:

		return BulkSyntheticIsoTimestamp(1'704'067'200LL + Ctx.RowId);

	case BulkSyntheticValueKind::Text:

		return std::string("v_") + std::to_string(Ctx.RowId ^ static_cast<int64_t>(Ctx.ColIndex));

	case BulkSyntheticValueKind::Advanced: {

		const auto Desc = AdvancedTypes::ParseTypeSpelling(Col.DefaultValue);

		if(!Desc)

			return std::to_string(Ctx.RowId);

		switch(Desc->Family) {

		case AdvancedTypes::TypeFamily::Vector: {

			std::vector<double> Vec;

			const std::size_t Len = Desc->VectorLength > 0 ? Desc->VectorLength : 8;

			Vec.reserve(Len);

			for(std::size_t I = 0; I < Len; ++I) {

				const double Phase =

				    static_cast<double>(RowSeed(Ctx.RowId, I) % 1024) / 1024.0;

				Vec.push_back(std::sin(Phase * 6.283185307));

			}

			return AdvancedTypes::FormatVectorCell(Vec);

		}

		case AdvancedTypes::TypeFamily::List: {

			std::vector<std::string> Els;

			Els.push_back("tag_" + std::to_string(RowSeed(Ctx.RowId, 0x7A6ULL) % 17));

			Els.push_back(std::to_string(RowSeed(Ctx.RowId, 0x100ULL) % 100));

			return AdvancedTypes::FormatListCell(Els);

		}

		case AdvancedTypes::TypeFamily::Point: {

			const int64_t Lon = static_cast<int64_t>(RowSeed(Ctx.RowId, 0x10D011ULL) % 360) - 180;

			const int64_t Lat = static_cast<int64_t>(RowSeed(Ctx.RowId, 0x1A7ULL) % 180) - 90;

			return "POINT(" + std::to_string(Lon) + " " + std::to_string(Lat) + ")";

		}

		case AdvancedTypes::TypeFamily::Map:

		case AdvancedTypes::TypeFamily::Struct:

			return AdvancedTypes::FormatStructCell({{"id", std::to_string(Ctx.RowId)},

			                                        {"k", std::to_string(RowSeed(Ctx.RowId, 0x13ULL) % 13)}});

		default:

			return std::to_string(Ctx.RowId);

		}

	}

	}

	return std::to_string(Ctx.RowId);

}

} // namespace AstralDB

