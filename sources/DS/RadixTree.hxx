#pragma once

#include <IO/Spinlock.hxx>
#include <IO/Task.hxx>
#include <vector>
#include <memory>
#include <optional>
#include <string>
#include <atomic>
#include <mutex>

namespace AstralDB {
namespace DS {
// Platform-agnostic RCU tracker
class RCUTracker {
	std::atomic<size_t> GlobalEpoch_ {0};
	std::mutex RetireMutex_;
	std::vector<std::pair<size_t, std::function<void()>>> Retired_;
public:
	static thread_local size_t LocalEpoch_;
	void Enter() { LocalEpoch_ = GlobalEpoch_.load(std::memory_order_acquire); }
	void Exit() { LocalEpoch_ = 0; }
	void Synchronize() {
		std::lock_guard<std::mutex> Lock(RetireMutex_);
		// NOTE: This is a minimal, header-only, single-threaded-safe version.
		for(auto It = Retired_.begin(); It != Retired_.end();) {
			if(It->first <= LocalEpoch_) {
				It->second();
				It = Retired_.erase(It);
			} else ++It;
		}
	}
	void Retire(std::function<void()> Deleter) {
		std::lock_guard<std::mutex> Lock(RetireMutex_);
		Retired_.emplace_back(GlobalEpoch_.fetch_add(1, std::memory_order_acq_rel), std::move(Deleter));
	}
};
// Header-only: thread_local variable must be inline
inline thread_local size_t RCUTracker::LocalEpoch_ = 0;

template<class KeyType, class ValueType> class RadixTree {
public:
	using NodePointer = std::shared_ptr<RadixTree<KeyType, ValueType>>;
private:
	std::vector<std::string> Edges_;
	std::vector<NodePointer> Children_;
	std::optional<ValueType> Value_;
	mutable Spinlock Mutex_;
	static inline RCUTracker RCU_{};
public:
	RadixTree() = default;
	virtual ~RadixTree() = default;

	class RCUReadGuard {
	public:
		RCUReadGuard() { RCU_.Enter(); }
		~RCUReadGuard() { RCU_.Exit(); }
	};

	NodePointer InsertRCU(const KeyType& Key, const ValueType& Value, size_t Depth = 0) const {
		NodePointer NewNode = std::make_shared<RadixTree>(*this);
		NewNode->Insert(Key, Value, Depth);
		return NewNode;
	}

	NodePointer RemoveRCU(const KeyType& Key, size_t Depth = 0) const {
		NodePointer NewNode = std::make_shared<RadixTree>(*this);
		NewNode->Remove(Key, Depth);
		return NewNode;
	}

	std::optional<ValueType> FindRCU(const KeyType& Key, size_t Depth = 0) const {
		RCUReadGuard Guard;
		return Find(Key, Depth);
	}

	auto InsertAsync(const KeyType& Key, const ValueType& Value, size_t Depth = 0) {
		return RunAsync([this, Key, Value, Depth]() {
			Insert(Key, Value, Depth);
		});
	}

	void Insert(const KeyType& Key, ValueType&& Value, size_t Depth = 0) {
		AstralDB::SpinlockGuard lock(Mutex_);
		if (Depth == Key.size()) {
			Value_ = std::move(Value);
			return;
		}
		const std::string_view RemainingKey = std::string_view(Key).substr(Depth);
		for (size_t i = 0; i < Edges_.size(); ++i) {
			const std::string& Edge = Edges_[i];
			// Find the longest common prefix between RemainingKey and Edge
			size_t j = 0;
			while (j < Edge.size() && j < RemainingKey.size() && Edge[j] == RemainingKey[j]) ++j;
			if (j == 0) continue; // No match, try next edge

			if (j == Edge.size() && j == RemainingKey.size()) {
				// Exact match, update value
				Children_[i]->Value_ = std::move(Value);
				return;
			} else if (j == Edge.size()) {
				// Edge is a prefix of RemainingKey, go deeper
				Children_[i]->Insert(Key, std::move(Value), Depth + j);
				return;
			} else if (j == RemainingKey.size()) {
				// RemainingKey is a prefix of Edge, need to split edge
				NodePointer NewChild = std::make_shared<RadixTree<KeyType, ValueType>>();
				NewChild->Value_ = std::move(Value);
				// Move the existing child under the new child
				NewChild->Edges_.push_back(Edge.substr(j));
				NewChild->Children_.push_back(Children_[i]);
				// Update current edge and child
				Edges_[i] = std::string(RemainingKey);
				Children_[i] = NewChild;
				return;
			} else {
				// Split edge at position j
				NodePointer SplitNode = std::make_shared<RadixTree<KeyType, ValueType>>();
				// Existing child becomes a child of splitNode
				SplitNode->Edges_.push_back(Edge.substr(j));
				SplitNode->Children_.push_back(Children_[i]);
				// New child for the new key
				NodePointer NewChild = std::make_shared<RadixTree<KeyType, ValueType>>();
				NewChild->Insert(Key, std::move(Value), Depth + j);
				SplitNode->Edges_.push_back(std::string(RemainingKey.substr(j)));
				SplitNode->Children_.push_back(NewChild);
				// Update edge and child
				Edges_[i] = Edge.substr(0, j);
				Children_[i] = SplitNode;
				return;
			}
		}
		// No matching edge, create new
		NodePointer Child = std::make_shared<RadixTree<KeyType, ValueType>>();
		Child->Insert(Key, std::move(Value), Depth + RemainingKey.size());
		Edges_.push_back(std::string(RemainingKey));
		Children_.push_back(Child);
	}

	auto FindAsync(const KeyType& Key, size_t Depth = 0) const {
		return RunAsync([this, Key, Depth]() {
			return Find(Key, Depth);
		});
	}

	// Return the raw pointer type for unique_ptr values
	using RawPtrType = typename std::pointer_traits<decltype(std::declval<ValueType>().get())>::element_type*;
	RawPtrType Find(const KeyType& Key, size_t Depth = 0) const {
		SpinlockGuard lock(Mutex_);
		if (Depth == Key.size()) {
			if (Value_) return Value_->get();
			return nullptr;
		}
		const std::string_view RemainingKey = std::string_view(Key).substr(Depth);
		for (size_t i = 0; i < Edges_.size(); ++i) {
			const std::string& Edge = Edges_[i];
			if (RemainingKey.substr(0, Edge.size()) == Edge) {
				return Children_[i]->Find(Key, Depth + Edge.size());
			}
		}
		return nullptr;
	}

	auto RemoveAsync(const KeyType& Key, size_t Depth = 0) {
		return RunAsync([this, Key, Depth]() {
			return Remove(Key, Depth);
		});
	}

	bool Remove(const KeyType& Key, size_t Depth = 0) {
		SpinlockGuard lock(Mutex_);
		if (Depth == Key.size()) {
			if (Value_) {
				Value_.reset();
				return true;
			}
			return false;
		}
		const std::string_view RemainingKey = std::string_view(Key).substr(Depth);
		for (size_t i = 0; i < Edges_.size(); ++i) {
			const std::string& Edge = Edges_[i];
			if (RemainingKey.substr(0, Edge.size()) == Edge) {
				bool Removed = Children_[i]->Remove(Key, Depth + Edge.size());
				// Clean up empty nodes and merge edges if needed
				if (Children_[i]->IsEmpty()) {
					Children_.erase(Children_.begin() + i);
					Edges_.erase(Edges_.begin() + i);
				} else if (Children_[i]->Children_.size() == 1 && !Children_[i]->Value_.has_value()) {
					// Merge child edge
					Edges_[i] += Children_[i]->Edges_[0];
					NodePointer onlyChild = Children_[i]->Children_[0];
					Children_[i] = onlyChild;
					Children_[i]->Edges_.erase(Children_[i]->Edges_.begin());
				}
				return Removed;
			}
		}
		return false;
	}

	bool Empty() const {
		SpinlockGuard lock(Mutex_);
		return !Value_.has_value() && Children_.empty();
	}

	void Add(const NodePointer & /*Node*/) {
		
	}

	template<typename Visitor>
	void Traverse(Visitor &&V, std::string Prefix) const {
		AstralDB::SpinlockGuard lock(Mutex_);
		if(Value_) V(Prefix, *Value_);
		for(size_t i = 0; i < Edges_.size(); ++i) {
			Children_[i]->Traverse(V, Prefix + Edges_[i]);
		}
	}
};
} // namespace DS
} // namespace AstralDB