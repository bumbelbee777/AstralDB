#include <Database/Storage/OlapAggregateMicrokernels.hxx>
#include <Database/Storage/SimdTiling.hxx>
#include <IO/SIMD.hxx>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <vector>

namespace AstralDB {
namespace OlapAggregateMicrokernels {

namespace {

bool ParseInt64View(std::string_view Cell, double &Out) noexcept {
	if(Cell.empty())
		return false;
	bool Neg = false;
	std::size_t I = 0;
	if(Cell[I] == '-') {
		Neg = true;
		++I;
		if(I >= Cell.size())
			return false;
	}
	long long Acc = 0;
	bool Any = false;
	for(; I < Cell.size(); ++I) {
		const char C = Cell[I];
		if(C < '0' || C > '9')
			break;
		Any = true;
		Acc = Acc * 10 + (C - '0');
	}
	if(!Any)
		return false;
	if(I < Cell.size() && Cell[I] == '.') {
		++I;
		double Frac = 0.1;
		bool FracAny = false;
		for(; I < Cell.size(); ++I) {
			const char C = Cell[I];
			if(C < '0' || C > '9')
				break;
			FracAny = true;
			Acc = Acc + (C - '0') * Frac;
			Frac *= 0.1;
		}
		if(I < Cell.size())
			return false;
		(void)FracAny;
	} else if(I < Cell.size())
		return false;
	Out = Neg ? -static_cast<double>(Acc) : static_cast<double>(Acc);
	return true;
}

} // namespace

bool TryParseF64Cell(std::string_view Cell, double &Out) noexcept {
	if(Cell.find('.') != std::string_view::npos) {
		char *End = nullptr;
		const double V = std::strtod(Cell.data(), &End);
		if(End == Cell.data() || End != Cell.data() + Cell.size())
			return false;
		Out = V;
		return true;
	}
	if(ParseInt64View(Cell, Out))
		return true;
	char *End = nullptr;
	const double V = std::strtod(Cell.data(), &End);
	if(End == Cell.data() || (End != Cell.data() + Cell.size()))
		return false;
	Out = V;
	return true;
}

void FusedParseAddF64(std::string_view Cell, double &Acc) noexcept {
	double X = 0.0;
	if(TryParseF64Cell(Cell, X))
		Acc += X;
}

double SumF64(const double *Values, std::size_t Count) noexcept {
	if(!Values || Count == 0)
		return 0.0;
	const TiledCachePlan Plan = SimdTiling::ActivePlan(WorkloadClass::OlapScan, sizeof(double));
	const std::size_t Panel = SimdTiling::L1PanelElements(Plan, sizeof(double));
	SimdTileSession TileSession;
	SimdTiling::BeginTileScan(Count, WorkloadClass::OlapScan, sizeof(double), TileSession);
	double Sum = 0.0;
	for(std::size_t Begin = 0; Begin < Count; Begin += Panel) {
		const std::size_t End = (std::min)(Begin + Panel, Count);
		const std::size_t Slice = End - Begin;
		if(TileSession.Armed)
			SimdTiling::PrefetchStreamAhead(TileSession, Values, End, sizeof(double), Slice);
		Sum += Simd::SumF64(Values + Begin, Slice);
	}
	return Sum;
}

double SumCellsF64(const std::string *Cells, std::size_t Count) noexcept {
	if(!Cells || Count == 0)
		return 0.0;
	std::vector<double> Scratch;
	Scratch.reserve(Count);
	for(std::size_t I = 0; I < Count; ++I) {
		double X = 0.0;
		if(TryParseF64Cell(Cells[I], X))
			Scratch.push_back(X);
	}
	return SumF64(Scratch.data(), Scratch.size());
}

} // namespace OlapAggregateMicrokernels
} // namespace AstralDB
