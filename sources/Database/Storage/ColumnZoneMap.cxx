#include <Database/Storage/ColumnZoneMap.hxx>

#include <Database/Storage/BulkSynthetic.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace AstralDB {
namespace {

bool TryParseI64(const std::string &S, int64_t &Out) {
	char *End = nullptr;
	const long long V = std::strtoll(S.c_str(), &End, 10);
	if(End == S.c_str() || *End != '\0')
		return false;
	Out = static_cast<int64_t>(V);
	return true;
}

bool TryParseF64(const std::string &S, double &Out) {
	char *End = nullptr;
	const double V = std::strtod(S.c_str(), &End);
	if(End == S.c_str() || *End != '\0')
		return false;
	Out = V;
	return true;
}

int CompareLex(const std::string &A, const std::string &B) {
	if(A < B)
		return -1;
	if(A > B)
		return 1;
	return 0;
}

} // namespace

void UpdateColumnMinMax(ColumnMinMax &Stats, const std::string &Value) {
	if(Stats.Min.empty() && Stats.Max.empty()) {
		Stats.Min = Value;
		Stats.Max = Value;
		int64_t I = 0;
		double D = 0;
		if(TryParseI64(Value, I)) {
			Stats.Numeric = true;
			Stats.MinI64 = Stats.MaxI64 = I;
			Stats.MinF64 = Stats.MaxF64 = static_cast<double>(I);
		} else if(TryParseF64(Value, D)) {
			Stats.Numeric = true;
			Stats.MinF64 = Stats.MaxF64 = D;
			Stats.MinI64 = Stats.MaxI64 = static_cast<int64_t>(D);
		}
		return;
	}
	if(CompareLex(Value, Stats.Min) < 0)
		Stats.Min = Value;
	if(CompareLex(Value, Stats.Max) > 0)
		Stats.Max = Value;
	int64_t I = 0;
	double D = 0;
	if(TryParseI64(Value, I)) {
		if(!Stats.Numeric) {
			Stats.Numeric = true;
			Stats.MinI64 = Stats.MaxI64 = I;
		} else {
			Stats.MinI64 = std::min(Stats.MinI64, I);
			Stats.MaxI64 = std::max(Stats.MaxI64, I);
		}
		Stats.MinF64 = static_cast<double>(Stats.MinI64);
		Stats.MaxF64 = static_cast<double>(Stats.MaxI64);
	} else if(TryParseF64(Value, D)) {
		if(!Stats.Numeric) {
			Stats.Numeric = true;
			Stats.MinF64 = Stats.MaxF64 = D;
		} else {
			Stats.MinF64 = std::min(Stats.MinF64, D);
			Stats.MaxF64 = std::max(Stats.MaxF64, D);
		}
	}
}

void RebuildRowGroupZoneMaps(std::vector<RowGroupStats> &Out,
                             const std::unordered_map<std::string, std::vector<std::string>> &Columns,
                             std::size_t RowCount) {
	Out.clear();
	if(RowCount == 0)
		return;
	const std::size_t GroupCount = (RowCount + kColumnRowGroupSize - 1) / kColumnRowGroupSize;
	Out.resize(GroupCount);
	for(std::size_t G = 0; G < GroupCount; ++G) {
		Out[G].StartRow = G * kColumnRowGroupSize;
		Out[G].EndRow = std::min(RowCount, Out[G].StartRow + kColumnRowGroupSize);
	}
	for(const auto &[ColName, Vec] : Columns) {
		if(Vec.size() != RowCount)
			continue;
		for(std::size_t G = 0; G < GroupCount; ++G) {
			ColumnMinMax &Mm = Out[G].ColumnStats[ColName];
			for(std::size_t R = Out[G].StartRow; R < Out[G].EndRow; ++R)
				UpdateColumnMinMax(Mm, Vec[R]);
		}
	}
}

bool RowGroupMayContain(const RowGroupStats &Group, const std::string &Col, const std::string &Op,
                        const std::string &Val) {
	const auto It = Group.ColumnStats.find(Col);
	if(It == Group.ColumnStats.end())
		return true;
	const ColumnMinMax &Mm = It->second;
	if(Op == "=" || Op == "==") {
		if(Mm.Numeric) {
			int64_t Want = 0;
			if(TryParseI64(Val, Want))
				return Want >= Mm.MinI64 && Want <= Mm.MaxI64;
			double Dw = 0;
			if(TryParseF64(Val, Dw))
				return Dw >= Mm.MinF64 && Dw <= Mm.MaxF64;
		}
		return Val >= Mm.Min && Val <= Mm.Max;
	}
	if(Op == "!=" || Op == "<>") {
		if(Mm.Numeric) {
			int64_t Want = 0;
			if(TryParseI64(Val, Want))
				return Want < Mm.MinI64 || Want > Mm.MaxI64;
		}
		return Val < Mm.Min || Val > Mm.Max;
	}
	if(Op == ">" || Op == "GT") {
		if(Mm.Numeric) {
			int64_t Want = 0;
			if(TryParseI64(Val, Want))
				return Mm.MaxI64 > Want;
			double Dw = 0;
			if(TryParseF64(Val, Dw))
				return Mm.MaxF64 > Dw;
		}
		return Mm.Max > Val;
	}
	if(Op == ">=" || Op == "GE") {
		if(Mm.Numeric) {
			int64_t Want = 0;
			if(TryParseI64(Val, Want))
				return Mm.MaxI64 >= Want;
			double Dw = 0;
			if(TryParseF64(Val, Dw))
				return Mm.MaxF64 >= Dw;
		}
		return Mm.Max >= Val;
	}
	if(Op == "<" || Op == "LT") {
		if(Mm.Numeric) {
			int64_t Want = 0;
			if(TryParseI64(Val, Want))
				return Mm.MinI64 < Want;
			double Dw = 0;
			if(TryParseF64(Val, Dw))
				return Mm.MinF64 < Dw;
		}
		return Mm.Min < Val;
	}
	if(Op == "<=" || Op == "LE") {
		if(Mm.Numeric) {
			int64_t Want = 0;
			if(TryParseI64(Val, Want))
				return Mm.MinI64 <= Want;
			double Dw = 0;
			if(TryParseF64(Val, Dw))
				return Mm.MinF64 <= Dw;
		}
		return Mm.Min <= Val;
	}
	if(Op == "BETWEEN") {
		const size_t Split = Val.find('\x1E');
		if(Split == std::string::npos)
			return true;
		const std::string Lo = Val.substr(0, Split);
		const std::string Hi = Val.substr(Split + 1);
		return RowGroupMayContain(Group, Col, ">=", Lo) && RowGroupMayContain(Group, Col, "<=", Hi);
	}
	return true;
}

std::vector<std::size_t> RowGroupsToScan(const std::vector<RowGroupStats> &Groups, std::size_t RowCount,
                                         const std::string &Col, const std::string &Op, const std::string &Val) {
	std::vector<std::size_t> Out;
	if(Groups.empty()) {
		Out.reserve(RowCount);
		for(std::size_t I = 0; I < RowCount; ++I)
			Out.push_back(I);
		return Out;
	}
	for(const RowGroupStats &G : Groups) {
		if(!RowGroupMayContain(G, Col, Op, Val))
			continue;
		for(std::size_t R = G.StartRow; R < G.EndRow; ++R)
			Out.push_back(R);
	}
	return Out;
}

void RebuildLazyBulkZoneMaps(std::vector<RowGroupStats> &Out, const std::vector<Database::Column> &Schema,
                             std::size_t RowCount, int64_t BulkStartId, int64_t BulkStep) {
	Out.clear();
	if(RowCount == 0)
		return;
	const std::size_t GroupCount = (RowCount + kColumnRowGroupSize - 1) / kColumnRowGroupSize;
	Out.resize(GroupCount);
	for(std::size_t G = 0; G < GroupCount; ++G) {
		Out[G].StartRow = G * kColumnRowGroupSize;
		Out[G].EndRow = std::min(RowCount, Out[G].StartRow + kColumnRowGroupSize);
	}
	for(const Database::Column &Co : Schema) {
		const BulkSyntheticValueKind Kind = ClassifyBulkColumn(Co, 0, Schema.size());
		if(Kind != BulkSyntheticValueKind::PrimaryKey && Kind != BulkSyntheticValueKind::ForeignKey &&
		   Kind != BulkSyntheticValueKind::Integer && Kind != BulkSyntheticValueKind::Decimal &&
		   Kind != BulkSyntheticValueKind::Timestamp)
			continue;
		for(std::size_t G = 0; G < GroupCount; ++G) {
			ColumnMinMax &Mm = Out[G].ColumnStats[Co.Name];
			const int64_t FirstRowId = BulkStartId + static_cast<int64_t>(Out[G].StartRow) * BulkStep;
			const int64_t LastRowId =
			    BulkStartId + static_cast<int64_t>(Out[G].EndRow > 0 ? Out[G].EndRow - 1 : 0) * BulkStep;
			int64_t MinK = 0;
			int64_t MaxK = 0;
			if(BulkSyntheticTryInt64Key(Co, FirstRowId, MinK) && BulkSyntheticTryInt64Key(Co, LastRowId, MaxK)) {
				if(MinK > MaxK)
					std::swap(MinK, MaxK);
				Mm.Numeric = true;
				Mm.MinI64 = MinK;
				Mm.MaxI64 = MaxK;
				Mm.Min = std::to_string(MinK);
				Mm.Max = std::to_string(MaxK);
			} else if(Kind == BulkSyntheticValueKind::Timestamp) {
				Mm.Min = BulkSyntheticIsoTimestamp(1'704'067'200LL + FirstRowId);
				Mm.Max = BulkSyntheticIsoTimestamp(1'704'067'200LL + LastRowId);
			}
		}
	}
}

} // namespace AstralDB
