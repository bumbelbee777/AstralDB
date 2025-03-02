#pragma once

#include <vector>
#include <memory>
#include <optional>
#include <algorithm>
#include <functional>

namespace AstralDB {
template <typename Key, typename Value, size_t Order = 4, typename Compare = std::less<Key>>
class BPlusTree {
    static constexpr size_t MinKeys = (Order + 1) / 2;
    Compare Compare_;

    struct Node {
        bool IsLeaf;
        std::vector<Key> Keys;
        std::vector<Value> Values;
        std::vector<std::shared_ptr<Node>> Children;
        std::shared_ptr<Node> Next;

        explicit Node(bool isLeafFlag)
            : IsLeaf(isLeafFlag) {}
    };

    std::shared_ptr<Node> Root;

    void BorrowFromLeft(const std::shared_ptr<Node>& Parent, int ChildIndex) {
        auto Child = Parent->Children[ChildIndex];
        auto LeftSibling = Parent->Children[ChildIndex - 1];
        if (Child->IsLeaf) {
            Child->Keys.insert(Child->Keys.begin(), LeftSibling->Keys.back());
            Child->Values.insert(Child->Values.begin(), LeftSibling->Values.back());
            LeftSibling->Keys.pop_back();
            LeftSibling->Values.pop_back();
            Parent->Keys[ChildIndex - 1] = Child->Keys.front();
        } else {
            Child->Keys.insert(Child->Keys.begin(), Parent->Keys[ChildIndex - 1]);
            Child->Children.insert(Child->Children.begin(), LeftSibling->Children.back());
            LeftSibling->Children.pop_back();
            Parent->Keys[ChildIndex - 1] = LeftSibling->Keys.back();
            LeftSibling->Keys.pop_back();
        }
    }

    void BorrowFromRight(const std::shared_ptr<Node>& Parent, int ChildIndex) {
        auto Child = Parent->Children[ChildIndex];
        auto RightSibling = Parent->Children[ChildIndex + 1];
        if (Child->IsLeaf) {
            Child->Keys.push_back(RightSibling->Keys.front());
            Child->Values.push_back(RightSibling->Values.front());
            RightSibling->Keys.erase(RightSibling->Keys.begin());
            RightSibling->Values.erase(RightSibling->Values.begin());
            Parent->Keys[ChildIndex] = RightSibling->Keys.front();
        } else {
            Child->Keys.push_back(Parent->Keys[ChildIndex]);
            Child->Children.push_back(RightSibling->Children.front());
            RightSibling->Children.erase(RightSibling->Children.begin());
            Parent->Keys[ChildIndex] = RightSibling->Keys.front();
            RightSibling->Keys.erase(RightSibling->Keys.begin());
        }
    }

    void MergeNodes(const std::shared_ptr<Node>& Parent, int LeftIndex, int RightIndex) {
        auto LeftNode = Parent->Children[LeftIndex];
        auto RightNode = Parent->Children[RightIndex];
        if (LeftNode->IsLeaf) {
            LeftNode->Keys.insert(LeftNode->Keys.end(), RightNode->Keys.begin(), RightNode->Keys.end());
            LeftNode->Values.insert(LeftNode->Values.end(), RightNode->Values.begin(), RightNode->Values.end());
            LeftNode->Next = RightNode->Next;
        } else {
            LeftNode->Keys.push_back(Parent->Keys[LeftIndex]);
            LeftNode->Keys.insert(LeftNode->Keys.end(), RightNode->Keys.begin(), RightNode->Keys.end());
            LeftNode->Children.insert(LeftNode->Children.end(), RightNode->Children.begin(), RightNode->Children.end());
        }
        Parent->Keys.erase(Parent->Keys.begin() + LeftIndex);
        Parent->Children.erase(Parent->Children.begin() + RightIndex);
    }

    bool DeleteHelper(const std::shared_ptr<Node>& CurrentNode, const Key& KeyValue, bool IsRoot, bool& Deleted) {
        if (CurrentNode->IsLeaf) {
            auto It = std::lower_bound(CurrentNode->Keys.begin(), CurrentNode->Keys.end(), KeyValue, Compare_);
            if(It == CurrentNode->Keys.end() || (*It != KeyValue))
            {
                Deleted = false;
                return false;
            }
            size_t Index = It - CurrentNode->Keys.begin();
            CurrentNode->Keys.erase(It);
            CurrentNode->Values.erase(CurrentNode->Values.begin() + Index);
            Deleted = true;
            if(!IsRoot && CurrentNode->Keys.size() < MinKeys)
                return true;
            return false;
        } else {
            int ChildIndex = 0;
            while (ChildIndex < static_cast<int>(CurrentNode->Keys.size()) && KeyValue >= CurrentNode->Keys[ChildIndex])
                ++ChildIndex;
            bool ChildUnderflow = DeleteHelper(CurrentNode->Children[ChildIndex], KeyValue, false, Deleted);
            if(!Deleted)
                return false;
            if(ChildUnderflow) {
                if(ChildIndex > 0 && CurrentNode->Children[ChildIndex - 1]->Keys.size() > MinKeys) {
                    BorrowFromLeft(CurrentNode, ChildIndex);
                    ChildUnderflow = false;
                }
                else if(ChildIndex < static_cast<int>(CurrentNode->Children.size()) - 1 &&
                         CurrentNode->Children[ChildIndex + 1]->Keys.size() > MinKeys) {
                    BorrowFromRight(CurrentNode, ChildIndex);
                    ChildUnderflow = false;
                } else {
                    if(ChildIndex > 0)
                        MergeNodes(CurrentNode, ChildIndex - 1, ChildIndex);
                    else
                        MergeNodes(CurrentNode, ChildIndex, ChildIndex + 1);
                }
            }
            if(!IsRoot && CurrentNode->Keys.size() < MinKeys)
                return true;
            return false;
        }
    }

public:
    BPlusTree() { Root = std::make_shared<Node>(true); }

    void Insert(const Key& KeyValue, const Value& Val) {
        auto InsertResult = InsertInternal(Root, KeyValue, Val);
        if(InsertResult.has_value()) {
            auto NewRoot = std::make_shared<Node>(false);
            NewRoot->Keys.push_back(InsertResult->first);
            NewRoot->Children.push_back(Root);
            NewRoot->Children.push_back(InsertResult->second);
            Root = NewRoot;
        }
    }

    std::optional<std::pair<Key, std::shared_ptr<Node>>>
    InsertInternal(const std::shared_ptr<Node>& CurrentNode, const Key& KeyValue, const Value& Val) {
        if (CurrentNode->IsLeaf) {
            auto It = std::lower_bound(CurrentNode->Keys.begin(), CurrentNode->Keys.end(), KeyValue, Compare_);
            size_t Index = It - CurrentNode->Keys.begin();
            CurrentNode->Keys.insert(It, KeyValue);
            CurrentNode->Values.insert(CurrentNode->Values.begin() + Index, Val);
            if (CurrentNode->Keys.size() > Order) {
                size_t Mid = CurrentNode->Keys.size() / 2;
                auto NewLeaf = std::make_shared<Node>(true);
                NewLeaf->Keys.assign(CurrentNode->Keys.begin() + Mid, CurrentNode->Keys.end());
                NewLeaf->Values.assign(CurrentNode->Values.begin() + Mid, CurrentNode->Values.end());
                CurrentNode->Keys.erase(CurrentNode->Keys.begin() + Mid, CurrentNode->Keys.end());
                CurrentNode->Values.erase(CurrentNode->Values.begin() + Mid, CurrentNode->Values.end());
                NewLeaf->Next = CurrentNode->Next;
                CurrentNode->Next = NewLeaf;
                return std::make_optional(std::make_pair(NewLeaf->Keys.front(), NewLeaf));
            }
            return std::nullopt;
        } else {
            size_t Index = std::upper_bound(CurrentNode->Keys.begin(), CurrentNode->Keys.end(), KeyValue, Compare_) - CurrentNode->Keys.begin();
            auto Child = CurrentNode->Children[Index];
            auto Result = InsertInternal(Child, KeyValue, Val);
            if (Result.has_value()) {
                Key PromoteKey = Result->first;
                auto NewChild = Result->second;
                auto It = std::upper_bound(CurrentNode->Keys.begin(), CurrentNode->Keys.end(), PromoteKey, Compare_);
                size_t Pos = It - CurrentNode->Keys.begin();
                CurrentNode->Keys.insert(It, PromoteKey);
                CurrentNode->Children.insert(CurrentNode->Children.begin() + Pos + 1, NewChild);
                if(CurrentNode->Keys.size() > Order) {
                    size_t Mid = CurrentNode->Keys.size() / 2;
                    Key NewPromoteKey = CurrentNode->Keys[Mid];
                    auto NewInternal = std::make_shared<Node>(false);
                    NewInternal->Keys.assign(CurrentNode->Keys.begin() + Mid + 1, CurrentNode->Keys.end());
                    NewInternal->Children.assign(CurrentNode->Children.begin() + Mid + 1, CurrentNode->Children.end());
                    CurrentNode->Keys.erase(CurrentNode->Keys.begin() + Mid, CurrentNode->Keys.end());
                    CurrentNode->Children.erase(CurrentNode->Children.begin() + Mid + 1, CurrentNode->Children.end());
                    return std::make_optional(std::make_pair(NewPromoteKey, NewInternal));
                }
            }
            return std::nullopt;
        }
    }

    bool Search(const Key& KeyValue, Value& OutValue) const {
        auto CurrentNode = Root;
        while(!CurrentNode->IsLeaf) {
            size_t Index = std::upper_bound(CurrentNode->Keys.begin(), CurrentNode->Keys.end(), KeyValue, Compare_) - CurrentNode->Keys.begin();
            CurrentNode = CurrentNode->Children[Index];
        }
        auto It = std::lower_bound(CurrentNode->Keys.begin(), CurrentNode->Keys.end(), KeyValue, Compare_);
        if(It != CurrentNode->Keys.end() && *It == KeyValue) {
            size_t Index = It - CurrentNode->Keys.begin();
            OutValue = CurrentNode->Values[Index];
            return true;
        }
        return false;
    }

    bool Update(const Key& KeyValue, const Value& NewValue) {
        auto CurrentNode = Root;
        while(!CurrentNode->IsLeaf) {
            size_t Index = std::upper_bound(CurrentNode->Keys.begin(), CurrentNode->Keys.end(), KeyValue, Compare_) - CurrentNode->Keys.begin();
            CurrentNode = CurrentNode->Children[Index];
        }
        auto It = std::lower_bound(CurrentNode->Keys.begin(), CurrentNode->Keys.end(), KeyValue, Compare_);
        if(It != CurrentNode->Keys.end() && *It == KeyValue) {
            size_t Index = It - CurrentNode->Keys.begin();
            CurrentNode->Values[Index] = NewValue;
            return true;
        }
        return false;
    }

    bool Delete(const Key& KeyValue) {
        bool Deleted = false;
        bool Underflow = DeleteHelper(Root, KeyValue, true, Deleted);
        if(!Root->IsLeaf && Root->Children.size() == 1)
            Root = Root->Children.front();
        return Deleted;
    }

    std::vector<Value> RangeSearch(const Key& LowerBound, const Key& UpperBound) const {
        std::vector<Value> Results;
        auto CurrentNode = Root;
        while(!CurrentNode->IsLeaf) {
            size_t Index = std::upper_bound(CurrentNode->Keys.begin(), CurrentNode->Keys.end(), LowerBound, Compare_) - CurrentNode->Keys.begin();
            CurrentNode = CurrentNode->Children[Index];
        }
        while(CurrentNode) {
            for (size_t i = 0; i < CurrentNode->Keys.size(); ++i) {
                if (!Compare_(CurrentNode->Keys[i], LowerBound) && !Compare_(UpperBound, CurrentNode->Keys[i]))
                    Results.push_back(CurrentNode->Values[i]);
                else if (Compare_(UpperBound, CurrentNode->Keys[i]))
                    return Results;
            }
            CurrentNode = CurrentNode->Next;
        }
        return Results;
    }

    std::vector<Key> GetAllKeys() const {
        std::vector<Key> KeysOut;
        auto CurrentNode = Root;
        while(!CurrentNode->IsLeaf)
            CurrentNode = CurrentNode->Children.front();
        while(CurrentNode) {
            KeysOut.insert(KeysOut.end(), CurrentNode->Keys.begin(), CurrentNode->Keys.end());
            CurrentNode = CurrentNode->Next;
        }
        return KeysOut;
    }

    bool Contains(const Key& KeyValue) const {
        Value Dummy;
        return Search(KeyValue, Dummy);
    }

    std::shared_ptr<Node> GetRoot() const { return Root; }
};
} // namespace AstralDB
