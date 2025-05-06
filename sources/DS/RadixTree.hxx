#pragma once

#include <IO/Spinlock.hxx>
#include <IO/Task.hxx>
#include <vector>
#include <memory>
#include <optional>
#include <string>

namespace AstralDB {
namespace DS {
template<class KeyType, class ValueType> class RadixTree {
public:
	using NodePointer = std::shared_ptr<RadixTree<KeyType, ValueType>>;

private:
	// Edge compression: store strings instead of single chars for edges
	std::vector<std::string> Edges_;
	std::vector<NodePointer> Children_;
	std::optional<ValueType> Value_;
	mutable Spinlock Mutex_;
	// Placeholder for RCU mechanism (platform-specific, not implemented here)

public:
	RadixTree() = default;
	virtual ~RadixTree() = default;

	// Async Insert
	auto InsertAsync(const KeyType& Key, const ValueType& Value, size_t Depth = 0) {
		return RunAsync([this, Key, Value, Depth]() {
			Insert(Key, Value, Depth);
		});
	}

	void Insert(const KeyType& Key, const ValueType& Value, size_t Depth = 0) {
		AstralDB::SpinlockGuard lock(Mutex_);
		if (Depth == Key.size()) {
			Value_ = Value;
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
				Children_[i]->Value_ = Value;
				return;
			} else if (j == Edge.size()) {
				// Edge is a prefix of RemainingKey, go deeper
				Children_[i]->Insert(Key, Value, Depth + j);
				return;
			} else if (j == RemainingKey.size()) {
				// RemainingKey is a prefix of Edge, need to split edge
				NodePointer newChild = std::make_shared<RadixTree<KeyType, ValueType>>();
				newChild->Value_ = Value;
				// Move the existing child under the new child
				newChild->Edges_.push_back(Edge.substr(j));
				newChild->Children_.push_back(Children_[i]);
				// Update current edge and child
				Edges_[i] = std::string(RemainingKey);
				Children_[i] = newChild;
				return;
			} else {
				// Split edge at position j
				NodePointer splitNode = std::make_shared<RadixTree<KeyType, ValueType>>();
				// Existing child becomes a child of splitNode
				splitNode->Edges_.push_back(Edge.substr(j));
				splitNode->Children_.push_back(Children_[i]);
				// New child for the new key
				NodePointer newChild = std::make_shared<RadixTree<KeyType, ValueType>>();
				newChild->Insert(Key, Value, Depth + j);
				splitNode->Edges_.push_back(std::string(RemainingKey.substr(j)));
				splitNode->Children_.push_back(newChild);
				// Update edge and child
				Edges_[i] = Edge.substr(0, j);
				Children_[i] = splitNode;
				return;
			}
		}
		// No matching edge, create new
		NodePointer Child = std::make_shared<RadixTree<KeyType, ValueType>>();
		Child->Insert(Key, Value, Depth + RemainingKey.size());
		Edges_.push_back(std::string(RemainingKey));
		Children_.push_back(Child);
	}

	// Async Find
	auto FindAsync(const KeyType& Key, size_t Depth = 0) const {
		return RunAsync([this, Key, Depth]() {
			return Find(Key, Depth);
		});
	}

	std::optional<ValueType> Find(const KeyType& Key, size_t Depth = 0) const {
		SpinlockGuard lock(Mutex_);
		if (Depth == Key.size()) return Value_;
		const std::string_view RemainingKey = std::string_view(Key).substr(Depth);
		for (size_t i = 0; i < Edges_.size(); ++i) {
			const std::string& Edge = Edges_[i];
			if (RemainingKey.substr(0, Edge.size()) == Edge) {
				return Children_[i]->Find(Key, Depth + Edge.size());
			}
		}
		return std::nullopt;
	}

	// Async Remove
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

	bool IsEmpty() const {
		SpinlockGuard lock(Mutex_);
		return !Value_.has_value() && Children_.empty();
	}
};
} // namespace DS
} // namespace AstralDB