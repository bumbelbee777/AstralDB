#include <Database/Storage/SparseCube.hxx>

#include <Database/Storage/AggEngine.hxx>

#include <algorithm>
#include <cstdlib>

namespace AstralDB {

namespace {

void PadOlapRows(RowTable &Tbl, const std::vector<std::string> &AllKeys, const std::vector<std::string> &Active,
                 int64_t OlapLevel) {
	for(auto &Row : Tbl) {
		int64_t GroupingBitmap = 0;
		for(size_t Ki = 0; Ki < AllKeys.size(); ++Ki) {
			const auto &K = AllKeys[Ki];
			const bool KeyActive = std::find(Active.begin(), Active.end(), K) != Active.end();
			if(!KeyActive)
				Row[K] = "";
			const bool Grouped = !KeyActive;
			Row["_grouping_" + K] = Grouped ? "1" : "0";
			if(Grouped)
				GroupingBitmap |= (int64_t{1} << static_cast<int>(Ki));
		}
		Row["_olap_level"] = std::to_string(OlapLevel);
		Row["_grouping_id"] = std::to_string(GroupingBitmap);
	}
}

bool SparseEnabled() {
	if(const char *V = std::getenv("ASTRALDB_SPARSE_CUBE")) {
		if(V[0] == '0' || V[0] == 'n' || V[0] == 'N')
			return false;
	}
	return true;
}

RowItem ProjectRow(const RowItem &Row, const std::vector<std::string> &Active) {
	RowItem Out;
	for(const auto &K : Active) {
		auto It = Row.find(K);
		Out[K] = It == Row.end() ? "" : It->second;
	}
	return Out;
}

} // namespace

void SparseCube::RunCube(RowTable &Tbl, const SQL::Instruction &Inst, const std::vector<std::string> &AllKeys) {
	const RowTable Source = Tbl;
	const size_t N = AllKeys.size();
	if(N > 8)
		return;
	const size_t Sets = size_t{1} << N;

	if(!SparseEnabled()) {
		RowTable Combined;
		for(size_t Mask = 0; Mask < Sets; ++Mask) {
			std::vector<std::string> Active;
			for(size_t I = 0; I < N; ++I) {
				if((Mask >> I) & 1)
					Active.push_back(AllKeys[I]);
			}
			RowTable Part = Source;
			if(!AggEngine::TryRun(Part, Inst, Active))
				return;
			PadOlapRows(Part, AllKeys, Active, static_cast<int64_t>(Mask));
			Combined.insert(Combined.end(), std::make_move_iterator(Part.begin()), std::make_move_iterator(Part.end()));
		}
		Tbl = std::move(Combined);
		return;
	}

	std::vector<RowTable> Parts(Sets);
	for(const auto &Row : Source) {
		for(size_t Mask = 0; Mask < Sets; ++Mask) {
			std::vector<std::string> Active;
			Active.reserve(N);
			for(size_t I = 0; I < N; ++I) {
				if((Mask >> I) & 1)
					Active.push_back(AllKeys[I]);
			}
			RowItem P = ProjectRow(Row, Active);
			for(const auto &[K, V] : Row)
				P.try_emplace(K, V);
			Parts[Mask].push_back(std::move(P));
		}
	}

	RowTable Combined;
	for(size_t Mask = 0; Mask < Sets; ++Mask) {
		std::vector<std::string> Active;
		for(size_t I = 0; I < N; ++I) {
			if((Mask >> I) & 1)
				Active.push_back(AllKeys[I]);
		}
		RowTable &Part = Parts[Mask];
		if(!AggEngine::TryRun(Part, Inst, Active))
			return;
		PadOlapRows(Part, AllKeys, Active, static_cast<int64_t>(Mask));
		Combined.insert(Combined.end(), std::make_move_iterator(Part.begin()), std::make_move_iterator(Part.end()));
	}
	Tbl = std::move(Combined);
}

void SparseCube::RunRollup(RowTable &Tbl, const SQL::Instruction &Inst, const std::vector<std::string> &AllKeys) {
	const RowTable Source = Tbl;
	RowTable Combined;
	const int MaxLevel = static_cast<int>(AllKeys.size());
	for(int Level = MaxLevel; Level >= 0; --Level) {
		std::vector<std::string> Active(AllKeys.begin(), AllKeys.begin() + Level);
		RowTable Part;
		if(SparseEnabled()) {
			Part.reserve(Source.size());
			for(const auto &Row : Source) {
				RowItem P = ProjectRow(Row, Active);
				for(const auto &[K, V] : Row)
					P.try_emplace(K, V);
				Part.push_back(std::move(P));
			}
		} else {
			Part = Source;
		}
		if(!AggEngine::TryRun(Part, Inst, Active))
			return;
		PadOlapRows(Part, AllKeys, Active, static_cast<int64_t>(Level));
		Combined.insert(Combined.end(), std::make_move_iterator(Part.begin()), std::make_move_iterator(Part.end()));
	}
	Tbl = std::move(Combined);
}

} // namespace AstralDB
