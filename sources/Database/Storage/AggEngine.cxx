#include <Database/Storage/OlapAggregateMicrokernels.hxx>
#include <Database/Storage/AggEngine.hxx>

#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Execution/BytecodeTypes.hxx>

#include <atomic>
#include <cmath>
#include <future>
#include <mutex>
#include <sstream>

namespace AstralDB {

namespace {

AggEngineMode ParseEnvMode() {
	if(const char *V = std::getenv("ASTRALDB_AGG_ENGINE")) {
		if(V[0] == 'l' || V[0] == 'L')
			return AggEngineMode::Legacy;
		if(V[0] == 'a' || V[0] == 'A')
			return AggEngineMode::Async;
	}
	return AggEngineMode::Simd;
}

struct AggSpec {
	SQL::GroupCombAggKind Kind = SQL::GroupCombAggKind::Sum;
	std::string SrcCol;
	std::string OutCol;
	double Quantile = 0.5;
};

std::string GroupKeySig(const RowItem &Row, const std::vector<std::string> &Keys) {
	std::string S;
	for(const auto &K : Keys) {
		S.push_back('\1');
		auto It = Row.find(K);
		S += It == Row.end() ? "" : It->second;
	}
	return S;
}

} // namespace

AggEngineMode ParseAggEngineMode() { return ParseEnvMode(); }

bool AggEngine::TryRun(RowTable &Tbl, const SQL::Instruction &Inst, const std::vector<std::string> &ActiveKeys) {
	const AggEngineMode Mode = ParseAggEngineMode();
	if(Mode == AggEngineMode::Legacy)
		return false;
	if(ColumnarGroupBy::TryRun(Tbl, Inst, ActiveKeys))
		return true;

	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	if(!Tag)
		return false;
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Nk)
		return false;

	if(*Tag == 1) {
		const size_t Base = static_cast<size_t>(2 + *Nk);
		std::string CountStarCol = "cnt";
		if(Inst.Operands.size() == Base + 1) {
			if(const auto *Cn = std::get_if<std::string>(&Inst.Operands[Base]); Cn && !Cn->empty())
				CountStarCol = *Cn;
		} else if(Inst.Operands.size() != Base)
			return false;

		std::unordered_map<std::string, int64_t> Cnt;
		std::unordered_map<std::string, RowItem> Template;
		Cnt.reserve(Tbl.size());
		Template.reserve(Tbl.size());
		for(const auto &Row : Tbl) {
			const std::string Sig = GroupKeySig(Row, ActiveKeys);
			++Cnt[Sig];
			if(Template.find(Sig) == Template.end()) {
				RowItem R;
				for(const auto &K : ActiveKeys) {
					auto It = Row.find(K);
					R[K] = It == Row.end() ? "" : It->second;
				}
				Template.emplace(Sig, std::move(R));
			}
		}
		RowTable Out;
		Out.reserve(Cnt.size());
		for(auto &[Sig, N] : Cnt) {
			RowItem R = Template[Sig];
			R[CountStarCol] = std::to_string(N);
			Out.push_back(std::move(R));
		}
		Tbl = std::move(Out);
		return true;
	}

	if(*Tag == 0) {
		std::unordered_map<std::string, RowItem> First;
		First.reserve(Tbl.size());
		for(const auto &Row : Tbl) {
			const std::string Sig = GroupKeySig(Row, ActiveKeys);
			First.try_emplace(Sig, Row);
		}
		RowTable Out;
		Out.reserve(First.size());
		for(auto &[Sig, R] : First)
			Out.push_back(std::move(R));
		Tbl = std::move(Out);
		return true;
	}

	if(*Tag != 3)
		return false;
	const size_t Base = static_cast<size_t>(2 + *Nk);
	if(Inst.Operands.size() < Base + 2)
		return false;
	const auto *HCnt = std::get_if<int64_t>(&Inst.Operands[Base]);
	const auto *Na = std::get_if<int64_t>(&Inst.Operands[Base + 1]);
	if(!HCnt || !Na || *Na <= 0)
		return false;
	const bool IncludeCountStar = (*HCnt != 0);
	const size_t IdxAfterSpecs = Base + 2 + static_cast<size_t>(*Na) * 3;
	std::string CountStarCol = "cnt";
	if(IncludeCountStar && Inst.Operands.size() > IdxAfterSpecs) {
		if(const auto *Cn = std::get_if<std::string>(&Inst.Operands[IdxAfterSpecs]); Cn && !Cn->empty())
			CountStarCol = *Cn;
	}

	std::vector<AggSpec> Specs;
	Specs.reserve(static_cast<size_t>(*Na));
	size_t Idx = Base + 2;
	for(int64_t A = 0; A < *Na; ++A) {
		const auto *Knd = std::get_if<int64_t>(&Inst.Operands[Idx++]);
		const auto *Sc = std::get_if<std::string>(&Inst.Operands[Idx++]);
		const auto *Ou = std::get_if<std::string>(&Inst.Operands[Idx++]);
		if(!Knd || !Sc || !Ou)
			return false;
		AggSpec Sp;
		Sp.Kind = static_cast<SQL::GroupCombAggKind>(*Knd);
		Sp.SrcCol = *Sc;
		Sp.OutCol = *Ou;
		Specs.push_back(std::move(Sp));
	}

	struct GroupAcc {
		RowItem KeyRow;
		std::vector<double> Sum;
		std::vector<int64_t> AvgN;
		std::vector<WelfordState> Welford;
		std::vector<TDigestState> Digest;
		std::vector<FreqSketchState> Mode;
		std::vector<bool> HaveMinMax;
		std::vector<std::string> CurMin;
		std::vector<std::string> CurMax;
		int64_t CntStar = 0;
	};

	std::unordered_map<std::string, GroupAcc> Acc;
	CuckooMap KeyMap;
	RadixPartition Part(8);

	auto AccumulateRow = [&](const RowItem &Row) {
		const std::string Sig = GroupKeySig(Row, ActiveKeys);
		uint32_t Slot = KeyMap.FindOrInsert(Sig);
		(void)Slot;
		auto &G = Acc[Sig];
		if(G.KeyRow.empty()) {
			for(const auto &K : ActiveKeys) {
				auto It = Row.find(K);
				G.KeyRow[K] = It == Row.end() ? "" : It->second;
			}
			G.Sum.assign(Specs.size(), 0.0);
			G.AvgN.assign(Specs.size(), 0);
			G.Welford.assign(Specs.size(), {});
			G.Digest.assign(Specs.size(), {});
			G.Mode.assign(Specs.size(), {});
			G.HaveMinMax.assign(Specs.size(), false);
			G.CurMin.assign(Specs.size(), {});
			G.CurMax.assign(Specs.size(), {});
		}
		++G.CntStar;
		for(std::size_t Si = 0; Si < Specs.size(); ++Si) {
			const auto &Sp = Specs[Si];
			auto It = Row.find(Sp.SrcCol);
			const std::string Cell = It == Row.end() ? "" : It->second;
			switch(Sp.Kind) {
			case SQL::GroupCombAggKind::Sum:
			case SQL::GroupCombAggKind::Avg: {
				OlapAggregateMicrokernels::FusedParseAddF64(Cell, G.Sum[Si]);
				if(Sp.Kind == SQL::GroupCombAggKind::Avg)
					++G.AvgN[Si];
			} break;
			case SQL::GroupCombAggKind::Min:
				if(!G.HaveMinMax[Si] || Cell < G.CurMin[Si]) {
					G.HaveMinMax[Si] = true;
					G.CurMin[Si] = Cell;
				}
				break;
			case SQL::GroupCombAggKind::Max:
				if(!G.HaveMinMax[Si] || Cell > G.CurMax[Si]) {
					G.HaveMinMax[Si] = true;
					G.CurMax[Si] = Cell;
				}
				break;
			case SQL::GroupCombAggKind::StdDevPop:
			case SQL::GroupCombAggKind::StdDevSamp: {
				try {
					G.Welford[Si].Add(std::stod(Cell));
				} catch(...) {
				}
			} break;
			case SQL::GroupCombAggKind::Median:
			case SQL::GroupCombAggKind::ApproxQuantile: {
				try {
					G.Digest[Si].Add(std::stod(Cell));
				} catch(...) {
				}
			} break;
			case SQL::GroupCombAggKind::Mode:
				if(!Cell.empty())
					G.Mode[Si].Add(Cell);
				break;
			}
		}
	};

	if(Mode == AggEngineMode::Async && JobSystem::Instance().IsRunning()) {
		const std::size_t Partitions = Part.PartitionCount();
		std::vector<std::vector<const RowItem *>> Buckets(Partitions);
		std::vector<uint64_t> Hashes;
		Hashes.reserve(Tbl.size());
		for(const auto &Row : Tbl)
			Hashes.push_back(SimdHash::Hash64(GroupKeySig(Row, ActiveKeys)));
		std::vector<std::size_t> Assign;
		Part.Assign(Hashes.data(), Hashes.size(), Assign);
		for(std::size_t I = 0; I < Tbl.size(); ++I)
			Buckets[Assign[I]].push_back(&Tbl[I]);

		std::vector<std::future<std::unordered_map<std::string, GroupAcc>>> Futs;
		Futs.reserve(Partitions);
		for(std::size_t P = 0; P < Partitions; ++P) {
			if(Buckets[P].empty())
				continue;
			Futs.push_back(JobSystem::Instance().SubmitAsync([Buckets = Buckets[P], ActiveKeys, Specs]() {
				std::unordered_map<std::string, GroupAcc> Local;
				CuckooMap LocalMap;
				for(const RowItem *RowPtr : Buckets) {
					const std::string Sig = GroupKeySig(*RowPtr, ActiveKeys);
					(void)LocalMap.FindOrInsert(Sig);
					auto &G = Local[Sig];
					if(G.KeyRow.empty()) {
						for(const auto &K : ActiveKeys) {
							auto It = RowPtr->find(K);
							G.KeyRow[K] = It == RowPtr->end() ? "" : It->second;
						}
						G.Sum.assign(Specs.size(), 0.0);
						G.AvgN.assign(Specs.size(), 0);
						G.Welford.assign(Specs.size(), {});
						G.Digest.assign(Specs.size(), {});
						G.Mode.assign(Specs.size(), {});
						G.HaveMinMax.assign(Specs.size(), false);
						G.CurMin.assign(Specs.size(), {});
						G.CurMax.assign(Specs.size(), {});
					}
					++G.CntStar;
					for(std::size_t Si = 0; Si < Specs.size(); ++Si) {
						const auto &Sp = Specs[Si];
						auto It = RowPtr->find(Sp.SrcCol);
						const std::string Cell = It == RowPtr->end() ? "" : It->second;
						switch(Sp.Kind) {
						case SQL::GroupCombAggKind::Sum:
						case SQL::GroupCombAggKind::Avg: {
							OlapAggregateMicrokernels::FusedParseAddF64(Cell, G.Sum[Si]);
							if(Sp.Kind == SQL::GroupCombAggKind::Avg)
								++G.AvgN[Si];
						} break;
						case SQL::GroupCombAggKind::Min:
							if(!G.HaveMinMax[Si] || Cell < G.CurMin[Si]) {
								G.HaveMinMax[Si] = true;
								G.CurMin[Si] = Cell;
							}
							break;
						case SQL::GroupCombAggKind::Max:
							if(!G.HaveMinMax[Si] || Cell > G.CurMax[Si]) {
								G.HaveMinMax[Si] = true;
								G.CurMax[Si] = Cell;
							}
							break;
						case SQL::GroupCombAggKind::StdDevPop:
						case SQL::GroupCombAggKind::StdDevSamp: {
							try {
								G.Welford[Si].Add(std::stod(Cell));
							} catch(...) {
							}
						} break;
						case SQL::GroupCombAggKind::Median:
						case SQL::GroupCombAggKind::ApproxQuantile: {
							try {
								G.Digest[Si].Add(std::stod(Cell));
							} catch(...) {
							}
						} break;
						case SQL::GroupCombAggKind::Mode:
							if(!Cell.empty())
								G.Mode[Si].Add(Cell);
							break;
						}
					}
				}
				return Local;
			}));
		}
		for(auto &F : Futs) {
			auto Local = F.get();
			for(auto &[Sig, LG] : Local) {
				auto &G = Acc[Sig];
				if(G.KeyRow.empty())
					G = std::move(LG);
				else {
					G.CntStar += LG.CntStar;
					for(std::size_t Si = 0; Si < Specs.size(); ++Si) {
						G.Sum[Si] += LG.Sum[Si];
						G.AvgN[Si] += LG.AvgN[Si];
						G.Welford[Si].Merge(LG.Welford[Si]);
						G.Digest[Si].Merge(LG.Digest[Si]);
						G.Mode[Si].Merge(LG.Mode[Si]);
						if(LG.HaveMinMax[Si] && (!G.HaveMinMax[Si] || LG.CurMin[Si] < G.CurMin[Si])) {
							G.HaveMinMax[Si] = true;
							G.CurMin[Si] = LG.CurMin[Si];
						}
						if(LG.HaveMinMax[Si] && (!G.HaveMinMax[Si] || LG.CurMax[Si] > G.CurMax[Si])) {
							G.HaveMinMax[Si] = true;
							G.CurMax[Si] = LG.CurMax[Si];
						}
					}
				}
			}
		}
	} else {
		for(const auto &Row : Tbl)
			AccumulateRow(Row);
	}

	RowTable Out;
	Out.reserve(Acc.size());
	for(auto &[Sig, G] : Acc) {
		RowItem R = std::move(G.KeyRow);
		if(IncludeCountStar)
			R[CountStarCol] = std::to_string(G.CntStar);
		for(std::size_t Si = 0; Si < Specs.size(); ++Si) {
			const auto &Sp = Specs[Si];
			switch(Sp.Kind) {
			case SQL::GroupCombAggKind::Sum:
				R[Sp.OutCol] = std::to_string(static_cast<long long>(std::llround(G.Sum[Si])));
				break;
			case SQL::GroupCombAggKind::Avg: {
				std::ostringstream O;
				O << (G.AvgN[Si] > 0 ? G.Sum[Si] / static_cast<double>(G.AvgN[Si]) : 0.0);
				R[Sp.OutCol] = O.str();
			} break;
			case SQL::GroupCombAggKind::Min:
				R[Sp.OutCol] = G.HaveMinMax[Si] ? G.CurMin[Si] : "";
				break;
			case SQL::GroupCombAggKind::Max:
				R[Sp.OutCol] = G.HaveMinMax[Si] ? G.CurMax[Si] : "";
				break;
			case SQL::GroupCombAggKind::StdDevPop: {
				std::ostringstream O;
				O << G.Welford[Si].StdDevPop();
				R[Sp.OutCol] = O.str();
			} break;
			case SQL::GroupCombAggKind::StdDevSamp: {
				std::ostringstream O;
				O << G.Welford[Si].StdDevSamp();
				R[Sp.OutCol] = O.str();
			} break;
			case SQL::GroupCombAggKind::Median: {
				std::ostringstream O;
				O << G.Digest[Si].Median();
				R[Sp.OutCol] = O.str();
			} break;
			case SQL::GroupCombAggKind::ApproxQuantile: {
				std::ostringstream O;
				O << G.Digest[Si].Quantile(Sp.Quantile);
				R[Sp.OutCol] = O.str();
			} break;
			case SQL::GroupCombAggKind::Mode:
				R[Sp.OutCol] = G.Mode[Si].Mode();
				break;
			}
		}
		Out.push_back(std::move(R));
	}
	Tbl = std::move(Out);
	return true;
}

} // namespace AstralDB
