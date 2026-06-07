#include <Database/Storage/ColumnarScanFilter.hxx>
#include <Database/Storage/ColumnZoneMap.hxx>
#include <Database/Storage/VectorizedOps.hxx>
#include <Database/Storage/VectorizedScan.hxx>
#include <Database/Execution/BytecodeTypes.hxx>

namespace AstralDB {
namespace {

constexpr const char *kOpIsNull = "__IS_NULL__";
constexpr const char *kOpIsNotNull = "__IS_NOT_NULL__";

static FilterCompareOp OpFromString(const std::string &Op) {
	if(Op == "=" || Op == "==")
		return FilterCompareOp::Eq;
	if(Op == "!=" || Op == "<>")
		return FilterCompareOp::Ne;
	if(Op == ">" || Op == "GT")
		return FilterCompareOp::Gt;
	if(Op == ">=" || Op == "GE")
		return FilterCompareOp::Ge;
	if(Op == "<" || Op == "LT")
		return FilterCompareOp::Lt;
	if(Op == "<=" || Op == "LE")
		return FilterCompareOp::Le;
	return FilterCompareOp::Eq;
}

static std::vector<std::size_t> FilterColumnarRowsByOp(const ColumnarTable &Col, const std::string &Column,
                                                       const std::string &Op, const std::string &Literal) {
	if(Col.RowCount == 0)
		return {};
	const auto It = Col.Columns.find(Column);
	if(It == Col.Columns.end() || It->second.size() != Col.RowCount)
		return {};
	const std::vector<std::string> &Vec = It->second;
	std::vector<std::size_t> Matched;
	Matched.reserve(Col.RowCount);
	if(Op == kOpIsNotNull) {
		for(std::size_t Ri = 0; Ri < Col.RowCount; ++Ri) {
			if(!Vec[Ri].empty())
				Matched.push_back(Ri);
		}
		return Matched;
	}
	if(Op == kOpIsNull) {
		for(std::size_t Ri = 0; Ri < Col.RowCount; ++Ri) {
			if(Vec[Ri].empty())
				Matched.push_back(Ri);
		}
		return Matched;
	}
	return FilterColumnarRows(Col, Column, OpFromString(Op), Literal);
}

static bool ComputeFilterColumnarActiveIndices(const ColumnarTable &Col,
                                             const std::vector<std::vector<FilterPredicateTriple>> &Branches,
                                             std::vector<std::size_t> &Active) {
	if(Col.BulkSyntheticLazy || Col.RowCount == 0 || Col.Columns.empty() || Branches.empty())
		return false;
	Active.clear();
	for(const auto &Branch : Branches) {
		if(Branch.empty())
			continue;
		std::vector<std::size_t> BranchHit;
		for(const auto &[ColName, Op, Val] : Branch) {
			const auto PredHits = FilterColumnarRowsByOp(Col, ColName, Op, Val);
			if(BranchHit.empty()) {
				BranchHit = PredHits;
				continue;
			}
			std::vector<std::size_t> Inter;
			Inter.reserve(std::min(BranchHit.size(), PredHits.size()));
			std::size_t A = 0;
			std::size_t B = 0;
			while(A < BranchHit.size() && B < PredHits.size()) {
				if(BranchHit[A] == PredHits[B]) {
					Inter.push_back(BranchHit[A]);
					++A;
					++B;
				} else if(BranchHit[A] < PredHits[B])
					++A;
				else
					++B;
			}
			BranchHit = std::move(Inter);
			if(BranchHit.empty())
				break;
		}
		if(BranchHit.empty())
			continue;
		if(Active.empty()) {
			Active = std::move(BranchHit);
			continue;
		}
		std::vector<std::size_t> Union;
		Union.reserve(Active.size() + BranchHit.size());
		std::size_t A = 0;
		std::size_t B = 0;
		while(A < Active.size() || B < BranchHit.size()) {
			if(B >= BranchHit.size() || (A < Active.size() && Active[A] < BranchHit[B])) {
				Union.push_back(Active[A++]);
			} else if(A >= Active.size() || BranchHit[B] < Active[A]) {
				Union.push_back(BranchHit[B++]);
			} else {
				Union.push_back(Active[A]);
				++A;
				++B;
			}
		}
		Active = std::move(Union);
	}
	return true;
}

} // namespace

void RefreshColumnarZoneMaps(ColumnarTable &Col) {
	if(Col.BulkSyntheticLazy && Col.Columns.empty())
		return;
	RebuildRowGroupZoneMaps(Col.RowGroups, Col.Columns, Col.RowCount);
}

std::vector<std::size_t> FilterColumnarRows(const ColumnarTable &Col, const std::string &Column, FilterCompareOp Op,
                                            const std::string &Literal) {
	std::vector<std::size_t> Candidates;
	if(Col.RowCount == 0)
		return Candidates;
	const auto It = Col.Columns.find(Column);
	if(It == Col.Columns.end() || It->second.size() != Col.RowCount)
		return Candidates;

	const std::string OpStr = Op == FilterCompareOp::Eq   ? "="
	                          : Op == FilterCompareOp::Ne ? "!="
	                          : Op == FilterCompareOp::Gt ? ">"
	                          : Op == FilterCompareOp::Ge ? ">="
	                          : Op == FilterCompareOp::Lt ? "<"
	                                                      : "<=";
	if(!Col.RowGroups.empty())
		Candidates = RowGroupsToScan(Col.RowGroups, Col.RowCount, Column, OpStr, Literal);
	else {
		Candidates.reserve(Col.RowCount);
		for(std::size_t I = 0; I < Col.RowCount; ++I)
			Candidates.push_back(I);
	}

	int64_t LitI64 = 0;
	const bool NumericLit = [&]() {
		char *End = nullptr;
		const long long V = std::strtoll(Literal.c_str(), &End, 10);
		if(End == Literal.c_str() || *End != '\0')
			return false;
		LitI64 = static_cast<int64_t>(V);
		return true;
	}();

	std::vector<std::size_t> Matched;
	Matched.reserve(Candidates.size());
	const std::vector<std::string> &Vec = It->second;

	if(NumericLit) {
		const std::size_t BatchSz = VectorBatchSizeFromEnv();
		std::size_t Pos = 0;
		while(Pos < Candidates.size()) {
			const std::size_t BatchEnd = std::min(Pos + BatchSz, Candidates.size());
			ColumnVector ColBatch;
			BuildI64ColumnBatch(Vec, Candidates, Pos, BatchEnd, ColBatch);
			if(!ColBatch.I64.empty()) {
				std::vector<std::size_t> Hit;
				VectorizedFilter Filter(ColBatch.I64.data(), ColBatch.I64.size(), Op, LitI64);
				Filter.ExecuteIndices(Hit);
				for(const std::size_t H : Hit)
					Matched.push_back(ColBatch.RowIndices[H]);
			}
			Pos = BatchEnd;
		}
		return Matched;
	}

	for(const std::size_t Ri : Candidates) {
		const std::string &Cell = Vec[Ri];
		bool Ok = false;
		switch(Op) {
		case FilterCompareOp::Eq:
			Ok = Cell == Literal;
			break;
		case FilterCompareOp::Ne:
			Ok = Cell != Literal;
			break;
		case FilterCompareOp::Gt:
			Ok = Cell > Literal;
			break;
		case FilterCompareOp::Ge:
			Ok = Cell >= Literal;
			break;
		case FilterCompareOp::Lt:
			Ok = Cell < Literal;
			break;
		case FilterCompareOp::Le:
			Ok = Cell <= Literal;
			break;
		}
		if(Ok)
			Matched.push_back(Ri);
	}
	return Matched;
}

void CompactColumnarByIndices(ColumnarTable &Col, const std::vector<std::size_t> &Active) {
	if(Active.size() == Col.RowCount)
		return;
	for(auto &[Cn, Vec] : Col.Columns) {
		if(Vec.size() != Col.RowCount)
			continue;
		std::vector<std::string> Next;
		Next.reserve(Active.size());
		for(const std::size_t Ri : Active)
			Next.push_back(Vec[Ri]);
		Vec = std::move(Next);
	}
	for(auto &[Name, Vec] : Col.BulkSyntheticWindowDbl) {
		if(Vec.size() != Col.RowCount)
			continue;
		std::vector<double> Next;
		Next.reserve(Active.size());
		for(const std::size_t Ri : Active)
			Next.push_back(Vec[Ri]);
		Vec = std::move(Next);
	}
	Col.RowCount = Active.size();
	if(Col.RowCount > 0)
		RebuildRowGroupZoneMaps(Col.RowGroups, Col.Columns, Col.RowCount);
	else {
		Col.RowGroups.clear();
	}
}

bool TryFilterColumnarDnfInPlace(ColumnarTable &Col,
                                 const std::vector<std::vector<FilterPredicateTriple>> &Branches) {
	std::vector<std::size_t> Active;
	if(!ComputeFilterColumnarActiveIndices(Col, Branches, Active))
		return false;
	CompactColumnarByIndices(Col, Active);
	return true;
}

bool TryFilterColumnarDnf(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                          const std::vector<std::vector<FilterPredicateTriple>> &Branches, RowTable &OutRows) {
	(void)Schema;
	std::vector<std::size_t> Active;
	if(!ComputeFilterColumnarActiveIndices(Col, Branches, Active))
		return false;

	OutRows.clear();
	OutRows.reserve(Active.size());
	for(const std::size_t Ri : Active) {
		RowItem Row;
		for(const auto &[Cn, Vec] : Col.Columns) {
			if(Vec.size() == Col.RowCount)
				Row[Cn] = Vec[Ri];
		}
		OutRows.push_back(std::move(Row));
	}
	return true;
}

} // namespace AstralDB
