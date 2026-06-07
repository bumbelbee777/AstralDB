#include <SQL/Bytecode/QueryCheckpoint.hxx>

#include <DS/Blake3.hxx>
#include <DS/XChaCha20.hxx>
#include <Database/Security/AtRestKey.hxx>

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace AstralDB {
namespace SQL {

std::atomic<bool> gRequestQueryCheckpoint{false};

namespace {

std::mutex gDominantCkptMutex;
std::optional<DominantExecutorCheckpoint> gDominantCkpt;

const char *EnvGet(const char *Name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
	return std::getenv(Name);
#pragma warning(pop)
#else
	return std::getenv(Name);
#endif
}

bool EnvTruthy(const char *Name) noexcept {
	const char *V = EnvGet(Name);
	return V != nullptr && V[0] != '\0' && V[0] != '0';
}

std::uint64_t EnvU64(const char *Name, std::uint64_t Default) {
	if(const char *V = EnvGet(Name)) {
		char *End = nullptr;
		const unsigned long long N = std::strtoull(V, &End, 10);
		if(End != V && N > 0)
			return static_cast<std::uint64_t>(N);
	}
	return Default;
}

std::string SerializePayload(const QueryCheckpointPayload &P) {
	std::ostringstream O;
	O << "QC1\n";
	O << P.DatabasePath.string() << '\n';
	O << P.InstructionPointer << ' ' << P.StackPointer << ' ' << P.BasePointer << ' ' << P.Flags << '\n';
	O << P.StepsExecuted << ' ' << (P.ExecutionCompleted ? 1 : 0) << '\n';
	O << P.PartialStats.RowsScanned << ' ' << P.PartialStats.ResultRows << ' ' << P.PartialStats.FastPathFlags
	  << ' ' << P.PartialStats.MinRowsScannedExpected << ' ' << P.PartialStats.MinResultRowsExpected << '\n';
	O << P.Registers.size() << '\n';
	for(std::uint64_t R : P.Registers)
		O << R << ' ';
	O << '\n';
	O << P.StackWords.size() << '\n';
	for(std::uint64_t W : P.StackWords)
		O << W << ' ';
	O << '\n';
	for(int I = 0; I < 32; ++I) {
		const int B = static_cast<int>(P.BytecodeDigest[static_cast<std::size_t>(I)]);
		O << B << (I + 1 < 32 ? ' ' : '\n');
	}
	if(P.Dominant.has_value()) {
		const auto &D = *P.Dominant;
		O << "DOM 1\n";
		O << D.ExecutorId << '\n';
		O << D.PhaseIndex << ' ' << D.RowCursor << ' ' << D.PartialScanned << ' ' << D.PartialResultRows << '\n';
	} else {
		O << "DOM 0\n";
	}
	return O.str();
}

bool DeserializePayload(std::string_view Text, QueryCheckpointPayload &Out) {
	const std::string TextStr(Text);
	std::istringstream In{TextStr};
	std::string Magic;
	if(!std::getline(In, Magic) || Magic != "QC1")
		return false;
	std::string DbPathLine;
	if(!std::getline(In, DbPathLine))
		return false;
	Out.DatabasePath = DbPathLine;
	{
		std::string Line;
		if(!std::getline(In, Line))
			return false;
		std::istringstream L(Line);
		L >> Out.InstructionPointer >> Out.StackPointer >> Out.BasePointer >> Out.Flags;
	}
	{
		std::string Line;
		if(!std::getline(In, Line))
			return false;
		std::istringstream L(Line);
		int Done = 0;
		L >> Out.StepsExecuted >> Done;
		Out.ExecutionCompleted = Done != 0;
	}
	{
		std::string Line;
		if(!std::getline(In, Line))
			return false;
		std::istringstream L(Line);
		L >> Out.PartialStats.RowsScanned >> Out.PartialStats.ResultRows >> Out.PartialStats.FastPathFlags >>
		    Out.PartialStats.MinRowsScannedExpected >> Out.PartialStats.MinResultRowsExpected;
	}
	std::size_t RegCount = 0;
	{
		std::string Line;
		if(!std::getline(In, Line))
			return false;
		std::istringstream L(Line);
		L >> RegCount;
	}
	Out.Registers.resize(RegCount);
	{
		std::string RegWordsLine;
		if(!std::getline(In, RegWordsLine))
			return false;
		if(RegCount > 0) {
			std::istringstream L(RegWordsLine);
			for(std::size_t I = 0; I < RegCount; ++I)
				L >> Out.Registers[I];
		}
	}
	std::size_t StackCount = 0;
	{
		std::string Line;
		if(!std::getline(In, Line))
			return false;
		std::istringstream L(Line);
		L >> StackCount;
	}
	Out.StackWords.resize(StackCount);
	{
		std::string StackWordsLine;
		if(!std::getline(In, StackWordsLine))
			return false;
		if(StackCount > 0) {
			std::istringstream L(StackWordsLine);
			for(std::size_t I = 0; I < StackCount; ++I)
				L >> Out.StackWords[I];
		}
	}
	{
		std::string Line;
		if(!std::getline(In, Line))
			return true;
		std::istringstream L(Line);
		for(int I = 0; I < 32; ++I) {
			int B = 0;
			if(!(L >> B))
				break;
			Out.BytecodeDigest[static_cast<std::size_t>(I)] = static_cast<std::uint8_t>(B);
		}
	}
	std::string DomLine;
	if(!std::getline(In, DomLine))
		return true;
	if(DomLine == "DOM 1") {
		DominantExecutorCheckpoint D;
		if(!std::getline(In, D.ExecutorId))
			return false;
		std::string PhaseLine;
		if(!std::getline(In, PhaseLine))
			return false;
		std::istringstream PL(PhaseLine);
		PL >> D.PhaseIndex >> D.RowCursor >> D.PartialScanned >> D.PartialResultRows;
		Out.Dominant = std::move(D);
	}
	return true;
}

void CaptureVmSlice(const BytecodeInterpreter &Vm, QueryCheckpointPayload &Out) {
	Out.InstructionPointer = static_cast<std::size_t>(Vm.CurrentInstruction());
	Out.StackPointer = static_cast<std::size_t>(Vm.StackTop());
	Out.BasePointer = static_cast<std::size_t>(Vm.StackBase());
	Out.Registers = Vm.Registers();
	Out.StackWords.clear();
	Out.StackWords.reserve(Out.StackPointer);
	for(std::size_t I = 0; I < Out.StackPointer; ++I)
		Out.StackWords.push_back(0);
	Out.StepsExecuted = Vm.StepsExecuted();
	Out.PartialStats = Vm.LastTimeSqlStats();
}

} // namespace

std::array<std::uint8_t, 32> HashBytecodeDigest(const Bytecode &Code) {
	std::vector<std::uint8_t> Buf;
	Buf.reserve(Code.size() * 16);
	for(const Instruction &Inst : Code) {
		Buf.push_back(static_cast<std::uint8_t>(Inst.Opcode_));
		Buf.push_back(static_cast<std::uint8_t>(Inst.Operands.size() & 255));
	}
	return Blake3::Hash(Buf);
}

std::filesystem::path DefaultQueryCheckpointPath(const std::filesystem::path &DbPath) {
	return DbPath.string() + ".query.ckpt";
}

void SaveQueryCheckpoint(const QueryCheckpointPayload &Payload, const std::filesystem::path &Path) {
	const std::string Plain = SerializePayload(Payload);
	std::array<uint8_t, 24> Nonce{};
	for(std::size_t I = 0; I < Nonce.size(); ++I)
		Nonce[I] = static_cast<uint8_t>((Plain.size() + I * 17) & 255);
	std::vector<uint8_t> In(Plain.begin(), Plain.end());
	std::vector<uint8_t> Out(In.size());
	XChaCha20 Cipher(kAtRestXChaChaKey, Nonce);
	Cipher.Encrypt(In, Out);
	std::ofstream F(Path, std::ios::binary | std::ios::trunc);
	if(!F)
		throw std::runtime_error("Cannot write query checkpoint: " + Path.string());
	F.write(reinterpret_cast<const char *>(Nonce.data()), static_cast<std::streamsize>(Nonce.size()));
	F.write(reinterpret_cast<const char *>(Out.data()), static_cast<std::streamsize>(Out.size()));
}

std::optional<QueryCheckpointPayload> LoadQueryCheckpoint(const std::filesystem::path &Path) {
	if(!std::filesystem::exists(Path))
		return std::nullopt;
	std::ifstream F(Path, std::ios::binary);
	if(!F)
		return std::nullopt;
	std::array<uint8_t, 24> Nonce{};
	F.read(reinterpret_cast<char *>(Nonce.data()), static_cast<std::streamsize>(Nonce.size()));
	std::vector<uint8_t> Enc((std::istreambuf_iterator<char>(F)), std::istreambuf_iterator<char>());
	if(Enc.empty())
		return std::nullopt;
	std::vector<uint8_t> Plain(Enc.size());
	XChaCha20 Cipher(kAtRestXChaChaKey, Nonce);
	Cipher.Decrypt(Enc, Plain);
	QueryCheckpointPayload Out;
	if(!DeserializePayload(std::string(reinterpret_cast<const char *>(Plain.data()), Plain.size()), Out))
		return std::nullopt;
	return Out;
}

bool MaybeSaveQueryCheckpoint(BytecodeInterpreter &Vm, const Bytecode &Code, const DominantExecutorCheckpoint *Dominant) {
	if(!gRequestQueryCheckpoint.load(std::memory_order_acquire))
		return false;
	QueryCheckpointPayload Payload;
	Payload.BytecodeDigest = HashBytecodeDigest(Code);
	Payload.DatabasePath = Vm.DatabasePath();
	CaptureVmSlice(Vm, Payload);
	if(Dominant)
		Payload.Dominant = *Dominant;
	const auto Path = DefaultQueryCheckpointPath(Vm.DatabasePath());
	SaveQueryCheckpoint(Payload, Path);
	gRequestQueryCheckpoint.store(false, std::memory_order_release);
	return true;
}

bool ResumeQueryCheckpoint(BytecodeInterpreter &Vm, const Bytecode &Code, const QueryCheckpointPayload &Loaded) {
	const auto Digest = HashBytecodeDigest(Code);
	const bool DigestOk = Digest == Loaded.BytecodeDigest;
	if(DigestOk) {
		Vm.SeekInstruction(Loaded.InstructionPointer);
		Vm.MutableTimeSqlStats() = Loaded.PartialStats;
	}
	if(Loaded.Dominant.has_value())
		SetDominantExecutorCheckpoint(*Loaded.Dominant);
	return DigestOk || Loaded.Dominant.has_value();
}

void ClearDominantExecutorCheckpoint() noexcept {
	std::lock_guard<std::mutex> Lk(gDominantCkptMutex);
	gDominantCkpt.reset();
}

std::optional<DominantExecutorCheckpoint> GetDominantExecutorCheckpoint() noexcept {
	std::lock_guard<std::mutex> Lk(gDominantCkptMutex);
	return gDominantCkpt;
}

void SetDominantExecutorCheckpoint(DominantExecutorCheckpoint State) noexcept {
	std::lock_guard<std::mutex> Lk(gDominantCkptMutex);
	gDominantCkpt = std::move(State);
}

std::size_t QueryCheckpointIntervalSteps() noexcept {
	return static_cast<std::size_t>(EnvU64("ASTRALDB_QUERY_CHECKPOINT_INTERVAL", 0));
}

bool QueryCheckpointOnSignalEnabled() noexcept {
	return EnvTruthy("ASTRALDB_QUERY_CHECKPOINT_ON_SIGNAL");
}

} // namespace SQL
} // namespace AstralDB
