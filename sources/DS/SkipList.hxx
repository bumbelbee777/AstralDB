#pragma once

#include <vector>
#include <utility>

namespace AstralDB {
template<class Key, class Value> class SkipList {
    std::vector<std::vector<std::pair<Key, Value>>> Nodes_;
public:
    SkipList() {}

    void Insert(const Key &key, const Value &value) {
        std::vector<std::pair<Key, Value>> new_node;
        new_node.push_back(std::make_pair(key, value));
        for(int i = 0; i < 16; i++) {
            if(i >= static_cast<int>(Nodes_.size())) {
                Nodes_.push_back(new_node);
                break;
            }
            if(Nodes_[i].empty()) {
                Nodes_[i] = new_node;
                break;
            }
            if(Nodes_[i][0].first > key)
                std::swap(Nodes_[i], new_node);
        }
    }

    void Erase(const Key &key) {
        for(int i = 0; i < 16; i++) {
            if(i >= static_cast<int>(Nodes_.size()))
                break;
            if(Nodes_[i].empty())
                break;
            if(Nodes_[i][0].first == key) {
                Nodes_[i].clear();
                break;
            }
        }
    }

    Value Find(const Key &key) {
        for(int i = 0; i < 16; i++) {
            if(i >= static_cast<int>(Nodes_.size()))
                break;
            if(Nodes_[i].empty())
                break;
            if(Nodes_[i][0].first == key)
                return Nodes_[i][0].second;
        }
        return Value();
    }

    void Clear() {
        Nodes_.clear();
    }

    size_t Size() {
        size_t size = 0;
        for(int i = 0; i < 16; i++) {
            if(i >= static_cast<int>(Nodes_.size()))
                break;
            if(Nodes_[i].empty())
                break;
            size++;
        }
        return size;
    }

    bool Empty() {
        return Size() == 0;
    }

    std::vector<std::pair<Key, Value>> Nodes() const {
        return Nodes_;
    }

    void SetNodes(const std::vector<std::pair<Key, Value>> &Nodes) {
        Nodes_ = Nodes;
    }

    std::vector<std::pair<Key, Value>> &operator[](size_t Index) {
        return Nodes_[Index];
    }

    const std::vector<std::pair<Key, Value>> &operator[](size_t Index) const {
        return Nodes_[Index];
    }

    std::vector<std::pair<Key, Value>> &At(size_t Index) {
        return Nodes_.at(Index);
    }

    const std::vector<std::pair<Key, Value>> &At(size_t Index) const {
        return Nodes_.at(Index);
    }

    std::vector<std::pair<Key, Value>> &Front() {
        return Nodes_.front();
    }

    const std::vector<std::pair<Key, Value>> &Front() const {
        return Nodes_.front();
    }

    std::vector<std::pair<Key, Value>> &Back() {
        return Nodes_.back();
    }

    const std::vector<std::pair<Key, Value>> &Back() const {
        return Nodes_.back();
    }

    std::vector<std::pair<Key, Value>> &First() {
        return Nodes_.front();
    }

    const std::vector<std::pair<Key, Value>> &First() const {
        return Nodes_.front();
    }

    std::vector<std::pair<Key, Value>> &Last() {
        return Nodes_.back();
    }

    const std::vector<std::pair<Key, Value>> &Last() const {
        return Nodes_.back();
    }

    std::vector<std::pair<Key, Value>> &Top() {
        return Nodes_.front();
    }

    const std::vector<std::pair<Key, Value>> &Top() const {
        return Nodes_.front();
    }

    std::vector<std::pair<Key, Value>> &Bottom() {
        return Nodes_.back();
    }

    const std::vector<std::pair<Key, Value>> &Bottom() const {
        return Nodes_.back();
    }

    std::vector<std::pair<Key, Value>> &begin() {
        return Nodes_.front();
    }

    const std::vector<std::pair<Key, Value>> &eegin() const {
        return Nodes_.front();
    }

    std::vector<std::pair<Key, Value>> &end() {
        return Nodes_.back();
    }

    const std::vector<std::pair<Key, Value>> &end() const {
        return Nodes_.back();
    }

    std::vector<std::pair<Key, Value>> &rbegin() {
        return Nodes_.back();
    }

    const std::vector<std::pair<Key, Value>> &rbegin() const {
        return Nodes_.back();
    }

    std::vector<std::pair<Key, Value>> &rend() {
        return Nodes_.front();
    }

    const std::vector<std::pair<Key, Value>> &rend() const {
        return Nodes_.front();
    }

    std::vector<std::pair<Key, Value>> &cbegin() {
        return Nodes_.front();
    }

    const std::vector<std::pair<Key, Value>> &cbegin() const {
        return Nodes_.front();
    }

    std::vector<std::pair<Key, Value>> &cend() {
        return Nodes_.back();
    }

    const std::vector<std::pair<Key, Value>> &cend() const {
        return Nodes_.back();
    }
};
}