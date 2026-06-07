#include <Database/Storage/ColumnView.hxx>

#include <charconv>
#include <cstring>
#include <sstream>

namespace AstralDB {

ColumnView::ColumnView(const std::byte *Data, std::size_t Size, ColumnEncoding Enc, std::size_t RowCount)
    : Data_(Data), Size_(Size), Encoding_(Enc), RowCount_(RowCount) {
	if(Enc == ColumnEncoding::PlainF64)
		NullBitmapOffset_ = RowCount * sizeof(double);
	else if(Enc == ColumnEncoding::PlainI64)
		NullBitmapOffset_ = RowCount * sizeof(int64_t);
}

bool ColumnView::IsNull(std::size_t Row) const {
	if(Row >= RowCount_ || NullBitmapOffset_ == 0 || NullBitmapOffset_ >= Size_)
		return false;
	const auto *Words = reinterpret_cast<const uint64_t *>(Data_ + NullBitmapOffset_);
	return (Words[Row / 64] & (std::uint64_t{1} << (Row % 64))) != 0;
}

double ColumnView::AsF64(std::size_t Row) const {
	if(Row >= RowCount_ || Encoding_ != ColumnEncoding::PlainF64)
		return 0.0;
	const auto *Vals = reinterpret_cast<const double *>(Data_);
	return Vals[Row];
}

int64_t ColumnView::AsI64(std::size_t Row) const {
	if(Row >= RowCount_ || Encoding_ != ColumnEncoding::PlainI64)
		return 0;
	const auto *Vals = reinterpret_cast<const int64_t *>(Data_);
	return Vals[Row];
}

std::string ColumnView::AsString(std::size_t Row) const {
	if(IsNull(Row))
		return {};
	switch(Encoding_) {
	case ColumnEncoding::PlainF64: {
		std::ostringstream O;
		O << AsF64(Row);
		return O.str();
	}
	case ColumnEncoding::PlainI64:
		return std::to_string(AsI64(Row));
	default:
		return {};
	}
}

void ColumnView::DecodeF64Lane(std::size_t Begin, std::size_t Count, double *Out) const {
	if(Encoding_ != ColumnEncoding::PlainF64 || !Out)
		return;
	const std::size_t N = (std::min)(Count, RowCount_ - Begin);
	const auto *Vals = reinterpret_cast<const double *>(Data_);
	for(std::size_t I = 0; I < N; ++I)
		Out[I] = Vals[Begin + I];
}

void ColumnView::GatherStrings(std::size_t Begin, std::size_t Count, std::vector<std::string> &Out) const {
	const std::size_t N = (std::min)(Count, RowCount_ - Begin);
	Out.resize(N);
	for(std::size_t I = 0; I < N; ++I)
		Out[I] = AsString(Begin + I);
}

} // namespace AstralDB
