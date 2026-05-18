#include <Database/MathSci.hxx>

#include <Database/AdvancedTypes.hxx>
#include <Database/MathSciAutograd.hxx>
#include <Database/MathSciSignal.hxx>
#include <Database/MathSciSolves.hxx>
#include <Database/GeoSpatial.hxx>
#include <Database/TimeSeriesCompression.hxx>
#include <IO/MathUtil.hxx>
#include <IO/SIMD.hxx>
#include <DS/JSONCodec.hxx>
#include <SQL/JsonSql.hxx>
#include <SQL/XmlSql.hxx>
#include <SQL/SQL.hxx>
#include <SQL/TextSearch.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <unordered_map>

namespace AstralDB {
namespace MathSci {
namespace {

using SQL::ScalarSqlFn;

struct Entry {
	ScalarSqlFn Fn;
	int Min;
	int Max;
};

std::optional<double> ToNum(std::string_view S) {
	if(S.empty())
		return std::nullopt;
	try {
		return std::stod(std::string(S));
	} catch(...) {
		return std::nullopt;
	}
}

static bool SqlCellIsNull(std::string_view S) {
	return S.empty();
}

static bool SqlCellsEqual(std::string_view A, std::string_view B) {
	if(SqlCellIsNull(A) && SqlCellIsNull(B))
		return true;
	if(SqlCellIsNull(A) || SqlCellIsNull(B))
		return false;
	const auto Na = ToNum(A);
	const auto Nb = ToNum(B);
	if(Na && Nb)
		return *Na == *Nb;
	return A == B;
}

std::string FmtNum(double V) {
	std::ostringstream O;
	O.precision(12);
	O << V;
	return std::move(O).str();
}

std::optional<std::string> FormatSeqList(const std::vector<double> &V) {
	std::vector<std::string> Cells;
	Cells.reserve(V.size());
	for(double X : V)
		Cells.push_back(FmtNum(X));
	return AdvancedTypes::FormatListCell(Cells);
}

std::vector<float> SeqToF32(const std::vector<double> &V) {
	std::vector<float> Out(V.size());
	for(size_t I = 0; I < V.size(); ++I)
		Out[I] = static_cast<float>(V[I]);
	return Out;
}

std::vector<double> ToF64Vec(const std::vector<float> &V) {
	std::vector<double> Out(V.size());
	for(size_t I = 0; I < V.size(); ++I)
		Out[I] = static_cast<double>(V[I]);
	return Out;
}

std::optional<std::vector<double>> AsDoubles(const std::vector<std::string> &Cells) {
	std::vector<double> Out;
	Out.reserve(Cells.size());
	for(const auto &C : Cells) {
		const auto V = ToNum(C);
		if(!V)
			return std::nullopt;
		Out.push_back(*V);
	}
	return Out;
}

std::optional<std::vector<double>> FastParseNumericList(std::string_view Cell) {
	if(Cell.size() < 4 || Cell[0] != 'L' || Cell[1] != '[')
		return std::nullopt;
	const size_t Close = Cell.find(']');
	if(Close == std::string::npos || Close + 2 >= Cell.size() || Cell[Close + 1] != ':')
		return std::nullopt;
	std::vector<double> Out;
	Out.reserve(8);
	std::string_view Body = Cell.substr(Close + 2);
	size_t Pos = 0;
	while(Pos < Body.size()) {
		size_t End = Pos;
		while(End < Body.size() && Body[End] != ',')
			++End;
		const auto N = ToNum(Body.substr(Pos, End - Pos));
		if(!N)
			return std::nullopt;
		Out.push_back(*N);
		Pos = End + (End < Body.size() ? 1 : 0);
	}
	return Out.empty() ? std::nullopt : std::optional<std::vector<double>>(std::move(Out));
}

std::optional<std::vector<double>> ParseSeq(std::string_view Cell) {
	if(const auto Fast = FastParseNumericList(Cell))
		return Fast;
	if(const auto L = AdvancedTypes::ParseListCell(Cell))
		return AsDoubles(*L);
	if(const auto V = AdvancedTypes::ParseVectorCell(Cell))
		return *V;
	if(const auto N = ToNum(Cell)) {
		std::vector<double> One{*N};
		return One;
	}
	return std::nullopt;
}

std::optional<double> UnaryNum(const std::vector<std::string> &Cells) {
	if(Cells.size() != 1)
		return std::nullopt;
	return ToNum(Cells[0]);
}

std::optional<std::pair<double, double>> BinaryNum(const std::vector<std::string> &Cells) {
	if(Cells.size() != 2)
		return std::nullopt;
	const auto A = ToNum(Cells[0]);
	const auto B = ToNum(Cells[1]);
	if(!A || !B)
		return std::nullopt;
	return std::make_pair(*A, *B);
}

std::optional<double> MeanOf(const std::vector<double> &V) {
	if(V.empty())
		return std::nullopt;
	return SafeDiv(std::accumulate(V.begin(), V.end(), 0.0), static_cast<double>(V.size()));
}

std::optional<double> VarPopOf(const std::vector<double> &V) {
	const auto M = MeanOf(V);
	if(!M || V.empty())
		return std::nullopt;
	double Acc = 0.0;
	for(double X : V) {
		const double D = X - *M;
		Acc += D * D;
	}
	return SafeDiv(Acc, static_cast<double>(V.size()));
}

std::optional<double> VarSampOf(const std::vector<double> &V) {
	if(V.size() < 2)
		return std::nullopt;
	const auto M = MeanOf(V);
	if(!M)
		return std::nullopt;
	double Acc = 0.0;
	for(double X : V) {
		const double D = X - *M;
		Acc += D * D;
	}
	return SafeDiv(Acc, static_cast<double>(V.size() - 1));
}

std::optional<double> MedianOf(std::vector<double> V) {
	if(V.empty())
		return std::nullopt;
	std::sort(V.begin(), V.end());
	const size_t Mid = V.size() / 2;
	if(V.size() % 2 == 1)
		return V[Mid];
	return (V[Mid - 1] + V[Mid]) * 0.5;
}

std::mt19937_64 &RngEngine() {
	static thread_local std::mt19937_64 Eng{std::random_device{}()};
	return Eng;
}

std::optional<std::pair<std::vector<double>, std::vector<double>>> BinarySeq(const std::vector<std::string> &Cells) {
	if(Cells.size() != 2)
		return std::nullopt;
	const auto A = ParseSeq(Cells[0]);
	const auto B = ParseSeq(Cells[1]);
	if(!A || !B || A->size() != B->size() || A->empty())
		return std::nullopt;
	return std::make_pair(*A, *B);
}

std::optional<std::pair<std::vector<double>, std::vector<double>>> BinarySeqAnyLen(const std::vector<std::string> &Cells) {
	if(Cells.size() != 2)
		return std::nullopt;
	const auto A = ParseSeq(Cells[0]);
	const auto B = ParseSeq(Cells[1]);
	if(!A || !B || A->empty() || B->empty())
		return std::nullopt;
	return std::make_pair(*A, *B);
}

std::optional<std::string> SortListCell(std::string_view Cell, bool Desc) {
	const auto L = AdvancedTypes::ParseListCell(Cell);
	if(!L)
		return std::nullopt;
	std::vector<std::string> Out = *L;
	const auto AllNumeric = [&]() {
		for(const auto &S : Out) {
			if(!ToNum(S))
				return false;
		}
		return true;
	}();
	if(AllNumeric) {
		std::sort(Out.begin(), Out.end(), [&](const std::string &A, const std::string &B) {
			const double X = *ToNum(A);
			const double Y = *ToNum(B);
			return Desc ? X > Y : X < Y;
		});
	} else {
		std::sort(Out.begin(), Out.end(), [Desc](const std::string &A, const std::string &B) {
			return Desc ? A > B : A < B;
		});
	}
	return AdvancedTypes::FormatListCell(Out);
}

const std::unordered_map<std::string, Entry> &BuiltinTable() {
	static const std::unordered_map<std::string, Entry> T = {
	    {"ABS", {ScalarSqlFn::Abs, 1, 1}},
	    {"SQRT", {ScalarSqlFn::Sqrt, 1, 1}},
	    {"CBRT", {ScalarSqlFn::Cbrt, 1, 1}},
	    {"EXP", {ScalarSqlFn::Exp, 1, 1}},
	    {"LN", {ScalarSqlFn::Ln, 1, 1}},
	    {"LOG10", {ScalarSqlFn::Log10, 1, 1}},
	    {"LOG2", {ScalarSqlFn::Log2, 1, 1}},
	    {"SIN", {ScalarSqlFn::Sin, 1, 1}},
	    {"COS", {ScalarSqlFn::Cos, 1, 1}},
	    {"TAN", {ScalarSqlFn::Tan, 1, 1}},
	    {"ASIN", {ScalarSqlFn::Asin, 1, 1}},
	    {"ACOS", {ScalarSqlFn::Acos, 1, 1}},
	    {"ATAN", {ScalarSqlFn::Atan, 1, 1}},
	    {"SINH", {ScalarSqlFn::Sinh, 1, 1}},
	    {"COSH", {ScalarSqlFn::Cosh, 1, 1}},
	    {"TANH", {ScalarSqlFn::Tanh, 1, 1}},
	    {"FLOOR", {ScalarSqlFn::Floor, 1, 1}},
	    {"CEIL", {ScalarSqlFn::Ceil, 1, 1}},
	    {"ROUND", {ScalarSqlFn::Round, 1, 1}},
	    {"TRUNC", {ScalarSqlFn::Trunc, 1, 1}},
	    {"SIGN", {ScalarSqlFn::Sign, 1, 1}},
	    {"DEGREES", {ScalarSqlFn::Degrees, 1, 1}},
	    {"RADIANS", {ScalarSqlFn::Radians, 1, 1}},
	    {"POW", {ScalarSqlFn::Pow, 2, 2}},
	    {"ATAN2", {ScalarSqlFn::Atan2, 2, 2}},
	    {"MOD", {ScalarSqlFn::Mod, 2, 2}},
	    {"HYPOT", {ScalarSqlFn::Hypot, 2, 2}},
	    {"LERP", {ScalarSqlFn::Lerp, 3, 3}},
	    {"CLAMP", {ScalarSqlFn::Clamp, 3, 3}},
	    {"MEAN", {ScalarSqlFn::Mean, 1, 1}},
	    {"VAR_POP", {ScalarSqlFn::VarPop, 1, 1}},
	    {"VAR_SAMP", {ScalarSqlFn::VarSamp, 1, 1}},
	    {"STDDEV_POP", {ScalarSqlFn::StdPop, 1, 1}},
	    {"STDDEV_SAMP", {ScalarSqlFn::StdSamp, 1, 1}},
	    {"MEDIAN", {ScalarSqlFn::Median, 1, 1}},
	    {"ENTROPY", {ScalarSqlFn::Entropy, 1, 1}},
	    {"NORM_L1", {ScalarSqlFn::NormL1, 1, 1}},
	    {"NORM_L2", {ScalarSqlFn::NormL2, 1, 1}},
	    {"LIST_SUM", {ScalarSqlFn::ListSum, 1, 1}},
	    {"CORR", {ScalarSqlFn::Corr, 2, 2}},
	    {"COVAR_POP", {ScalarSqlFn::CovPop, 2, 2}},
	    {"COVAR_SAMP", {ScalarSqlFn::CovSamp, 2, 2}},
	    {"SIGMOID", {ScalarSqlFn::Sigmoid, 1, 1}},
	    {"RELU", {ScalarSqlFn::Relu, 1, 1}},
	    {"SOFTMAX", {ScalarSqlFn::Softmax, 1, 1}},
	    {"MINMAX_SCALE", {ScalarSqlFn::MinMaxScale, 1, 1}},
	    {"ZSCORE", {ScalarSqlFn::ZScore, 1, 1}},
	    {"LIST_LEN", {ScalarSqlFn::ListLen, 1, 1}},
	    {"LIST_GET", {ScalarSqlFn::ListGet, 2, 2}},
	    {"LIST_APPEND", {ScalarSqlFn::ListAppend, 2, 2}},
	    {"LIST_CONCAT", {ScalarSqlFn::ListConcat, 2, 2}},
	    {"LIST_CONTAINS", {ScalarSqlFn::ListContains, 2, 2}},
	    {"LIST_SLICE", {ScalarSqlFn::ListSlice, 3, 3}},
	    {"LOGISTIC", {ScalarSqlFn::Logistic, 1, 1}},
	    {"LOGIT", {ScalarSqlFn::Logit, 1, 1}},
	    {"SOFTPLUS", {ScalarSqlFn::Softplus, 1, 1}},
	    {"LEAKY_RELU", {ScalarSqlFn::LeakyRelu, 1, 2}},
	    {"MSE_LOSS", {ScalarSqlFn::MseLoss, 2, 2}},
	    {"MAE_LOSS", {ScalarSqlFn::MaeLoss, 2, 2}},
	    {"RMSE_LOSS", {ScalarSqlFn::RmseLoss, 2, 2}},
	    {"BCE_LOSS", {ScalarSqlFn::BceLoss, 2, 2}},
	    {"HINGE_LOSS", {ScalarSqlFn::HingeLoss, 2, 2}},
	    {"HUBER_LOSS", {ScalarSqlFn::HuberLoss, 3, 3}},
	    {"CE_LOSS", {ScalarSqlFn::CeLoss, 2, 2}},
	    {"RANDOM", {ScalarSqlFn::Random, 0, 0}},
	    {"RANDOM_NORMAL", {ScalarSqlFn::RandomNormal, 0, 0}},
	    {"RANDOM_INT", {ScalarSqlFn::RandomInt, 2, 2}},
	    {"SETSEED", {ScalarSqlFn::SetSeed, 1, 1}},
	    {"COSINE_SIM", {ScalarSqlFn::CosineSim, 2, 2}},
	    {"EUCLIDEAN_DIST", {ScalarSqlFn::EuclideanDist, 2, 2}},
	    {"MANHATTAN_DIST", {ScalarSqlFn::ManhattanDist, 2, 2}},
	    {"MATVEC", {ScalarSqlFn::MatVec, 2, 2}},
	    {"LIST_SORT", {ScalarSqlFn::ListSort, 1, 1}},
	    {"LIST_SORT_DESC", {ScalarSqlFn::ListSortDesc, 1, 1}},
	    {"LIST_REVERSE", {ScalarSqlFn::ListReverse, 1, 1}},
	    {"JSON_EXTRACT", {ScalarSqlFn::JsonExtract, 2, 2}},
	    {"JSON_CONTAINS", {ScalarSqlFn::JsonContains, 2, 2}},
	    {"JSON_MERGE", {ScalarSqlFn::JsonMerge, 2, 2}},
	    {"JSON_ARRAY_LENGTH", {ScalarSqlFn::JsonArrayLength, 1, 1}},
	    {"JSON_KEYS", {ScalarSqlFn::JsonKeys, 1, 1}},
	    {"NULLIF", {ScalarSqlFn::NullIf, 2, 2}},
	    {"GREATEST", {ScalarSqlFn::Greatest, 2, 2}},
	    {"LEAST", {ScalarSqlFn::Least, 2, 2}},
	    {"TEXT_CONTAINS", {ScalarSqlFn::TextContains, 2, 2}},
	    {"MATCH_AGAINST", {ScalarSqlFn::TextMatch, 2, 2}},
	    {"XML_EXTRACT", {ScalarSqlFn::XmlExtract, 2, 2}},
	    {"XML_SERIALIZE", {ScalarSqlFn::XmlSerialize, 1, 1}},
	    {"XML_VALID", {ScalarSqlFn::XmlValid, 1, 1}},
	    {"TEXT_RANK", {ScalarSqlFn::TextRank, 2, 2}},
	    {"VECTOR_TOPK", {ScalarSqlFn::VectorTopK, 3, 3}},
	    {"FFT", {ScalarSqlFn::Fft, 1, 1}},
	    {"IFFT", {ScalarSqlFn::Ifft, 1, 1}},
	    {"DCT", {ScalarSqlFn::Dct, 1, 1}},
	    {"IDCT", {ScalarSqlFn::Idct, 1, 1}},
	    {"CONV_FULL", {ScalarSqlFn::Conv1d, 2, 2}},
	    {"CONV1D", {ScalarSqlFn::Conv1d, 2, 2}},
	    {"CONV_SAME", {ScalarSqlFn::Conv1dSame, 2, 2}},
	    {"CONV1D_SAME", {ScalarSqlFn::Conv1dSame, 2, 2}},
	    {"LAPLACIAN", {ScalarSqlFn::Laplacian1d, 1, 1}},
	    {"LAPLACIAN1D", {ScalarSqlFn::Laplacian1d, 1, 1}},
	    {"AD_GRAD_ADD", {ScalarSqlFn::AdGradAdd, 1, 1}},
	    {"AD_GRAD_MUL_LHS", {ScalarSqlFn::AdGradMulLhs, 3, 3}},
	    {"AD_GRAD_MUL_RHS", {ScalarSqlFn::AdGradMulRhs, 3, 3}},
	    {"AD_GRAD_RELU", {ScalarSqlFn::AdGradRelu, 2, 2}},
	    {"AD_GRAD_SIGMOID", {ScalarSqlFn::AdGradSigmoid, 2, 2}},
	    {"AD_GRAD_CONV1D_IN", {ScalarSqlFn::AdGradConv1dIn, 3, 3}},
	    {"AD_GRAD_CONV1D_K", {ScalarSqlFn::AdGradConv1dK, 3, 3}},
	    {"AD_CHAIN", {ScalarSqlFn::AdChain, 2, 2}},
	    {"AD_HESSIAN", {ScalarSqlFn::AdHessian, 2, 2}},
	    {"AD_HESSIAN_RELU", {ScalarSqlFn::AdHessianRelu, 2, 2}},
	    {"AD_HESSIAN_SIGMOID", {ScalarSqlFn::AdHessianSigmoid, 2, 2}},
	    {"AD_HESSIAN_SQUARE", {ScalarSqlFn::AdHessianSquare, 2, 2}},
	    {"AD_WIRTINGER_MUL_LHS", {ScalarSqlFn::AdWirtingerMulLhs, 3, 3}},
	    {"AD_WIRTINGER_MUL_RHS", {ScalarSqlFn::AdWirtingerMulRhs, 3, 3}},
	    {"AD_WIRTINGER_ABS2", {ScalarSqlFn::AdWirtingerAbs2, 2, 2}},
	    {"AD_WIRTINGER_CHAIN", {ScalarSqlFn::AdWirtingerChain, 2, 2}},
	    {"AD_WIRTINGER_DZ", {ScalarSqlFn::AdWirtingerDz, 1, 1}},
	    {"AD_WIRTINGER_DZBAR", {ScalarSqlFn::AdWirtingerDzBar, 1, 1}},
	    {"ODE_EULER", {ScalarSqlFn::OdeEuler, 3, 3}},
	    {"ODE_RK4", {ScalarSqlFn::OdeRk4, 6, 6}},
	    {"SDE_EULER", {ScalarSqlFn::SdeEuler, 5, 5}},
	    {"SDE_GBM", {ScalarSqlFn::SdeGbm, 5, 5}},
	    {"SDE_OU", {ScalarSqlFn::SdeOu, 6, 6}},
	    {"PDE_HEAT_STEP", {ScalarSqlFn::PdeHeatStep, 4, 4}},
	    {"PDE_POISSON_STEP", {ScalarSqlFn::PdePoissonStep, 3, 3}},
	    {"ODE_HEUN", {ScalarSqlFn::OdeHeun, 4, 4}},
	    {"ODE_MIDPOINT", {ScalarSqlFn::OdeMidpoint, 3, 3}},
	    {"ODE_IMPLICIT_EULER", {ScalarSqlFn::OdeImplicitEuler, 3, 3}},
	    {"SDE_MILSTEIN", {ScalarSqlFn::SdeMilstein, 5, 5}},
	    {"PDE_ADVECTION_STEP", {ScalarSqlFn::PdeAdvectionStep, 4, 4}},
	    {"PDE_WAVE_STEP", {ScalarSqlFn::PdeWaveStep, 5, 5}},
	    {"SOLVE_ODE", {ScalarSqlFn::SolveOde, 7, 7}},
	    {"ST_POINT", {ScalarSqlFn::StPoint, 2, 2}},
	    {"ST_X", {ScalarSqlFn::StX, 1, 1}},
	    {"ST_Y", {ScalarSqlFn::StY, 1, 1}},
	    {"ST_AS_TEXT", {ScalarSqlFn::StAsText, 1, 1}},
	    {"ST_DISTANCE", {ScalarSqlFn::StDistance, 2, 2}},
	    {"ST_DISTANCE_SPHERICAL", {ScalarSqlFn::StDistanceSpherical, 2, 2}},
	    {"ST_WITHIN_BBOX", {ScalarSqlFn::StWithinBbox, 5, 5}},
	    {"ST_POINTZ", {ScalarSqlFn::StPointZ, 3, 3}},
	    {"ST_ELEVATION", {ScalarSqlFn::StElevation, 1, 1}},
	    {"ST_DEM_SAMPLE", {ScalarSqlFn::StDemSample, 9, 9}},
	    {"ST_TERRAIN_SLOPE", {ScalarSqlFn::StTerrainSlope, 9, 9}},
	    {"TS_COMPRESS", {ScalarSqlFn::TsCompress, 1, 1}},
	    {"TS_DECOMPRESS", {ScalarSqlFn::TsDecompress, 1, 1}},
	    {"TS_COMPRESS_SERIES", {ScalarSqlFn::TsCompressSeries, 2, 2}},
	};
	return T;
}

const std::unordered_map<ScalarSqlFn, BuiltinArity> &ArityTable() {
	static const std::unordered_map<ScalarSqlFn, BuiltinArity> T = [] {
		std::unordered_map<ScalarSqlFn, BuiltinArity> M;
		for(const auto &[Name, E] : BuiltinTable()) {
			(void)Name;
			M[E.Fn] = {E.Min, E.Max};
		}
		return M;
	}();
	return T;
}

} // namespace

bool IsMathSciScalarFn(ScalarSqlFn Fn) {
	return (Fn >= ScalarSqlFn::Abs && Fn <= ScalarSqlFn::TextMatch) ||
	       (Fn >= ScalarSqlFn::XmlExtract && Fn <= ScalarSqlFn::VectorTopK) ||
	       (Fn >= ScalarSqlFn::Fft && Fn <= ScalarSqlFn::StTerrainSlope);
}

BuiltinArity ArityFor(ScalarSqlFn Fn) {
	const auto It = ArityTable().find(Fn);
	if(It == ArityTable().end())
		return {0, 0};
	return It->second;
}

const char *SqlNameFor(ScalarSqlFn Fn) {
	for(const auto &[Name, E] : BuiltinTable()) {
		if(E.Fn == Fn)
			return Name.c_str();
	}
	return "?";
}

std::optional<BuiltinSpec> LookupBuiltin(std::string_view Name) {
	std::string Key(Name);
	for(char &C : Key)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
	const auto It = BuiltinTable().find(Key);
	if(It == BuiltinTable().end())
		return std::nullopt;
	return BuiltinSpec{It->second.Fn, {It->second.Min, It->second.Max}};
}

std::optional<std::string> EvalScalar(ScalarSqlFn Fn, const std::vector<std::string> &Cells) {
	if(!IsMathSciScalarFn(Fn))
		return std::nullopt;

	switch(Fn) {
	case ScalarSqlFn::Abs: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::abs(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Sqrt: {
		const auto X = UnaryNum(Cells);
		return X && *X >= 0 ? std::optional<std::string>(FmtNum(std::sqrt(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Cbrt: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::cbrt(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Exp: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::exp(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Ln: {
		const auto X = UnaryNum(Cells);
		return X && *X > 0 ? std::optional<std::string>(FmtNum(std::log(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Log10: {
		const auto X = UnaryNum(Cells);
		return X && *X > 0 ? std::optional<std::string>(FmtNum(std::log10(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Log2: {
		const auto X = UnaryNum(Cells);
		return X && *X > 0 ? std::optional<std::string>(FmtNum(std::log2(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Sin: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::sin(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Cos: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::cos(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Tan: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::tan(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Asin: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::asin(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Acos: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::acos(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Atan: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::atan(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Sinh: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::sinh(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Cosh: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::cosh(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Tanh: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::tanh(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Floor: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::floor(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Ceil: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::ceil(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Round: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::round(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Trunc: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(std::trunc(*X))) : std::nullopt;
	}
	case ScalarSqlFn::Sign: {
		const auto X = UnaryNum(Cells);
		if(!X)
			return std::nullopt;
		return FmtNum(*X > 0 ? 1.0 : (*X < 0 ? -1.0 : 0.0));
	}
	case ScalarSqlFn::Degrees: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(*X * 180.0 / 3.14159265358979323846)) : std::nullopt;
	}
	case ScalarSqlFn::Radians: {
		const auto X = UnaryNum(Cells);
		return X ? std::optional<std::string>(FmtNum(*X * 3.14159265358979323846 / 180.0)) : std::nullopt;
	}
	case ScalarSqlFn::Pow: {
		const auto P = BinaryNum(Cells);
		return P ? std::optional<std::string>(FmtNum(std::pow(P->first, P->second))) : std::nullopt;
	}
	case ScalarSqlFn::Atan2: {
		const auto P = BinaryNum(Cells);
		return P ? std::optional<std::string>(FmtNum(std::atan2(P->first, P->second))) : std::nullopt;
	}
	case ScalarSqlFn::Mod: {
		const auto P = BinaryNum(Cells);
		return P ? std::optional<std::string>(FmtNum(std::fmod(P->first, P->second))) : std::nullopt;
	}
	case ScalarSqlFn::Hypot: {
		const auto P = BinaryNum(Cells);
		return P ? std::optional<std::string>(FmtNum(std::hypot(P->first, P->second))) : std::nullopt;
	}
	case ScalarSqlFn::Lerp: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto A = ToNum(Cells[0]);
		const auto B = ToNum(Cells[1]);
		const auto T = ToNum(Cells[2]);
		if(!A || !B || !T)
			return std::nullopt;
		return FmtNum(*A + (*B - *A) * *T);
	}
	case ScalarSqlFn::Clamp: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto V = ToNum(Cells[0]);
		const auto Lo = ToNum(Cells[1]);
		const auto Hi = ToNum(Cells[2]);
		if(!V || !Lo || !Hi)
			return std::nullopt;
		return FmtNum(std::max(*Lo, std::min(*Hi, *V)));
	}
	case ScalarSqlFn::Mean: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V)
			return std::nullopt;
		const auto M = MeanOf(*V);
		return M ? std::optional<std::string>(FmtNum(*M)) : std::nullopt;
	}
	case ScalarSqlFn::VarPop: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V)
			return std::nullopt;
		const auto R = VarPopOf(*V);
		return R ? std::optional<std::string>(FmtNum(*R)) : std::nullopt;
	}
	case ScalarSqlFn::VarSamp: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V)
			return std::nullopt;
		const auto R = VarSampOf(*V);
		return R ? std::optional<std::string>(FmtNum(*R)) : std::nullopt;
	}
	case ScalarSqlFn::StdPop: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V)
			return std::nullopt;
		const auto R = VarPopOf(*V);
		return R ? std::optional<std::string>(FmtNum(std::sqrt(*R))) : std::nullopt;
	}
	case ScalarSqlFn::StdSamp: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V)
			return std::nullopt;
		const auto R = VarSampOf(*V);
		return R ? std::optional<std::string>(FmtNum(std::sqrt(*R))) : std::nullopt;
	}
	case ScalarSqlFn::Median: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V)
			return std::nullopt;
		const auto R = MedianOf(*V);
		return R ? std::optional<std::string>(FmtNum(*R)) : std::nullopt;
	}
	case ScalarSqlFn::Entropy: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V || V->empty())
			return std::nullopt;
		double Sum = 0.0;
		for(double X : *V) {
			if(X < 0)
				return std::nullopt;
			Sum += X;
		}
		if(Sum <= 0)
			return std::nullopt;
		double H = 0.0;
		for(double X : *V) {
			const double P = X / Sum;
			if(P > 0)
				H -= P * std::log(P);
		}
		return FmtNum(H);
	}
	case ScalarSqlFn::NormL1: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V)
			return std::nullopt;
		double S = 0.0;
		for(double X : *V)
			S += std::abs(X);
		return FmtNum(S);
	}
	case ScalarSqlFn::NormL2: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V)
			return std::nullopt;
		std::vector<float> F;
		F.reserve(V->size());
		for(double X : *V)
			F.push_back(static_cast<float>(X));
		const float Dot = Simd::DotProductF32(F.data(), F.data(), F.size());
		return FmtNum(std::sqrt(static_cast<double>(Dot)));
	}
	case ScalarSqlFn::ListSum: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V)
			return std::nullopt;
		return FmtNum(std::accumulate(V->begin(), V->end(), 0.0));
	}
	case ScalarSqlFn::Corr:
	case ScalarSqlFn::CovPop:
	case ScalarSqlFn::CovSamp: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto A = ParseSeq(Cells[0]);
		const auto B = ParseSeq(Cells[1]);
		if(!A || !B || A->size() != B->size() || A->empty())
			return std::nullopt;
		const auto Ma = MeanOf(*A);
		const auto Mb = MeanOf(*B);
		if(!Ma || !Mb)
			return std::nullopt;
		double Cov = 0.0;
		for(size_t I = 0; I < A->size(); ++I)
			Cov += ((*A)[I] - *Ma) * ((*B)[I] - *Mb);
		if(Fn == ScalarSqlFn::CovPop)
			return FmtNum(SafeDiv(Cov, static_cast<double>(A->size())));
		if(Fn == ScalarSqlFn::CovSamp) {
			if(A->size() < 2)
				return std::nullopt;
			return FmtNum(SafeDiv(Cov, static_cast<double>(A->size() - 1)));
		}
		const auto Va = VarPopOf(*A);
		const auto Vb = VarPopOf(*B);
		if(!Va || !Vb || *Va == 0 || *Vb == 0)
			return std::nullopt;
		return FmtNum(SafeDiv(SafeDiv(Cov, static_cast<double>(A->size())), std::sqrt(*Va * *Vb)));
	}
	case ScalarSqlFn::Sigmoid: {
		const auto X = UnaryNum(Cells);
		if(!X)
			return std::nullopt;
		return FmtNum(1.0 / (1.0 + std::exp(-*X)));
	}
	case ScalarSqlFn::Relu: {
		const auto X = UnaryNum(Cells);
		if(!X)
			return std::nullopt;
		return FmtNum(*X > 0 ? *X : 0.0);
	}
	case ScalarSqlFn::Softmax: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V || V->empty())
			return std::nullopt;
		double MaxV = *std::max_element(V->begin(), V->end());
		std::vector<double> ExpV;
		ExpV.reserve(V->size());
		double Sum = 0.0;
		for(double X : *V) {
			const double E = std::exp(X - MaxV);
			ExpV.push_back(E);
			Sum += E;
		}
		if(Sum == 0)
			return std::nullopt;
		std::vector<std::string> Out;
		Out.reserve(ExpV.size());
		for(double E : ExpV)
			Out.push_back(FmtNum(E / Sum));
		return AdvancedTypes::FormatListCell(Out);
	}
	case ScalarSqlFn::MinMaxScale: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V || V->empty())
			return std::nullopt;
		const double Lo = *std::min_element(V->begin(), V->end());
		const double Hi = *std::max_element(V->begin(), V->end());
		const double Span = Hi - Lo;
		std::vector<std::string> Out;
		Out.reserve(V->size());
		for(double X : *V)
			Out.push_back(Span == 0 ? FmtNum(0) : FmtNum((X - Lo) / Span));
		return AdvancedTypes::FormatListCell(Out);
	}
	case ScalarSqlFn::ZScore: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V || V->size() < 2)
			return std::nullopt;
		const auto M = MeanOf(*V);
		const auto S = VarSampOf(*V);
		if(!M || !S)
			return std::nullopt;
		const double Sd = std::sqrt(*S);
		if(Sd == 0)
			return std::nullopt;
		std::vector<std::string> Out;
		Out.reserve(V->size());
		for(double X : *V)
			Out.push_back(FmtNum((X - *M) / Sd));
		return AdvancedTypes::FormatListCell(Out);
	}
	case ScalarSqlFn::ListLen: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto L = AdvancedTypes::ParseListCell(Cells[0]);
		if(!L)
			return std::nullopt;
		return std::to_string(L->size());
	}
	case ScalarSqlFn::ListGet: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto L = AdvancedTypes::ParseListCell(Cells[0]);
		const auto Idx = ToNum(Cells[1]);
		if(!L || !Idx || *Idx < 0)
			return std::nullopt;
		const size_t I = static_cast<size_t>(*Idx);
		if(I >= L->size())
			return std::nullopt;
		return (*L)[I];
	}
	case ScalarSqlFn::ListAppend: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto L = AdvancedTypes::ParseListCell(Cells[0]);
		if(!L)
			return std::nullopt;
		std::vector<std::string> Out = *L;
		Out.push_back(Cells[1]);
		return AdvancedTypes::FormatListCell(Out);
	}
	case ScalarSqlFn::ListConcat: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto A = AdvancedTypes::ParseListCell(Cells[0]);
		const auto B = AdvancedTypes::ParseListCell(Cells[1]);
		if(!A || !B)
			return std::nullopt;
		std::vector<std::string> Out = *A;
		Out.insert(Out.end(), B->begin(), B->end());
		return AdvancedTypes::FormatListCell(Out);
	}
	case ScalarSqlFn::ListContains: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto L = AdvancedTypes::ParseListCell(Cells[0]);
		if(!L)
			return std::nullopt;
		for(const auto &E : *L) {
			if(E == Cells[1])
				return std::string("1");
		}
		return std::string("0");
	}
	case ScalarSqlFn::ListSlice: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto L = AdvancedTypes::ParseListCell(Cells[0]);
		const auto S = ToNum(Cells[1]);
		const auto E = ToNum(Cells[2]);
		if(!L || !S || !E || *S < 0 || *E < *S)
			return std::nullopt;
		const size_t Start = static_cast<size_t>(*S);
		const size_t End = static_cast<size_t>(*E);
		if(Start > L->size())
			return AdvancedTypes::FormatListCell({});
		std::vector<std::string> Out;
		for(size_t I = Start; I < End && I < L->size(); ++I)
			Out.push_back((*L)[I]);
		return AdvancedTypes::FormatListCell(Out);
	}
	case ScalarSqlFn::Logistic: {
		const auto X = UnaryNum(Cells);
		if(!X)
			return std::nullopt;
		return FmtNum(1.0 / (1.0 + std::exp(-*X)));
	}
	case ScalarSqlFn::Logit: {
		const auto P = UnaryNum(Cells);
		if(!P || *P <= 0.0 || *P >= 1.0)
			return std::nullopt;
		return FmtNum(std::log(*P / (1.0 - *P)));
	}
	case ScalarSqlFn::Softplus: {
		const auto X = UnaryNum(Cells);
		if(!X)
			return std::nullopt;
		return FmtNum(*X > 20 ? *X : std::log1p(std::exp(*X)));
	}
	case ScalarSqlFn::LeakyRelu: {
		if(Cells.empty())
			return std::nullopt;
		const auto X = ToNum(Cells[0]);
		if(!X)
			return std::nullopt;
		double Alpha = 0.01;
		if(Cells.size() >= 2) {
			const auto A = ToNum(Cells[1]);
			if(!A)
				return std::nullopt;
			Alpha = *A;
		}
		return FmtNum(*X > 0 ? *X : Alpha * *X);
	}
	case ScalarSqlFn::MseLoss: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		double Acc = 0.0;
		for(size_t I = 0; I < P->first.size(); ++I) {
			const double D = P->first[I] - P->second[I];
			Acc += D * D;
		}
		return FmtNum(SafeDiv(Acc, static_cast<double>(P->first.size())));
	}
	case ScalarSqlFn::MaeLoss: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		double Acc = 0.0;
		for(size_t I = 0; I < P->first.size(); ++I)
			Acc += std::abs(P->first[I] - P->second[I]);
		return FmtNum(SafeDiv(Acc, static_cast<double>(P->first.size())));
	}
	case ScalarSqlFn::RmseLoss: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		double Acc = 0.0;
		for(size_t I = 0; I < P->first.size(); ++I) {
			const double D = P->first[I] - P->second[I];
			Acc += D * D;
		}
		return FmtNum(std::sqrt(SafeDiv(Acc, static_cast<double>(P->first.size()))));
	}
	case ScalarSqlFn::BceLoss: {
		const auto P = BinaryNum(Cells);
		if(!P)
			return std::nullopt;
		const double Pred = std::clamp(P->first, 1e-12, 1.0 - 1e-12);
		const double Y = P->second;
		return FmtNum(-(Y * std::log(Pred) + (1.0 - Y) * std::log(1.0 - Pred)));
	}
	case ScalarSqlFn::HingeLoss: {
		const auto P = BinaryNum(Cells);
		if(!P)
			return std::nullopt;
		const double V = 1.0 - P->second * P->first;
		return FmtNum(V > 0 ? V : 0.0);
	}
	case ScalarSqlFn::HuberLoss: {
		if(Cells.size() != 3)
			return std::nullopt;
		const std::vector<std::string> Pair = {Cells[0], Cells[1]};
		const auto P = BinarySeq(Pair);
		const auto Delta = ToNum(Cells[2]);
		if(!P || !Delta || *Delta <= 0)
			return std::nullopt;
		double Acc = 0.0;
		for(size_t I = 0; I < P->first.size(); ++I) {
			const double D = std::abs(P->first[I] - P->second[I]);
			if(D <= *Delta)
				Acc += 0.5 * D * D;
			else
				Acc += *Delta * (D - 0.5 * *Delta);
		}
		return FmtNum(SafeDiv(Acc, static_cast<double>(P->first.size())));
	}
	case ScalarSqlFn::CeLoss: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		double Acc = 0.0;
		for(size_t I = 0; I < P->first.size(); ++I) {
			const double Prob = std::clamp(P->first[I], 1e-12, 1.0);
			Acc -= P->second[I] * std::log(Prob);
		}
		return FmtNum(SafeDiv(Acc, static_cast<double>(P->first.size())));
	}
	case ScalarSqlFn::Random: {
		std::uniform_real_distribution<double> Dist(0.0, 1.0);
		return FmtNum(Dist(RngEngine()));
	}
	case ScalarSqlFn::RandomNormal: {
		std::normal_distribution<double> Dist(0.0, 1.0);
		return FmtNum(Dist(RngEngine()));
	}
	case ScalarSqlFn::RandomInt: {
		const auto Lo = ToNum(Cells[0]);
		const auto Hi = ToNum(Cells[1]);
		if(!Lo || !Hi || *Hi < *Lo)
			return std::nullopt;
		std::uniform_int_distribution<int64_t> Dist(static_cast<int64_t>(*Lo), static_cast<int64_t>(*Hi));
		return std::to_string(Dist(RngEngine()));
	}
	case ScalarSqlFn::SetSeed: {
		const auto S = UnaryNum(Cells);
		if(!S)
			return std::nullopt;
		RngEngine().seed(static_cast<uint64_t>(*S));
		return FmtNum(*S);
	}
	case ScalarSqlFn::CosineSim: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		double Dot = 0.0;
		double Na = 0.0;
		double Nb = 0.0;
		for(size_t I = 0; I < P->first.size(); ++I) {
			Dot += P->first[I] * P->second[I];
			Na += P->first[I] * P->first[I];
			Nb += P->second[I] * P->second[I];
		}
		if(Na == 0 || Nb == 0)
			return std::nullopt;
		return FmtNum(Dot / (std::sqrt(Na) * std::sqrt(Nb)));
	}
	case ScalarSqlFn::EuclideanDist: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		double Acc = 0.0;
		for(size_t I = 0; I < P->first.size(); ++I) {
			const double D = P->first[I] - P->second[I];
			Acc += D * D;
		}
		return FmtNum(std::sqrt(Acc));
	}
	case ScalarSqlFn::ManhattanDist: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		double Acc = 0.0;
		for(size_t I = 0; I < P->first.size(); ++I)
			Acc += std::abs(P->first[I] - P->second[I]);
		return FmtNum(Acc);
	}
	case ScalarSqlFn::MatVec: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto M = AdvancedTypes::DecodeMatrixCell(Cells[0]);
		const auto V = AdvancedTypes::ParseVectorCell(Cells[1]);
		if(!M || !V || V->size() != M->Cols)
			return std::nullopt;
		std::vector<float> Mf;
		Mf.reserve(M->Flat.size());
		for(double X : M->Flat)
			Mf.push_back(static_cast<float>(X));
		std::vector<float> Vf;
		Vf.reserve(V->size());
		for(double X : *V)
			Vf.push_back(static_cast<float>(X));
		std::vector<float> Out(M->Rows);
		Simd::MatrixVectorMulF32(Mf.data(), Vf.data(), Out.data(), M->Rows, M->Cols);
		std::vector<double> Od(Out.begin(), Out.end());
		return AdvancedTypes::FormatVectorCell(Od);
	}
	case ScalarSqlFn::ListSort:
		if(Cells.size() != 1)
			return std::nullopt;
		return SortListCell(Cells[0], false);
	case ScalarSqlFn::ListSortDesc:
		if(Cells.size() != 1)
			return std::nullopt;
		return SortListCell(Cells[0], true);
	case ScalarSqlFn::ListReverse: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto L = AdvancedTypes::ParseListCell(Cells[0]);
		if(!L)
			return std::nullopt;
		std::vector<std::string> Out = *L;
		std::reverse(Out.begin(), Out.end());
		return AdvancedTypes::FormatListCell(Out);
	}
	case ScalarSqlFn::JsonExtract: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto Root = SQL::JsonSql::ParseCellJson(Cells[0]);
		if(!Root)
			return std::string{};
		const auto Got = SQL::JsonSql::ExtractPath(*Root, Cells[1]);
		return Got ? std::optional<std::string>(SQL::JsonSql::JsonCellToText(*Got)) : std::string{};
	}
	case ScalarSqlFn::JsonContains: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto Root = SQL::JsonSql::ParseCellJson(Cells[0]);
		if(!Root)
			return std::string("0");
		return SQL::JsonSql::JsonCellToText(*Root).find(Cells[1]) != std::string::npos ? std::string("1") :
		                                                                                    std::string("0");
	}
	case ScalarSqlFn::JsonMerge: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto A = SQL::JsonSql::ParseCellJson(Cells[0]);
		const auto B = SQL::JsonSql::ParseCellJson(Cells[1]);
		if(!A || !B || !A->IsObject() || !B->IsObject())
			return Cells[0];
		DS::JSONObject M = A->AsObject();
		for(const auto &Kv : B->AsObject())
			M[Kv.first] = Kv.second;
		return DS::SerializeJSON(DS::JSON(std::move(M)));
	}
	case ScalarSqlFn::JsonArrayLength: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto Root = SQL::JsonSql::ParseCellJson(Cells[0]);
		if(!Root || !Root->IsArray())
			return std::string("0");
		return std::to_string(Root->AsArray().size());
	}
	case ScalarSqlFn::JsonKeys: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto Root = SQL::JsonSql::ParseCellJson(Cells[0]);
		if(!Root || !Root->IsObject())
			return AdvancedTypes::FormatListCell({});
		std::vector<std::string> Keys;
		for(const auto &Kv : Root->AsObject())
			Keys.push_back(Kv.first);
		return AdvancedTypes::FormatListCell(Keys);
	}
	case ScalarSqlFn::NullIf: {
		if(Cells.size() != 2)
			return std::nullopt;
		if(SqlCellsEqual(Cells[0], Cells[1]))
			return std::nullopt;
		if(SqlCellIsNull(Cells[0]))
			return std::nullopt;
		return Cells[0];
	}
	case ScalarSqlFn::Greatest: {
		if(Cells.size() != 2)
			return std::nullopt;
		if(SqlCellIsNull(Cells[0]) || SqlCellIsNull(Cells[1]))
			return std::nullopt;
		const auto A = ToNum(Cells[0]);
		const auto B = ToNum(Cells[1]);
		if(!A || !B)
			return std::nullopt;
		return FmtNum(std::max(*A, *B));
	}
	case ScalarSqlFn::Least: {
		if(Cells.size() != 2)
			return std::nullopt;
		if(SqlCellIsNull(Cells[0]) || SqlCellIsNull(Cells[1]))
			return std::nullopt;
		const auto A = ToNum(Cells[0]);
		const auto B = ToNum(Cells[1]);
		if(!A || !B)
			return std::nullopt;
		return FmtNum(std::min(*A, *B));
	}
	case ScalarSqlFn::TextContains:
		if(Cells.size() != 2)
			return std::nullopt;
		return SQL::TextSearch::MatchesQuery(Cells[0], Cells[1]) ? std::string("1") : std::string("0");
	case ScalarSqlFn::TextMatch:
		if(Cells.size() != 2)
			return std::nullopt;
		return SQL::TextSearch::MatchAgainst(Cells[0], Cells[1]) ? std::string("1") : std::string("0");
	case ScalarSqlFn::XmlExtract: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto Root = SQL::XmlSql::ParseCellXml(Cells[0]);
		if(!Root)
			return std::string{};
		const auto Got = SQL::XmlSql::ExtractPath(*Root, Cells[1]);
		return Got ? std::optional<std::string>(*Got) : std::string{};
	}
	case ScalarSqlFn::XmlSerialize: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto Root = SQL::XmlSql::ParseCellXml(Cells[0]);
		if(!Root)
			return Cells[0];
		return SQL::XmlSql::SerializeCell(*Root);
	}
	case ScalarSqlFn::XmlValid:
		if(Cells.size() != 1)
			return std::nullopt;
		return SQL::XmlSql::IsValidXml(Cells[0]) ? std::string("1") : std::string("0");
	case ScalarSqlFn::TextRank:
		if(Cells.size() != 2)
			return std::nullopt;
		return FmtNum(SQL::TextSearch::RankScore(Cells[0], Cells[1]));
	case ScalarSqlFn::Fft: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V || V->empty())
			return std::nullopt;
		const auto Out = MathSciSignal::FftInterleavedReIm(*V);
		if(Out.empty())
			return std::nullopt;
		std::vector<double> Od(Out.begin(), Out.end());
		return FormatSeqList(Od);
	}
	case ScalarSqlFn::Ifft: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V || V->size() < 4 || (V->size() % 2) != 0)
			return std::nullopt;
		const auto Out = MathSciSignal::IfftRealFromInterleaved(*V);
		if(Out.empty())
			return std::nullopt;
		return FormatSeqList(Out);
	}
	case ScalarSqlFn::Dct: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V || V->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSignal::Dct2FromReal(*V));
	}
	case ScalarSqlFn::Idct: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V || V->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSignal::Idct2FromReal(*V));
	}
	case ScalarSqlFn::Conv1d: {
		const auto P = BinarySeqAnyLen(Cells);
		if(!P)
			return std::nullopt;
		return FormatSeqList(MathSciSignal::Conv1dFullFromReal(P->first, P->second));
	}
	case ScalarSqlFn::Conv1dSame: {
		const auto P = BinarySeqAnyLen(Cells);
		if(!P)
			return std::nullopt;
		return FormatSeqList(MathSciSignal::Conv1dSameFromReal(P->first, P->second));
	}
	case ScalarSqlFn::Laplacian1d: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V || V->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSignal::Laplacian1dFromReal(*V));
	}
	case ScalarSqlFn::AdGradAdd: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V || V->empty())
			return std::nullopt;
		const auto F = SeqToF32(*V);
		return FormatSeqList(ToF64Vec(MathSciSignal::AdGradAddF32(F.data(), F.size())));
	}
	case ScalarSqlFn::AdGradMulLhs: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto A = ParseSeq(Cells[0]);
		const auto B = ParseSeq(Cells[1]);
		const auto G = ParseSeq(Cells[2]);
		if(!A || !B || !G || A->size() != B->size() || A->size() != G->size() || A->empty())
			return std::nullopt;
		const auto Af = SeqToF32(*A);
		const auto Bf = SeqToF32(*B);
		const auto Gf = SeqToF32(*G);
		return FormatSeqList(ToF64Vec(MathSciSignal::AdGradMulLhsF32(Af.data(), Bf.data(), Gf.data(), Af.size())));
	}
	case ScalarSqlFn::AdGradMulRhs: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto A = ParseSeq(Cells[0]);
		const auto B = ParseSeq(Cells[1]);
		const auto G = ParseSeq(Cells[2]);
		if(!A || !B || !G || A->size() != B->size() || A->size() != G->size() || A->empty())
			return std::nullopt;
		const auto Af = SeqToF32(*A);
		const auto Bf = SeqToF32(*B);
		const auto Gf = SeqToF32(*G);
		return FormatSeqList(ToF64Vec(MathSciSignal::AdGradMulRhsF32(Af.data(), Bf.data(), Gf.data(), Af.size())));
	}
	case ScalarSqlFn::AdGradRelu: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		const auto Xf = SeqToF32(P->first);
		const auto Gf = SeqToF32(P->second);
		return FormatSeqList(ToF64Vec(MathSciSignal::AdGradReluF32(Xf.data(), Gf.data(), Xf.size())));
	}
	case ScalarSqlFn::AdGradSigmoid: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		const auto Yf = SeqToF32(P->first);
		const auto Gf = SeqToF32(P->second);
		return FormatSeqList(ToF64Vec(MathSciSignal::AdGradSigmoidF32(Yf.data(), Gf.data(), Yf.size())));
	}
	case ScalarSqlFn::AdGradConv1dIn: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto In = ParseSeq(Cells[0]);
		const auto K = ParseSeq(Cells[1]);
		const auto G = ParseSeq(Cells[2]);
		if(!In || !K || !G || In->empty() || K->empty() || G->empty())
			return std::nullopt;
		const auto Inf = SeqToF32(*In);
		const auto Kf = SeqToF32(*K);
		const auto Gf = SeqToF32(*G);
		return FormatSeqList(
		    ToF64Vec(MathSciSignal::AdGradConv1dInputF32(Inf.data(), Inf.size(), Kf.data(), Kf.size(), Gf.data(),
		                                                 Gf.size())));
	}
	case ScalarSqlFn::AdGradConv1dK: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto In = ParseSeq(Cells[0]);
		const auto K = ParseSeq(Cells[1]);
		const auto G = ParseSeq(Cells[2]);
		if(!In || !K || !G || In->empty() || K->empty() || G->empty())
			return std::nullopt;
		const auto Inf = SeqToF32(*In);
		const auto Kf = SeqToF32(*K);
		const auto Gf = SeqToF32(*G);
		return FormatSeqList(
		    ToF64Vec(MathSciSignal::AdGradConv1dKernelF32(Inf.data(), Inf.size(), Kf.data(), Kf.size(), Gf.data(),
		                                                    Gf.size())));
	}
	case ScalarSqlFn::AdChain: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		const auto Lf = SeqToF32(P->first);
		const auto Uf = SeqToF32(P->second);
		return FormatSeqList(ToF64Vec(MathSciSignal::AdChainF32(Lf.data(), Uf.data(), Lf.size())));
	}
	case ScalarSqlFn::AdHessian: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		return FormatSeqList(MathSciAutograd::AdHessianDiagFromReal(P->first, P->second));
	}
	case ScalarSqlFn::AdHessianRelu: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		return FormatSeqList(MathSciAutograd::AdHessianReluFromReal(P->first, P->second));
	}
	case ScalarSqlFn::AdHessianSigmoid: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		return FormatSeqList(MathSciAutograd::AdHessianSigmoidFromReal(P->first, P->second));
	}
	case ScalarSqlFn::AdHessianSquare: {
		const auto P = BinarySeq(Cells);
		if(!P)
			return std::nullopt;
		return FormatSeqList(MathSciAutograd::AdHessianSquareFromReal(P->first, P->second));
	}
	case ScalarSqlFn::AdWirtingerMulLhs: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto R = MathSciAutograd::AdWirtingerMulLhsFromCells(Cells[0], Cells[1], Cells[2]);
		return R ? FormatSeqList(*R) : std::nullopt;
	}
	case ScalarSqlFn::AdWirtingerMulRhs: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto R = MathSciAutograd::AdWirtingerMulRhsFromCells(Cells[0], Cells[1], Cells[2]);
		return R ? FormatSeqList(*R) : std::nullopt;
	}
	case ScalarSqlFn::AdWirtingerAbs2: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto R = MathSciAutograd::AdWirtingerAbs2FromCells(Cells[0], Cells[1]);
		return R ? FormatSeqList(*R) : std::nullopt;
	}
	case ScalarSqlFn::AdWirtingerChain: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto R = MathSciAutograd::AdWirtingerChainFromCells(Cells[0], Cells[1]);
		return R ? FormatSeqList(*R) : std::nullopt;
	}
	case ScalarSqlFn::AdWirtingerDz: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto R = MathSciAutograd::WirtingerDzFromCell(Cells[0]);
		return R ? FormatSeqList(*R) : std::nullopt;
	}
	case ScalarSqlFn::AdWirtingerDzBar: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto R = MathSciAutograd::WirtingerDzBarFromCell(Cells[0]);
		return R ? FormatSeqList(*R) : std::nullopt;
	}
	case ScalarSqlFn::OdeEuler: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto Y = ParseSeq(Cells[0]);
		const auto S = ParseSeq(Cells[1]);
		const auto Dt = ToNum(Cells[2]);
		if(!Y || !S || !Dt || Y->size() != S->size() || Y->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSolves::OdeEulerFromReal(*Y, *Dt, *S));
	}
	case ScalarSqlFn::OdeRk4: {
		if(Cells.size() != 6)
			return std::nullopt;
		const auto Y = ParseSeq(Cells[0]);
		const auto K1 = ParseSeq(Cells[1]);
		const auto K2 = ParseSeq(Cells[2]);
		const auto K3 = ParseSeq(Cells[3]);
		const auto K4 = ParseSeq(Cells[4]);
		const auto Dt = ToNum(Cells[5]);
		if(!Y || !K1 || !K2 || !K3 || !K4 || !Dt || Y->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSolves::OdeRk4FromReal(*Y, *Dt, *K1, *K2, *K3, *K4));
	}
	case ScalarSqlFn::SdeEuler: {
		if(Cells.size() != 5)
			return std::nullopt;
		const auto Y = ParseSeq(Cells[0]);
		const auto Dr = ParseSeq(Cells[1]);
		const auto Di = ParseSeq(Cells[2]);
		const auto Z = ParseSeq(Cells[3]);
		const auto Dt = ToNum(Cells[4]);
		if(!Y || !Dr || !Di || !Z || !Dt || Y->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSolves::SdeEulerFromReal(*Y, *Dt, *Dr, *Di, *Z));
	}
	case ScalarSqlFn::SdeGbm: {
		if(Cells.size() != 5)
			return std::nullopt;
		const auto Y = ToNum(Cells[0]);
		const auto Mu = ToNum(Cells[1]);
		const auto Sigma = ToNum(Cells[2]);
		const auto Dt = ToNum(Cells[3]);
		const auto Z = ToNum(Cells[4]);
		if(!Y || !Mu || !Sigma || !Dt || !Z)
			return std::nullopt;
		return FmtNum(MathSciSolves::SdeGbmScalar(*Y, *Mu, *Sigma, *Dt, *Z));
	}
	case ScalarSqlFn::SdeOu: {
		if(Cells.size() != 6)
			return std::nullopt;
		const auto X = ToNum(Cells[0]);
		const auto Mu = ToNum(Cells[1]);
		const auto Theta = ToNum(Cells[2]);
		const auto Sigma = ToNum(Cells[3]);
		const auto Dt = ToNum(Cells[4]);
		const auto Z = ToNum(Cells[5]);
		if(!X || !Mu || !Theta || !Sigma || !Dt || !Z)
			return std::nullopt;
		return FmtNum(MathSciSolves::SdeOuScalar(*X, *Mu, *Theta, *Sigma, *Dt, *Z));
	}
	case ScalarSqlFn::PdeHeatStep: {
		if(Cells.size() != 4)
			return std::nullopt;
		const auto U = ParseSeq(Cells[0]);
		const auto Alpha = ToNum(Cells[1]);
		const auto Dt = ToNum(Cells[2]);
		const auto Dx = ToNum(Cells[3]);
		if(!U || !Alpha || !Dt || !Dx || U->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSolves::PdeHeat1dFromReal(*U, *Alpha, *Dt, *Dx));
	}
	case ScalarSqlFn::PdePoissonStep: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto U = ParseSeq(Cells[0]);
		const auto F = ParseSeq(Cells[1]);
		const auto Omega = ToNum(Cells[2]);
		if(!U || !F || !Omega || U->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSolves::PdePoisson1dFromReal(*U, *F, *Omega));
	}
	case ScalarSqlFn::OdeHeun: {
		if(Cells.size() != 4)
			return std::nullopt;
		const auto Y = ParseSeq(Cells[0]);
		const auto K1 = ParseSeq(Cells[1]);
		const auto K2 = ParseSeq(Cells[2]);
		const auto Dt = ToNum(Cells[3]);
		if(!Y || !K1 || !K2 || !Dt || Y->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSolves::OdeHeunFromReal(*Y, *Dt, *K1, *K2));
	}
	case ScalarSqlFn::OdeMidpoint: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto Y = ParseSeq(Cells[0]);
		const auto Km = ParseSeq(Cells[1]);
		const auto Dt = ToNum(Cells[2]);
		if(!Y || !Km || !Dt || Y->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSolves::OdeMidpointFromReal(*Y, *Dt, *Km));
	}
	case ScalarSqlFn::OdeImplicitEuler: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto Y = ParseSeq(Cells[0]);
		const auto Lam = ParseSeq(Cells[1]);
		const auto Dt = ToNum(Cells[2]);
		if(!Y || !Lam || !Dt || Y->empty() || Y->size() != Lam->size())
			return std::nullopt;
		return FormatSeqList(MathSciSolves::OdeImplicitEulerFromReal(*Y, *Dt, *Lam));
	}
	case ScalarSqlFn::SdeMilstein: {
		if(Cells.size() != 5)
			return std::nullopt;
		const auto Y = ToNum(Cells[0]);
		const auto Mu = ToNum(Cells[1]);
		const auto Sigma = ToNum(Cells[2]);
		const auto Dt = ToNum(Cells[3]);
		const auto Z = ToNum(Cells[4]);
		if(!Y || !Mu || !Sigma || !Dt || !Z)
			return std::nullopt;
		return FmtNum(MathSciSolves::SdeMilsteinScalar(*Y, *Mu, *Sigma, *Dt, *Z));
	}
	case ScalarSqlFn::PdeAdvectionStep: {
		if(Cells.size() != 4)
			return std::nullopt;
		const auto U = ParseSeq(Cells[0]);
		const auto C = ToNum(Cells[1]);
		const auto Dt = ToNum(Cells[2]);
		const auto Dx = ToNum(Cells[3]);
		if(!U || !C || !Dt || !Dx || U->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSolves::PdeAdvection1dFromReal(*U, *C, *Dt, *Dx));
	}
	case ScalarSqlFn::PdeWaveStep: {
		if(Cells.size() != 5)
			return std::nullopt;
		const auto Up = ParseSeq(Cells[0]);
		const auto Uc = ParseSeq(Cells[1]);
		const auto C = ToNum(Cells[2]);
		const auto Dt = ToNum(Cells[3]);
		const auto Dx = ToNum(Cells[4]);
		if(!Up || !Uc || !C || !Dt || !Dx || Up->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSolves::PdeWave1dFromReal(*Up, *Uc, *C, *Dt, *Dx));
	}
	case ScalarSqlFn::SolveOde: {
		if(Cells.size() != 7)
			return std::nullopt;
		const std::string Method = Cells[0];
		const auto Y = ParseSeq(Cells[1]);
		const auto A = ParseSeq(Cells[2]);
		const auto B = ParseSeq(Cells[3]);
		const auto C = ParseSeq(Cells[4]);
		const auto D = ParseSeq(Cells[5]);
		const auto Dt = ToNum(Cells[6]);
		if(!Y || !A || !B || !C || !D || !Dt || Y->empty())
			return std::nullopt;
		return FormatSeqList(MathSciSolves::OdeSolveFromReal(Method, *Y, *Dt, *A, *B, *C, *D));
	}
	case ScalarSqlFn::StPoint: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto Lon = ToNum(Cells[0]);
		const auto Lat = ToNum(Cells[1]);
		if(!Lon || !Lat)
			return std::nullopt;
		return GeoSpatial::FormatPointCell(*Lon, *Lat);
	}
	case ScalarSqlFn::StX:
	case ScalarSqlFn::StY: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto P = GeoSpatial::ParsePointCell(Cells[0]);
		if(!P) {
			const auto W = GeoSpatial::ParseWktPoint(Cells[0]);
			if(!W)
				return std::nullopt;
			return FmtNum(Fn == ScalarSqlFn::StX ? W->Lon : W->Lat);
		}
		return FmtNum(Fn == ScalarSqlFn::StX ? P->Lon : P->Lat);
	}
	case ScalarSqlFn::StAsText: {
		if(Cells.size() != 1)
			return std::nullopt;
		if(const auto P = GeoSpatial::ParsePointCell(Cells[0]))
			return GeoSpatial::FormatWktPoint(*P);
		if(const auto W = GeoSpatial::ParseWktPoint(Cells[0]))
			return GeoSpatial::FormatWktPoint(*W);
		return std::nullopt;
	}
	case ScalarSqlFn::StDistance:
	case ScalarSqlFn::StDistanceSpherical: {
		if(Cells.size() != 2)
			return std::nullopt;
		auto P1 = GeoSpatial::ParsePointCell(Cells[0]);
		if(!P1)
			P1 = GeoSpatial::ParseWktPoint(Cells[0]);
		auto P2 = GeoSpatial::ParsePointCell(Cells[1]);
		if(!P2)
			P2 = GeoSpatial::ParseWktPoint(Cells[1]);
		if(!P1 || !P2)
			return std::nullopt;
		const double D = Fn == ScalarSqlFn::StDistance ? GeoSpatial::EuclideanDistance(*P1, *P2)
		                                               : GeoSpatial::HaversineMeters(*P1, *P2);
		return FmtNum(D);
	}
	case ScalarSqlFn::StWithinBbox: {
		if(Cells.size() != 5)
			return std::nullopt;
		auto P = GeoSpatial::ParsePointCell(Cells[0]);
		if(!P)
			P = GeoSpatial::ParseWktPoint(Cells[0]);
		const auto MinLon = ToNum(Cells[1]);
		const auto MinLat = ToNum(Cells[2]);
		const auto MaxLon = ToNum(Cells[3]);
		const auto MaxLat = ToNum(Cells[4]);
		if(!P || !MinLon || !MinLat || !MaxLon || !MaxLat)
			return std::nullopt;
		return GeoSpatial::WithinBbox(*P, *MinLon, *MinLat, *MaxLon, *MaxLat) ? "1" : "0";
	}
	case ScalarSqlFn::StPointZ: {
		if(Cells.size() != 3)
			return std::nullopt;
		const auto Lon = ToNum(Cells[0]);
		const auto Lat = ToNum(Cells[1]);
		const auto Elev = ToNum(Cells[2]);
		if(!Lon || !Lat || !Elev)
			return std::nullopt;
		return GeoSpatial::FormatTerrainCell(*Lon, *Lat, *Elev);
	}
	case ScalarSqlFn::StElevation: {
		if(Cells.size() != 1)
			return std::nullopt;
		if(const auto T = GeoSpatial::ParseTerrainCell(Cells[0]))
			return FmtNum(T->ElevM);
		if(const auto W = GeoSpatial::ParseWktPointZ(Cells[0]))
			return FmtNum(W->ElevM);
		return std::nullopt;
	}
	case ScalarSqlFn::StDemSample:
	case ScalarSqlFn::StTerrainSlope: {
		if(Cells.size() != 9)
			return std::nullopt;
		const auto Dem = AdvancedTypes::DecodeMatrixCell(Cells[0]);
		const auto Rows = ToNum(Cells[1]);
		const auto Cols = ToNum(Cells[2]);
		const auto MinLon = ToNum(Cells[3]);
		const auto MinLat = ToNum(Cells[4]);
		const auto MaxLon = ToNum(Cells[5]);
		const auto MaxLat = ToNum(Cells[6]);
		const auto Lon = ToNum(Cells[7]);
		const auto Lat = ToNum(Cells[8]);
		if(!Dem || !Rows || !Cols || !MinLon || !MinLat || !MaxLon || !MaxLat || !Lon || !Lat)
			return std::nullopt;
		const std::size_t R = static_cast<std::size_t>(*Rows);
		const std::size_t C = static_cast<std::size_t>(*Cols);
		if(Dem->Flat.size() != R * C)
			return std::nullopt;
		const auto Val = Fn == ScalarSqlFn::StDemSample
		                     ? GeoSpatial::SampleDemBilinear(Dem->Flat, R, C, *MinLon, *MinLat, *MaxLon, *MaxLat, *Lon,
		                                                     *Lat)
		                     : GeoSpatial::TerrainSlopeDegrees(Dem->Flat, R, C, *MinLon, *MinLat, *MaxLon, *MaxLat, *Lon,
		                                                       *Lat);
		if(!Val)
			return std::nullopt;
		return FmtNum(*Val);
	}
	case ScalarSqlFn::TsCompress: {
		if(Cells.size() != 1)
			return std::nullopt;
		const auto V = ParseSeq(Cells[0]);
		if(!V || V->empty())
			return std::nullopt;
		const std::string Out = TimeSeriesCompression::CompressValues(*V);
		return Out.empty() ? std::nullopt : std::optional<std::string>(Out);
	}
	case ScalarSqlFn::TsDecompress: {
		if(Cells.size() != 1)
			return std::nullopt;
		if(const auto V = TimeSeriesCompression::DecompressValues(Cells[0]))
			return FormatSeqList(*V);
		if(const auto S = TimeSeriesCompression::DecompressSeries(Cells[0])) {
			std::vector<double> Flat;
			Flat.reserve(S->first.size() * 2);
			for(size_t I = 0; I < S->first.size(); ++I) {
				Flat.push_back(S->first[I]);
				Flat.push_back(S->second[I]);
			}
			return FormatSeqList(Flat);
		}
		return std::nullopt;
	}
	case ScalarSqlFn::TsCompressSeries: {
		if(Cells.size() != 2)
			return std::nullopt;
		const auto E = ParseSeq(Cells[0]);
		const auto V = ParseSeq(Cells[1]);
		if(!E || !V || E->empty() || E->size() != V->size())
			return std::nullopt;
		const std::string Out = TimeSeriesCompression::CompressSeries(*E, *V);
		return Out.empty() ? std::nullopt : std::optional<std::string>(Out);
	}
	default:
		return std::nullopt;
	}
}

} // namespace MathSci
} // namespace AstralDB
