#include <Database/Storage/VectorizedScan.hxx>

#include <cstdlib>

namespace AstralDB {

void FillScanBatchI64(const std::vector<std::string> &Column, std::size_t Begin, std::size_t End,
                      ScanBatchI64 &Out) {
	Out.BeginRow = Begin;
	Out.Count = End > Begin ? End - Begin : 0;
	Out.Values.resize(Out.Count);
	Out.RowIndices.resize(Out.Count);
	for(std::size_t I = 0; I < Out.Count; ++I) {
		const std::size_t Ri = Begin + I;
		Out.RowIndices[I] = Ri;
		char *EndPtr = nullptr;
		const long long V = std::strtoll(Column[Ri].c_str(), &EndPtr, 10);
		Out.Values[I] = (EndPtr != Column[Ri].c_str() && *EndPtr == '\0') ? static_cast<int64_t>(V) : 0;
	}
}

} // namespace AstralDB
