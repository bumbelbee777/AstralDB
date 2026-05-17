#include <Database/TimeSeries.hxx>

#include <cctype>
#include <sstream>

namespace AstralDB {
namespace TimeSeries {
namespace {

bool IsDigit(char C) { return C >= '0' && C <= '9'; }

bool IsLeap(int Y) { return (Y % 4 == 0 && Y % 100 != 0) || (Y % 400 == 0); }

int DaysInMonth(int Y, int M) {
	static const int Dm[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if(M < 1 || M > 12)
		return 0;
	return Dm[M] + (M == 2 && IsLeap(Y) ? 1 : 0);
}

int64_t DaysBeforeYear(int Y) {
	int64_t Days = 0;
	for(int Yr = 1970; Yr < Y; ++Yr)
		Days += IsLeap(Yr) ? 366 : 365;
	return Days;
}

int64_t DaysBeforeMonthInYear(int Y, int M) {
	int64_t Days = 0;
	for(int Mo = 1; Mo < M; ++Mo)
		Days += DaysInMonth(Y, Mo);
	return Days;
}

std::optional<int> ParseYmdFields(int Y, int M, int D) {
	if(M < 1 || M > 12 || D < 1 || D > DaysInMonth(Y, M))
		return std::nullopt;
	return (Y * 10000) + (M * 100) + D;
}

std::optional<int64_t> PackedToEpoch(int Packed) {
	const int Y = Packed / 10000;
	const int M = (Packed / 100) % 100;
	const int D = Packed % 100;
	if(!ParseYmdFields(Y, M, D))
		return std::nullopt;
	return DaysBeforeYear(Y) * 86400LL + DaysBeforeMonthInYear(Y, M) * 86400LL + (D - 1) * 86400LL;
}

std::optional<int> EpochToPacked(int64_t Epoch) {
	const int64_t Days = Epoch / 86400LL;
	int Y = 1970;
	int64_t Rem = Days;
	while(true) {
		const int64_t YearDays = IsLeap(Y) ? 366 : 365;
		if(Rem < YearDays)
			break;
		Rem -= YearDays;
		++Y;
	}
	int M = 1;
	while(M <= 12) {
		const int Dim = DaysInMonth(Y, M);
		if(Rem < Dim)
			break;
		Rem -= Dim;
		++M;
	}
	return ParseYmdFields(Y, M, static_cast<int>(Rem + 1));
}

} // namespace

std::optional<int> ParseIsoYmd(std::string_view S) {
	if(S.size() < 10 || S[4] != '-' || S[7] != '-')
		return std::nullopt;
	for(int I : {0, 1, 2, 3, 5, 6, 8, 9}) {
		if(!IsDigit(S[static_cast<size_t>(I)]))
			return std::nullopt;
	}
	const int Y = (S[0] - '0') * 1000 + (S[1] - '0') * 100 + (S[2] - '0') * 10 + (S[3] - '0');
	const int M = (S[5] - '0') * 10 + (S[6] - '0');
	const int D = (S[8] - '0') * 10 + (S[9] - '0');
	return ParseYmdFields(Y, M, D);
}

std::optional<int64_t> ParseEpochSeconds(std::string_view S) {
	while(!S.empty() && std::isspace(static_cast<unsigned char>(S.front())))
		S.remove_prefix(1);
	while(!S.empty() && std::isspace(static_cast<unsigned char>(S.back())))
		S.remove_suffix(1);
	if(S.empty())
		return std::nullopt;
	const auto Packed = ParseIsoYmd(S);
	if(!Packed)
		return std::nullopt;
	const auto DayEpoch = PackedToEpoch(*Packed);
	if(!DayEpoch)
		return std::nullopt;
	if(S.size() <= 10)
		return *DayEpoch;
	char Sep = S[10];
	if(Sep != 'T' && Sep != ' ' && Sep != 't')
		return *DayEpoch;
	if(S.size() < 19 || S[13] != ':' || S[16] != ':')
		return *DayEpoch;
	for(size_t I = 11; I <= 18; ++I) {
		if(I == 13 || I == 16)
			continue;
		if(!IsDigit(S[I]))
			return std::nullopt;
	}
	const int H = (S[11] - '0') * 10 + (S[12] - '0');
	const int Mi = (S[14] - '0') * 10 + (S[15] - '0');
	const int Se = (S[17] - '0') * 10 + (S[18] - '0');
	if(H > 23 || Mi > 59 || Se > 59)
		return std::nullopt;
	return *DayEpoch + H * 3600LL + Mi * 60LL + Se;
}

std::string FormatIsoYmd(int Packed) {
	const int Y = Packed / 10000;
	const int M = (Packed / 100) % 100;
	const int D = Packed % 100;
	std::ostringstream O;
	O << Y << '-' << (M < 10 ? "0" : "") << M << '-' << (D < 10 ? "0" : "") << D;
	return std::move(O).str();
}

std::string FormatEpochSeconds(int64_t Epoch) {
	const int64_t DayBase = (Epoch >= 0 ? Epoch : Epoch - 86399) / 86400LL * 86400LL;
	const auto Packed = EpochToPacked(DayBase);
	if(!Packed)
		return {};
	const int64_t Rem = Epoch - DayBase;
	const int H = static_cast<int>(Rem / 3600);
	const int Mi = static_cast<int>((Rem % 3600) / 60);
	const int Se = static_cast<int>(Rem % 60);
	std::ostringstream O;
	O << FormatIsoYmd(*Packed) << 'T' << (H < 10 ? "0" : "") << H << ':' << (Mi < 10 ? "0" : "") << Mi << ':'
	  << (Se < 10 ? "0" : "") << Se;
	return std::move(O).str();
}

std::optional<TruncUnit> ParseTruncUnit(std::string_view Unit) {
	while(!Unit.empty() && std::isspace(static_cast<unsigned char>(Unit.front())))
		Unit.remove_prefix(1);
	while(!Unit.empty() && std::isspace(static_cast<unsigned char>(Unit.back())))
		Unit.remove_suffix(1);
	std::string Norm(Unit);
	if(Norm.size() >= 2 && Norm.front() == '\'' && Norm.back() == '\'')
		Norm = Norm.substr(1, Norm.size() - 2);
	for(char &C : Norm) {
		if(C >= 'A' && C <= 'Z')
			C = static_cast<char>(C - 'A' + 'a');
	}
	if(Norm == "year")
		return TruncUnit::Year;
	if(Norm == "month")
		return TruncUnit::Month;
	if(Norm == "day")
		return TruncUnit::Day;
	if(Norm == "hour")
		return TruncUnit::Hour;
	if(Norm == "minute")
		return TruncUnit::Minute;
	if(Norm == "second")
		return TruncUnit::Second;
	return std::nullopt;
}

std::optional<int64_t> TruncateEpoch(int64_t Epoch, TruncUnit Unit) {
	const int64_t DayBase = (Epoch >= 0 ? Epoch : Epoch - 86399) / 86400LL * 86400LL;
	const int64_t Rem = Epoch - DayBase;
	switch(Unit) {
	case TruncUnit::Year: {
		const auto Packed = EpochToPacked(DayBase);
		if(!Packed)
			return std::nullopt;
		const int Y = *Packed / 10000;
		return PackedToEpoch(Y * 10000 + 101);
	}
	case TruncUnit::Month: {
		const auto Packed = EpochToPacked(DayBase);
		if(!Packed)
			return std::nullopt;
		const int Y = *Packed / 10000;
		const int M = (*Packed / 100) % 100;
		return PackedToEpoch(Y * 10000 + M * 100 + 1);
	}
	case TruncUnit::Day:
		return DayBase;
	case TruncUnit::Hour:
		return DayBase + (Rem / 3600) * 3600;
	case TruncUnit::Minute:
		return DayBase + (Rem / 60) * 60;
	case TruncUnit::Second:
		return Epoch;
	}
	return std::nullopt;
}

std::optional<int64_t> TimeBucketEpoch(int64_t Epoch, int64_t BucketSeconds) {
	if(BucketSeconds <= 0)
		return std::nullopt;
	const int64_t Mod = Epoch % BucketSeconds;
	const int64_t Adjust = Mod < 0 ? Mod + BucketSeconds : Mod;
	return Epoch - Adjust;
}

std::optional<int> IsoAddDays(int PackedYmd, int Delta) {
	int Y = PackedYmd / 10000;
	int M = (PackedYmd / 100) % 100;
	int D = PackedYmd % 100;
	int Rem = Delta;
	while(Rem != 0) {
		if(Rem > 0) {
			const int Dim = DaysInMonth(Y, M);
			const int Left = Dim - D;
			if(Rem <= Left) {
				D += Rem;
				Rem = 0;
			} else {
				Rem -= Left + 1;
				D = 1;
				++M;
				if(M > 12) {
					M = 1;
					++Y;
				}
			}
		} else {
			if(D + Rem >= 1) {
				D += Rem;
				Rem = 0;
			} else {
				Rem += D;
				--M;
				if(M < 1) {
					M = 12;
					--Y;
				}
				D = DaysInMonth(Y, M);
			}
		}
	}
	return ParseYmdFields(Y, M, D);
}

std::optional<int64_t> EpochAddDays(int64_t Epoch, int Delta) {
	const int64_t DayBase = (Epoch >= 0 ? Epoch : Epoch - 86399) / 86400LL * 86400LL;
	const auto Packed = EpochToPacked(DayBase);
	if(!Packed)
		return std::nullopt;
	const auto Next = IsoAddDays(*Packed, Delta);
	if(!Next)
		return std::nullopt;
	const auto NextDay = PackedToEpoch(*Next);
	if(!NextDay)
		return std::nullopt;
	return *NextDay + (Epoch - DayBase);
}

std::optional<int64_t> EpochAddSeconds(int64_t Epoch, int64_t Delta) { return Epoch + Delta; }

int EpochDiffDays(int64_t A, int64_t B) {
	const int64_t Da = A / 86400LL;
	const int64_t Db = B / 86400LL;
	return static_cast<int>(Db - Da);
}

int64_t EpochDiffSeconds(int64_t A, int64_t B) { return B - A; }

} // namespace TimeSeries
} // namespace AstralDB
