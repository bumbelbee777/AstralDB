#include <SQL/Rewrite/RewriteOps.hxx>

#include <Database/Storage/HierarchyPath.hxx>
#include <IO/Limits.hxx>
#include <SQL/Bytecode/Bytecode.hxx>

#include <algorithm>
#include <unordered_set>

namespace AstralDB {
namespace SQL {

namespace {

std::string RowSignatureCanon(const std::unordered_map<std::string, std::string> &Row) {
	std::vector<std::string> Keys;
	Keys.reserve(Row.size());
	for(const auto &Kv : Row)
		Keys.push_back(Kv.first);
	std::sort(Keys.begin(), Keys.end());
	std::string Out;
	for(const std::string &K : Keys) {
		Out += K;
		Out.push_back('\x1E');
		const auto It = Row.find(K);
		Out += It == Row.end() ? "" : It->second;
		Out.push_back('\x1F');
	}
	return Out;
}

bool MergeDeltaBfs(Database &Db, const std::string &WorkNm, const std::string &DeltaNm, HierarchyPathIndex &Index) {
	bool Grew = false;
	Db.WithExclusiveBytecodeLock([&]() {
		if(TryRecursiveCteHierarchyMerge(Db, WorkNm, DeltaNm, Index)) {
			Grew = true;
			return;
		}
		auto WIt = Db.Tables_.find(WorkNm);
		auto DIt = Db.Tables_.find(DeltaNm);
		if(WIt == Db.Tables_.end() || DIt == Db.Tables_.end())
			return;
		std::unordered_set<std::string> Seen;
		for(const auto &R : WIt->second.RowStore)
			Seen.insert(RowSignatureCanon(R));
		for(const auto &R : DIt->second.RowStore) {
			if(Seen.insert(RowSignatureCanon(R)).second) {
				WIt->second.RowStore.push_back(R);
				Grew = true;
			}
		}
		DIt->second.RowStore.clear();
	});
	return Grew;
}

} // namespace

bool RewriteOps::SemiJoinProbe(Database &Db, const std::string &OuterTable, const std::string &InnerTable,
                               const std::string &OuterKey, const std::string &InnerKey, bool Negated,
                               const std::string &InnerFilterDnf,
                               const std::unordered_map<std::string, std::string> &OuterRow) {
	(void)OuterTable;
	(void)InnerFilterDnf;
	const auto It = OuterRow.find(OuterKey);
	if(It == OuterRow.end())
		return Negated;
	const auto IIt = Db.Tables_.find(InnerTable);
	if(IIt == Db.Tables_.end())
		return Negated;
	for(const auto &InnerRow : IIt->second.RowStore) {
		const auto Ik = InnerRow.find(InnerKey);
		if(Ik != InnerRow.end() && Ik->second == It->second)
			return !Negated;
	}
	return Negated;
}

bool RewriteOps::HandleRecursiveCteBfs(BytecodeInterpreter &Vm, const Bytecode &Code, std::size_t FixIp,
                                       const Instruction &Inst) {
	if(Inst.Operands.size() < 4)
		return false;
	const auto *WorkNm = std::get_if<std::string>(&Inst.Operands[0]);
	const auto *DeltaNm = std::get_if<std::string>(&Inst.Operands[1]);
	const auto *MaxIterOp = std::get_if<int64_t>(&Inst.Operands[2]);
	const auto *LoopStartOp = std::get_if<int64_t>(&Inst.Operands[3]);
	if(!WorkNm || WorkNm->empty() || !DeltaNm || DeltaNm->empty() || !MaxIterOp || !LoopStartOp)
		return false;
	if(*MaxIterOp < 0)
		return false;
	const std::size_t LoopStart = static_cast<std::size_t>(*LoopStartOp);
	if(LoopStart > FixIp)
		return false;
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;
	int64_t Remaining = *MaxIterOp;
	static thread_local HierarchyPathIndex CteHierarchy;
	if(Remaining == *MaxIterOp)
		CteHierarchy.Clear();
	if(!MergeDeltaBfs(*Db, *WorkNm, *DeltaNm, CteHierarchy))
		Remaining = 0;
	while(Remaining > 0) {
		Vm.SeekInstruction(LoopStart);
		while(Vm.CurrentInstruction() < FixIp) {
			if(Vm.StepsExecuted() > Limits::MaxInterpreterSteps)
				return false;
			if(!Vm.Step(Code))
				return false;
		}
		if(!MergeDeltaBfs(*Db, *WorkNm, *DeltaNm, CteHierarchy))
			break;
		--Remaining;
	}
	Vm.SeekInstruction(FixIp + 1);
	return true;
}

bool RewriteOps::HandleSemiJoinHash(BytecodeInterpreter &Vm, const Instruction &Inst) {
	if(Inst.Operands.size() < 5)
		return false;
	const auto *OuterTable = std::get_if<std::string>(&Inst.Operands[0]);
	const auto *InnerTable = std::get_if<std::string>(&Inst.Operands[1]);
	const auto *OuterKey = std::get_if<std::string>(&Inst.Operands[2]);
	const auto *InnerKey = std::get_if<std::string>(&Inst.Operands[3]);
	const auto *NegOp = std::get_if<int64_t>(&Inst.Operands[4]);
	if(!OuterTable || !InnerTable || !OuterKey || !InnerKey || !NegOp)
		return false;
	const bool Negated = *NegOp != 0;
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;
	std::unordered_set<std::string> Build;
	Db->WithExclusiveBytecodeLock([&]() {
		auto IIt = Db->Tables_.find(*InnerTable);
		if(IIt == Db->Tables_.end())
			return;
		for(const auto &R : IIt->second.RowStore) {
			const auto It = R.find(*InnerKey);
			if(It != R.end())
				Build.insert(It->second);
		}
		auto OIt = Db->Tables_.find(*OuterTable);
		if(OIt == Db->Tables_.end())
			return;
		std::vector<std::unordered_map<std::string, std::string>> Kept;
		for(const auto &R : OIt->second.RowStore) {
			const auto It = R.find(*OuterKey);
			const bool Hit = It != R.end() && Build.count(It->second) > 0;
			if(Negated ? !Hit : Hit)
				Kept.push_back(R);
		}
		OIt->second.RowStore = std::move(Kept);
	});
	return true;
}

bool RewriteOps::HandleHierarchyPathScan(BytecodeInterpreter &Vm, const Instruction &Inst) {
	if(Inst.Operands.size() < 4)
		return false;
	const auto *Dest = std::get_if<std::string>(&Inst.Operands[0]);
	const auto *Src = std::get_if<std::string>(&Inst.Operands[1]);
	const auto *IdCol = std::get_if<std::string>(&Inst.Operands[2]);
	const auto *ParentCol = std::get_if<std::string>(&Inst.Operands[3]);
	if(!Dest || !Src || !IdCol || !ParentCol || Dest->empty() || Src->empty())
		return false;
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;
	HierarchyPathIndex Index;
	Db->WithExclusiveBytecodeLock([&]() {
		auto SIt = Db->Tables_.find(*Src);
		auto DIt = Db->Tables_.find(*Dest);
		if(SIt == Db->Tables_.end() || DIt == Db->Tables_.end())
			return;
		Index.TryBuildFromTable(SIt->second.RowStore, *IdCol, *ParentCol);
		DIt->second.RowStore = SIt->second.RowStore;
		DIt->second.RecordWrite();
		DIt->second.SyncColumnarAfterRowMutation();
	});
	(void)Index;
	return true;
}

} // namespace SQL
} // namespace AstralDB
