#pragma once

#include <vector>
#include <optional>
#include <cstdint>
#include <utility>

namespace AstralDB {
namespace DS {
template<class Key, class Value> class HashTable {
	struct Entry {
		Key KeyValue;
		Value ValueData;
		bool Occupied = false;
		bool Deleted = false;
		Entry() = default;
		Entry(const Key& K, const Value& V) : KeyValue(K), ValueData(V), Occupied(true), Deleted(false) {}
	};

	std::vector<Entry> Data_;
	std::size_t Size_ = 0;
	static constexpr float LoadFactor = 0.7f;

	uint32_t Hash(const Key& K) const {
		// FNV-1a hash for generality
		uint32_t Hash = 2166136261u;
		const uint8_t* Ptr = reinterpret_cast<const uint8_t*>(&K);
		for(size_t i = 0; i < sizeof(Key); ++i)
			Hash = (Hash ^ Ptr[i]) * 16777619u;
		return Hash;
	}

	void Resize(std::size_t NewCap) {
		std::vector<Entry> NewData(NewCap);
		for(const auto& Entry : Data_) {
			if(Entry.Occupied && !Entry.Deleted) {
				std::size_t Index = Hash(Entry.KeyValue) & (NewCap - 1);
				while(NewData[Index].Occupied) Index = (Index + 1) & (NewCap - 1);
				NewData[Index] = Entry;
			}
		}
		Data_ = std::move(NewData);
	}

	void EnsureCapacity() {
		if(Size_ + 1 > Data_.size() * LoadFactor) {
			std::size_t NewCap = Data_.empty() ? 8 : Data_.size() * 2;
			Resize(NewCap);
		}
	}

public:
	HashTable() { Data_.resize(8); }

	void Insert(const Key& K, const Value& V) {
		EnsureCapacity();
		std::size_t Cap = Data_.size();
		std::size_t Index = Hash(K) & (Cap - 1);
		while(Data_[Index].Occupied && !Data_[Index].Deleted) {
			if(Data_[Index].KeyValue == K) {
				Data_[Index].ValueData = V;
				return;
			}
			Index = (Index + 1) & (Cap - 1);
		}
		Data_[Index] = Entry(K, V);
		++Size_;
	}

	std::optional<Value> Get(const Key& K) const {
		std::size_t Cap = Data_.size();
		std::size_t Index = Hash(K) & (Cap - 1);
		std::size_t Start = Index;
		while(Data_[Index].Occupied) {
			if(!Data_[Index].Deleted && Data_[Index].KeyValue == K)
				return Data_[Index].ValueData;
			Index = (Index + 1) & (Cap - 1);
			if(Index == Start) break;
		}
		return std::nullopt;
	}

	void Remove(const Key& K) {
		std::size_t Cap = Data_.size();
		std::size_t Index = Hash(K) & (Cap - 1);
		std::size_t Start = Index;
		while(Data_[Index].Occupied) {
			if(!Data_[Index].Deleted && Data_[Index].KeyValue == K) {
				Data_[Index].Deleted = true;
				--Size_;
				return;
			}
			Index = (Index + 1) & (Cap - 1);
			if(Index == Start) break;
		}
	}

	std::size_t Size() const { return Size_; }
	std::size_t Capacity() const { return Data_.size(); }
};
}
}