#include <Database/Storage/TimeSeriesCompression.hxx>

#include <cstdint>
#include <sstream>
#include <string>

namespace AstralDB {
namespace TimeSeriesCompression {
namespace {

std::string PackDeltas(const std::vector<int64_t> &Deltas) {
	std::ostringstream O;
	for(size_t I = 0; I < Deltas.size(); ++I) {
		if(I)
			O << ',';
		O << Deltas[I];
	}
	return O.str();
}

bool ParseInt64(std::string_view S, int64_t &Out) {
	try {
		Out = std::stoll(std::string(S));
		return true;
	} catch(...) {
		return false;
	}
}

std::vector<int64_t> UnpackDeltas(std::string_view Payload) {
	std::vector<int64_t> Out;
	std::string Cur;
	for(char C : Payload) {
		if(C == ',') {
			if(!Cur.empty()) {
				int64_t V = 0;
				if(ParseInt64(Cur, V))
					Out.push_back(V);
				Cur.clear();
			}
		} else
			Cur.push_back(C);
	}
	if(!Cur.empty()) {
		int64_t V = 0;
		if(ParseInt64(Cur, V))
			Out.push_back(V);
	}
	return Out;
}

std::vector<int64_t> ToEpochMicro(const std::vector<double> &Epochs) {
	std::vector<int64_t> Out(Epochs.size());
	for(size_t I = 0; I < Epochs.size(); ++I)
		Out[I] = static_cast<int64_t>(Epochs[I] * 1'000'000.0);
	return Out;
}

std::vector<double> FromEpochMicro(const std::vector<int64_t> &Micro) {
	std::vector<double> Out(Micro.size());
	for(size_t I = 0; I < Micro.size(); ++I)
		Out[I] = static_cast<double>(Micro[I]) / 1'000'000.0;
	return Out;
}

std::vector<int64_t> DeltaEncode(const std::vector<int64_t> &Seq) {
	if(Seq.empty())
		return {};
	std::vector<int64_t> D(Seq.size());
	D[0] = Seq[0];
	for(size_t I = 1; I < Seq.size(); ++I)
		D[I] = Seq[I] - Seq[I - 1];
	return D;
}

std::vector<int64_t> DeltaDecode(const std::vector<int64_t> &Deltas) {
	if(Deltas.empty())
		return {};
	std::vector<int64_t> Seq(Deltas.size());
	Seq[0] = Deltas[0];
	for(size_t I = 1; I < Deltas.size(); ++I)
		Seq[I] = Seq[I - 1] + Deltas[I];
	return Seq;
}

std::vector<int64_t> ValuesToMicro(const std::vector<double> &Values) {
	std::vector<int64_t> Out(Values.size());
	for(size_t I = 0; I < Values.size(); ++I)
		Out[I] = static_cast<int64_t>(Values[I] * 1'000'000.0);
	return Out;
}

std::vector<double> MicroToValues(const std::vector<int64_t> &Micro) {
	std::vector<double> Out(Micro.size());
	for(size_t I = 0; I < Micro.size(); ++I)
		Out[I] = static_cast<double>(Micro[I]) / 1'000'000.0;
	return Out;
}

} // namespace

std::string CompressValues(const std::vector<double> &Values) {
	if(Values.empty() || Values.size() > MaxSeriesLen)
		return {};
	const auto Micro = ValuesToMicro(Values);
	const auto D = DeltaEncode(Micro);
	std::ostringstream O;
	O << "TSC[V," << Values.size() << "|" << PackDeltas(D) << "]";
	return O.str();
}

std::optional<std::vector<double>> DecompressValues(std::string_view Cell) {
	if(Cell.size() < 8 || Cell.substr(0, 3) != "TSC")
		return std::nullopt;
	const size_t Bar = Cell.find('|');
	const size_t Lbr = Cell.find('[');
	const size_t Rbr = Cell.rfind(']');
	if(Bar == std::string::npos || Lbr == std::string::npos || Rbr == std::string::npos)
		return std::nullopt;
	const auto Deltas = UnpackDeltas(Cell.substr(Bar + 1, Rbr - Bar - 1));
	const auto Micro = DeltaDecode(Deltas);
	return MicroToValues(Micro);
}

std::string CompressSeries(const std::vector<double> &Epochs, const std::vector<double> &Values) {
	if(Epochs.size() != Values.size() || Epochs.empty() || Epochs.size() > MaxSeriesLen)
		return {};
	const auto TE = DeltaEncode(ToEpochMicro(Epochs));
	const auto TV = DeltaEncode(ValuesToMicro(Values));
	std::ostringstream O;
	O << "TSC[S," << Epochs.size() << "|E:" << PackDeltas(TE) << ";V:" << PackDeltas(TV) << "]";
	return O.str();
}

std::optional<std::pair<std::vector<double>, std::vector<double>>> DecompressSeries(std::string_view Cell) {
	if(Cell.size() < 10 || Cell.substr(0, 3) != "TSC")
		return std::nullopt;
	const size_t Bar = Cell.find('|');
	const size_t Rbr = Cell.rfind(']');
	if(Bar == std::string::npos || Rbr == std::string::npos)
		return std::nullopt;
	const std::string Payload = std::string(Cell.substr(Bar + 1, Rbr - Bar - 1));
	const size_t VMark = Payload.find(";V:");
	const size_t EMark = Payload.find("E:");
	if(VMark == std::string::npos || EMark == std::string::npos)
		return std::nullopt;
	const auto TE = DeltaDecode(UnpackDeltas(Payload.substr(EMark + 2, VMark - EMark - 2)));
	const auto TV = DeltaDecode(UnpackDeltas(Payload.substr(VMark + 3)));
	if(TE.size() != TV.size() || TE.empty())
		return std::nullopt;
	return std::pair{FromEpochMicro(TE), MicroToValues(TV)};
}

} // namespace TimeSeriesCompression
} // namespace AstralDB
