#include <SQL/SQL.hxx>

namespace AstralDB {
namespace SQL {

Tree<ASTNode> AST{};

HybridAST::HybridAST()
	: BPTreeRoot_(std::make_unique<BPTree>()), RadixRoot_(nullptr), UseRadix_(false) {}

void HybridAST::Add(const KeyType &Key, ValueType Node, size_t Depth) {
	if(Depth < SwitchDepth) {
		if(!BPTreeRoot_) BPTreeRoot_ = std::make_unique<BPTree>();
		BPTreeRoot_->Insert(Key, std::move(Node));
	} else {
		if(!RadixRoot_) RadixRoot_ = std::make_unique<RTree>();
		RadixRoot_->Insert(Key, std::move(Node), 0); // RadixTree expects rvalue
		UseRadix_ = true;
	}
}

bool HybridAST::Empty() const {
	if(UseRadix_ && RadixRoot_) return RadixRoot_->Empty();
	return BPTreeRoot_ ? BPTreeRoot_->GetAllKeys().empty() : true;
}

ExpressionAST *HybridAST::Find(const KeyType &Key, size_t Depth) {
	if(Depth < SwitchDepth && BPTreeRoot_) {
		ValueType *Ptr = BPTreeRoot_->GetPointer(Key);
		if(Ptr && Ptr->get())
			return Ptr->get();
	}
	if(RadixRoot_) {
		auto Result = RadixRoot_->Find(Key);
		if(Result)
			return Result;
	}
	return nullptr;
}

} // namespace SQL
} // namespace AstralDB