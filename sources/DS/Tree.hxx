#pragma once

#include <vector>
#include <algorithm>
#include <functional>
#include <iterator>

namespace AstralDB {

template <class T, class Compare = std::less<T>> class Tree {
private:
    std::vector<T> Nodes_;
    Compare Compare_;
public:
    Tree() = default;
    Tree(const std::vector<T> &Nodes) : Nodes_(Nodes) {}


    void Add(const T &Node) {
        Nodes_.push_back(Node);
    }

    void Add(T &&Node) {
        Nodes_.push_back(std::move(Node));
    }

    void Remove(const T &Value) {
        Nodes_.erase(std::remove(Nodes_.begin(), Nodes_.end(), Value), Nodes_.end());
    }

    void RemoveAt(size_t Index) {
        if(Index < Nodes_.size()) Nodes_.erase(Nodes_.begin() + Index);
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

    bool Search(const T &Value) const {
        return std::find(Nodes_.begin(), Nodes_.end(), Value) != Nodes_.end();
    }

    bool BinarySearch(const T &Value) const {
        return std::binary_search(Nodes_.begin(), Nodes_.end(), Value, Compare_);
    }

    void Sort() {
        std::sort(Nodes_.begin(), Nodes_.end(), Compare_);
    }

    typename std::vector<T>::iterator LowerBound(const T& Value) {
        return std::lower_bound(Nodes_.begin(), Nodes_.end(), Value, Compare_);
    }

    typename std::vector<T>::const_iterator LowerBound(const T& Value) const {
        return std::lower_bound(Nodes_.begin(), Nodes_.end(), Value, Compare_);
    }

    typename std::vector<T>::iterator UpperBound(const T& Value) {
        return std::upper_bound(Nodes_.begin(), Nodes_.end(), Value, Compare_);
    }

    typename std::vector<T>::const_iterator UpperBound(const T& Value) const {
        return std::upper_bound(Nodes_.begin(), Nodes_.end(), Value, Compare_);
    }

    std::vector<T>& GetNodes() {
        return Nodes_;
    }

    const std::vector<T>& GetNodes() const {
        return Nodes_;
    }

    Compare GetCompare() const {
        return Compare_;
    }

    void SetCompare(Compare NewCompare) {
        Compare_ = NewCompare;
    }

    void SetRoot(T NewRoot) {
        Nodes_.clear();
        Nodes_.push_back(std::move(NewRoot));
    }

    typename std::vector<T>::iterator begin() {
        return Nodes_.begin();
    }
    typename std::vector<T>::iterator end() {
        return Nodes_.end();
    }
    typename std::vector<T>::const_iterator begin() const {
        return Nodes_.begin();
    }
    typename std::vector<T>::const_iterator end() const {
        return Nodes_.end();
    }

    typename std::vector<T>::iterator Find(const T& Value) {
        return std::find(Nodes_.begin(), Nodes_.end(), Value);
    }
    typename std::vector<T>::const_iterator Find(const T& Value) const {
        return std::find(Nodes_.begin(), Nodes_.end(), Value);
    }
};
}
