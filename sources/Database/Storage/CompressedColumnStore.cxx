#include <Database/Storage/CompressedColumnStore.hxx>

#include <cstring>

namespace AstralDB {

void CompressedColumnStore::Clear() {
	Columns_.clear();
	RowCount_ = 0;
}

void CompressedColumnStore::AppendChunk(const std::string &Column, ColumnChunk Chunk) {
	auto &Cat = Columns_[Column];
	Cat.TotalRows += Chunk.RowCount;
	Cat.Chunks.push_back(std::move(Chunk));
	if(Chunk.RowCount > RowCount_)
		RowCount_ = Chunk.RowCount;
}

const CompressedColumnStore::ColumnCatalog *CompressedColumnStore::FindColumn(const std::string &Column) const {
	const auto It = Columns_.find(Column);
	return It == Columns_.end() ? nullptr : &It->second;
}

void CompressedColumnStore::AppendPlainF64Chunk(const std::string &Column, const double *Values, std::size_t Count,
                                                const std::vector<bool> &Nulls) {
	ColumnChunk Ck;
	Ck.Encoding = ColumnEncoding::PlainF64;
	Ck.RowCount = Count;
	Ck.Payload.resize(Count * sizeof(double));
	std::memcpy(Ck.Payload.data(), Values, Ck.Payload.size());
	if(!Nulls.empty()) {
		Ck.NullBitmapOffset = Ck.Payload.size();
		const std::size_t Words = (Count + 63) / 64;
		Ck.Payload.resize(Ck.Payload.size() + Words * sizeof(uint64_t), std::byte{0});
		for(std::size_t I = 0; I < Count && I < Nulls.size(); ++I) {
			if(Nulls[I]) {
				auto *Words = reinterpret_cast<uint64_t *>(Ck.Payload.data() + Ck.NullBitmapOffset);
				Words[I / 64] |= (std::uint64_t{1} << (I % 64));
			}
		}
	}
	if(Count > 0) {
		Ck.MinNumeric = Values[0];
		Ck.MaxNumeric = Values[0];
		for(std::size_t I = 1; I < Count; ++I) {
			Ck.MinNumeric = (std::min)(Ck.MinNumeric, Values[I]);
			Ck.MaxNumeric = (std::max)(Ck.MaxNumeric, Values[I]);
		}
	}
	AppendChunk(Column, std::move(Ck));
}

void CompressedColumnStore::AppendPlainI64Chunk(const std::string &Column, const int64_t *Values, std::size_t Count,
                                                const std::vector<bool> &Nulls) {
	ColumnChunk Ck;
	Ck.Encoding = ColumnEncoding::PlainI64;
	Ck.RowCount = Count;
	Ck.Payload.resize(Count * sizeof(int64_t));
	std::memcpy(Ck.Payload.data(), Values, Ck.Payload.size());
	if(!Nulls.empty()) {
		Ck.NullBitmapOffset = Ck.Payload.size();
		const std::size_t Words = (Count + 63) / 64;
		Ck.Payload.resize(Ck.Payload.size() + Words * sizeof(uint64_t), std::byte{0});
		for(std::size_t I = 0; I < Count && I < Nulls.size(); ++I) {
			if(Nulls[I]) {
				auto *Words = reinterpret_cast<uint64_t *>(Ck.Payload.data() + Ck.NullBitmapOffset);
				Words[I / 64] |= (std::uint64_t{1} << (I % 64));
			}
		}
	}
	AppendChunk(Column, std::move(Ck));
}

} // namespace AstralDB
