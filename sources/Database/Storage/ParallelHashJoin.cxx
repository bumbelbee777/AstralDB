#include <Database/Storage/ParallelHashJoin.hxx>

#include <Database/Storage/JoinBloomFilter.hxx>

#include <Database/Storage/ColumnarStorage.hxx>

#include <DS/RadixPartition.hxx>

#include <DS/SimdHash.hxx>

#include <IO/Job.hxx>



#include <algorithm>

#include <future>

#include <unordered_map>

#include <vector>



namespace AstralDB {

namespace {



constexpr std::size_t kSimpleHashMax = 10000;

constexpr std::size_t kParallelHashMax = 1000000;

constexpr std::size_t kRadixPartitionThreshold = 0;

constexpr unsigned kRadixBits = 8;



using JoinIndex = std::unordered_map<std::string, std::vector<std::size_t>>;



Database::Item MergeRowsPreferLeft(const Database::Item &Left, const Database::Item &Right) {

	Database::Item Out = Left;

	for(const auto &[K, V] : Right) {

		if(Out.find(K) == Out.end())

			Out[K] = V;

	}

	return Out;

}



void BuildHashTable(const Database::Table &Build, const std::string &BuildKey, JoinIndex &Index) {

	Index.reserve(Build.size());

	for(std::size_t Bi = 0; Bi < Build.size(); ++Bi) {

		const auto It = Build[Bi].find(BuildKey);

		const std::string Key = It == Build[Bi].end() ? "" : It->second;

		Index[Key].push_back(Bi);

	}

}



JoinBloomFilter MakeBloomForIndex(const JoinIndex &Index) {

	JoinBloomFilter Bloom(Index.size());

	for(const auto &[Key, _] : Index)

		Bloom.Insert(Key);

	return Bloom;

}



void ProbeInto(Database::Table &Result, const Database::Table &Probe, const Database::Table &Build,

               const std::string &ProbeKey, const JoinIndex &Index, const JoinBloomFilter *Bloom, bool BuildIsRight,

               std::size_t PiStart, std::size_t PiEnd) {

	for(std::size_t Pi = PiStart; Pi < PiEnd; ++Pi) {

		const auto It = Probe[Pi].find(ProbeKey);

		const std::string Key = It == Probe[Pi].end() ? "" : It->second;

		if(Bloom != nullptr && !Bloom->MayContain(Key))

			continue;

		const auto Hit = Index.find(Key);

		if(Hit == Index.end())

			continue;

		for(const std::size_t Bi : Hit->second)

			Result.push_back(BuildIsRight ? MergeRowsPreferLeft(Probe[Pi], Build[Bi])

			                              : MergeRowsPreferLeft(Build[Bi], Probe[Pi]));

	}

}



void ProbeSequential(Database::Table &Result, const Database::Table &Probe, const Database::Table &Build,

                     const std::string &ProbeKey, const JoinIndex &Index, const JoinBloomFilter *Bloom,

                     bool BuildIsRight) {

	ProbeInto(Result, Probe, Build, ProbeKey, Index, Bloom, BuildIsRight, 0, Probe.size());

}



void ProbeParallel(Database::Table &Result, const Database::Table &Probe, const Database::Table &Build,

                   const std::string &ProbeKey, const JoinIndex &Index, const JoinBloomFilter *Bloom, bool BuildIsRight) {

	const unsigned Workers = std::max(1u, JobWorkerCountFromEnv());

	const std::size_t Chunk = std::max<std::size_t>(1, (Probe.size() + Workers - 1) / Workers);

	std::vector<Database::Table> Partials(Workers);

	std::vector<std::future<void>> Futs;

	for(unsigned W = 0; W < Workers; ++W) {

		const std::size_t Start = static_cast<std::size_t>(W) * Chunk;

		if(Start >= Probe.size())

			break;

		const std::size_t End = std::min(Probe.size(), Start + Chunk);

		Futs.push_back(JobSystem::Instance().SubmitAsync([&, W, Start, End]() {

			ProbeInto(Partials[W], Probe, Build, ProbeKey, Index, Bloom, BuildIsRight, Start, End);

		}));

	}

	for(auto &F : Futs)

		F.wait();

	for(auto &P : Partials) {

		for(auto &Row : P)

			Result.push_back(std::move(Row));

	}

}



void BuildHashParallel(const Database::Table &Build, const std::string &BuildKey, JoinIndex &Index) {

	const unsigned Workers = std::max(1u, JobWorkerCountFromEnv());

	const std::size_t Chunk = std::max<std::size_t>(1, (Build.size() + Workers - 1) / Workers);

	std::vector<JoinIndex> Parts(Workers);

	std::vector<std::future<void>> Futs;

	for(unsigned W = 0; W < Workers; ++W) {

		const std::size_t Start = static_cast<std::size_t>(W) * Chunk;

		if(Start >= Build.size())

			break;

		const std::size_t End = std::min(Build.size(), Start + Chunk);

		Futs.push_back(JobSystem::Instance().SubmitAsync([&, W, Start, End]() {

			auto &Local = Parts[W];

			Local.reserve(End - Start);

			for(std::size_t Bi = Start; Bi < End; ++Bi) {

				const auto It = Build[Bi].find(BuildKey);

				const std::string Key = It == Build[Bi].end() ? "" : It->second;

				Local[Key].push_back(Bi);

			}

		}));

	}

	for(auto &F : Futs)

		F.wait();

	Index.reserve(Build.size());

	for(auto &P : Parts) {

		for(auto &[Key, Vec] : P) {

			auto &Dst = Index[Key];

			Dst.insert(Dst.end(), Vec.begin(), Vec.end());

		}

	}

}



std::string JoinKeyAt(const Database::Table &Side, const std::string &KeyCol, std::size_t Idx) {

	const auto It = Side[Idx].find(KeyCol);

	return It == Side[Idx].end() ? "" : It->second;

}



void HashInnerJoinRadixPartitioned(Database::Table &Result, const Database::Table &Build, const Database::Table &Probe,

                                     const std::string &BuildKey, const std::string &ProbeKey, bool BuildIsRight) {

	RadixPartition Part(kRadixBits);

	const std::size_t PartCount = Part.PartitionCount();

	std::vector<std::vector<std::size_t>> BuildBuckets(PartCount);

	std::vector<std::vector<std::size_t>> ProbeBuckets(PartCount);

	std::vector<uint64_t> BuildHashes(Build.size());

	std::vector<uint64_t> ProbeHashes(Probe.size());

	for(std::size_t I = 0; I < Build.size(); ++I)

		BuildHashes[I] = SimdHash::Hash64(JoinKeyAt(Build, BuildKey, I));

	for(std::size_t I = 0; I < Probe.size(); ++I)

		ProbeHashes[I] = SimdHash::Hash64(JoinKeyAt(Probe, ProbeKey, I));

	std::vector<std::size_t> BuildAssign;

	std::vector<std::size_t> ProbeAssign;

	Part.Assign(BuildHashes.data(), BuildHashes.size(), BuildAssign);

	Part.Assign(ProbeHashes.data(), ProbeHashes.size(), ProbeAssign);

	for(std::size_t I = 0; I < Build.size(); ++I)

		BuildBuckets[BuildAssign[I]].push_back(I);

	for(std::size_t I = 0; I < Probe.size(); ++I)

		ProbeBuckets[ProbeAssign[I]].push_back(I);



	std::vector<Database::Table> MergedParts(PartCount);

	std::vector<std::future<void>> Futs;

	for(std::size_t P = 0; P < PartCount; ++P) {

		if(BuildBuckets[P].empty() || ProbeBuckets[P].empty())

			continue;

		Futs.push_back(JobSystem::Instance().SubmitAsync([&, P]() {

			JoinIndex LocalIndex;

			LocalIndex.reserve(BuildBuckets[P].size());

			for(const std::size_t Bi : BuildBuckets[P]) {

				const std::string Key = JoinKeyAt(Build, BuildKey, Bi);

				LocalIndex[Key].push_back(Bi);

			}

			JoinBloomFilter Bloom = MakeBloomForIndex(LocalIndex);

			const JoinBloomFilter *BloomPtr = Build.size() > kSimpleHashMax ? &Bloom : nullptr;

			Database::Table LocalProbe;

			LocalProbe.reserve(ProbeBuckets[P].size());

			for(const std::size_t Pi : ProbeBuckets[P])

				LocalProbe.push_back(Probe[Pi]);

			ProbeInto(MergedParts[P], LocalProbe, Build, ProbeKey, LocalIndex, BloomPtr, BuildIsRight, 0,

			          LocalProbe.size());

		}));

	}

	for(auto &F : Futs)

		F.wait();

	Result.clear();

	for(auto &Part : MergedParts) {

		for(auto &Row : Part)

			Result.push_back(std::move(Row));

	}

}



} // namespace



void HashInnerJoinEqualityParallel(Database::Table &Result, const Database::Table &Left, const Database::Table &Right,

                                   const std::string &LeftCol, const std::string &RightCol) {

	const Database::Table *BuildSide = &Right;

	const Database::Table *ProbeSide = &Left;

	std::string BuildKey = RightCol;

	std::string ProbeKey = LeftCol;

	if(Left.size() > Right.size()) {

		BuildSide = &Left;

		ProbeSide = &Right;

		BuildKey = LeftCol;

		ProbeKey = RightCol;

	}

	const bool BuildIsRight = BuildSide == &Right;

	Result.clear();

	if(BuildSide->size() >= kRadixPartitionThreshold && ProbeSide->size() >= kSimpleHashMax &&

	   JobSystem::Instance().IsRunning()) {

		HashInnerJoinRadixPartitioned(Result, *BuildSide, *ProbeSide, BuildKey, ProbeKey, BuildIsRight);

		return;

	}

	JoinIndex Index;

	if(BuildSide->size() >= kRadixPartitionThreshold && JobSystem::Instance().IsRunning())

		BuildHashParallel(*BuildSide, BuildKey, Index);

	else

		BuildHashTable(*BuildSide, BuildKey, Index);

	JoinBloomFilter Bloom = MakeBloomForIndex(Index);

	const JoinBloomFilter *BloomPtr = BuildSide->size() > kSimpleHashMax ? &Bloom : nullptr;

	if(ProbeSide->size() < kSimpleHashMax)

		ProbeSequential(Result, *ProbeSide, *BuildSide, ProbeKey, Index, BloomPtr, BuildIsRight);

	else if(ProbeSide->size() < kParallelHashMax && JobSystem::Instance().IsRunning())

		ProbeParallel(Result, *ProbeSide, *BuildSide, ProbeKey, Index, BloomPtr, BuildIsRight);

	else

		ProbeSequential(Result, *ProbeSide, *BuildSide, ProbeKey, Index, BloomPtr, BuildIsRight);

}



bool TryColumnarInnerJoinEqualityParallel(const ColumnarTable &Left, const ColumnarTable &Right,

                                          const std::string &LeftCol, const std::string &RightCol, RowTable &Result) {

	if(Left.BulkSyntheticLazy || Right.BulkSyntheticLazy)

		return false;

	return TryColumnarInnerJoinEquality(Left, Right, LeftCol, RightCol, Result);

}



} // namespace AstralDB


