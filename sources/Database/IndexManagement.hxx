#pragma once

#include <DS/BPlusTree.hxx>
#include <DS/SkipList.hxx>
#include <DS/Tree.hxx>
#include <variant>

namespace AstralDB {
enum class IndexType {
    BPlusTree,
    SkipList,
    Tree
};

template<class Key, class Value> class IndexManager {
    std::variant<BPlusTree<Key, Value>, SkipList<Key, Value>, Tree<Key, Value>> Index_;
    IndexType Type_;
public:
    IndexManager() = default;
};
}