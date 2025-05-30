#pragma once

#include <memory>
#include <vector>
#include <algorithm>
#include <functional>
#include <ostream>
#include <string>

namespace AstralDB {

template <class T, class Compare = std::less<T>>
class Tree {
public:
    struct Node {
        T Value;
        std::vector<std::unique_ptr<Node>> Children;

        Node(const T& v)    : Value(v) {}
        Node(T&& v)         : Value(std::move(v)) {}
    };

    std::vector<std::unique_ptr<Node>> Nodes_;
    Compare Compare_;

    // Recursive printer helper
    static void PrintNode(std::ostream& os, const Node* node, int indent) {
        os << std::string(indent * 2, ' ') << node->Value << "\n";
        for (const auto& child : node->Children) {
            PrintNode(os, child.get(), indent + 1);
        }
    }

public:
    Tree() = default;
    Tree(std::vector<std::unique_ptr<Node>>&& nodes)
      : Nodes_(std::move(nodes)) {}

    Tree(Tree&& Other) noexcept {
		Nodes_ = std::move(Other.Nodes_);
		Compare_ = std::move(Other.Compare_);
	}

    // Friend operator<< for pretty-print
    friend std::ostream& operator<<(std::ostream& os, const Tree<T,Compare>& tree) {
        for (const auto& root : tree.Nodes_) {
            PrintNode(os, root.get(), 0);
        }
        return os;
    }

    // Add a new root node
    void Add(const T& Value) {
        Nodes_.push_back(std::make_unique<Node>(Value));
    }
    void Add(T&& Value) {
        Nodes_.push_back(std::make_unique<Node>(std::move(Value)));
    }

    // Add a child under a given parent node
    void AddChild(Node* parent, const T& value) {
        parent->Children.push_back(std::make_unique<Node>(value));
    }
    void AddChild(Node* parent, T&& value) {
        parent->Children.push_back(std::make_unique<Node>(std::move(value)));
    }

    void Remove(const T& value) {
        Nodes_.erase(
            std::remove_if(
                Nodes_.begin(), Nodes_.end(),
                [&](const std::unique_ptr<Node>& n){ return n->Value == value; }
            ),
            Nodes_.end()
        );
    }

    void RemoveAt(size_t index) {
        if (index < Nodes_.size())
            Nodes_.erase(Nodes_.begin() + index);
    }

    void Clear() {
        Nodes_.clear();
    }

    size_t Size() const {
        return Nodes_.size();
    }

    bool Empty() const {
        return Nodes_.empty();
    }

    bool Search(const T& value) const {
        return std::any_of(
            Nodes_.begin(), Nodes_.end(),
            [&](const std::unique_ptr<Node>& n){ return n->Value == value; }
        );
    }

    bool BinarySearch(const T& value) const {
        return std::binary_search(
            Nodes_.begin(), Nodes_.end(), value,
            [&](const std::unique_ptr<Node>& a, const T& v){
                return Compare_(a->Value, v);
            }
        );
    }

    void Sort() {
        std::sort(
            Nodes_.begin(), Nodes_.end(),
            [&](const std::unique_ptr<Node>& a, const std::unique_ptr<Node>& b){
                return Compare_(a->Value, b->Value);
            }
        );
    }

    auto LowerBound(const T& value) {
        return std::lower_bound(
            Nodes_.begin(), Nodes_.end(), value,
            [&](const std::unique_ptr<Node>& a, const T& v){
                return Compare_(a->Value, v);
            }
        );
    }
    auto LowerBound(const T& value) const {
        return std::lower_bound(
            Nodes_.begin(), Nodes_.end(), value,
            [&](const std::unique_ptr<Node>& a, const T& v){
                return Compare_(a->Value, v);
            }
        );
    }

    auto UpperBound(const T& value) {
        return std::upper_bound(
            Nodes_.begin(), Nodes_.end(), value,
            [&](const T& v, const std::unique_ptr<Node>& a){
                return Compare_(v, a->Value);
            }
        );
    }
    auto UpperBound(const T& value) const {
        return std::upper_bound(
            Nodes_.begin(), Nodes_.end(), value,
            [&](const T& v, const std::unique_ptr<Node>& a){
                return Compare_(v, a->Value);
            }
        );
    }

    std::vector<std::unique_ptr<Node>>& GetNodes() {
        return Nodes_;
    }
    const std::vector<std::unique_ptr<Node>>& GetNodes() const {
        return Nodes_;
    }

    T& GetRoot() {
        return Nodes_.front()->Value;
    }
    const T& GetRoot() const {
        return Nodes_.front()->Value;
    }

    Compare GetCompare() const {
        return Compare_;
    }
    void SetCompare(Compare newCompare) {
        Compare_ = newCompare;
    }

    void SetRoot(const T& value) {
        Nodes_.clear();
        Nodes_.push_back(std::make_unique<Node>(value));
    }
    void SetRoot(T&& value) {
        Nodes_.clear();
        Nodes_.push_back(std::make_unique<Node>(std::move(value)));
    }

    auto begin() { return Nodes_.begin(); }
    auto end()   { return Nodes_.end(); }
    auto begin() const { return Nodes_.begin(); }
    auto end()   const { return Nodes_.end(); }

    auto Find(const T& value) {
        return std::find_if(
            Nodes_.begin(), Nodes_.end(),
            [&](const std::unique_ptr<Node>& n){ return n->Value == value; }
        );
    }
    auto Find(const T& value) const {
        return std::find_if(
            Nodes_.begin(), Nodes_.end(),
            [&](const std::unique_ptr<Node>& n){ return n->Value == value; }
        );
    }

    void Insert(const T& value) {
        Nodes_.push_back(std::make_unique<Node>(value));
    }

    Tree<T>& operator=(const Tree<T>& Other) {
        std::swap(*this, Other);
        return *this;
    }

    Tree<T>& operator=(Tree<T>&& Other) noexcept {
        Nodes_ = std::move(Other.Nodes_);
        Compare_ = std::move(Other.Compare_);
        return *this;
    }
};

} // namespace AstralDB
