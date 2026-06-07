#include <SQL/Bytecode/BytecodeFormat.hxx>
#include <IO/Error.hxx>
#include <fstream>
#include <stdexcept>

namespace AstralDB {
namespace SQL {

namespace {

void WriteOperand(std::ostream &Out, const Value &Operand) {
	std::visit([&Out](const auto &V) {
		using T = std::decay_t<decltype(V)>;
		if constexpr(std::is_same_v<T, int64_t>) {
			const std::uint8_t Type = 0;
			Out.write(reinterpret_cast<const char *>(&Type), sizeof(Type));
			Out.write(reinterpret_cast<const char *>(&V), sizeof(V));
		} else if constexpr(std::is_same_v<T, std::string>) {
			const std::uint8_t Type = 1;
			Out.write(reinterpret_cast<const char *>(&Type), sizeof(Type));
			const std::uint32_t Length = static_cast<std::uint32_t>(V.length());
			Out.write(reinterpret_cast<const char *>(&Length), sizeof(Length));
			Out.write(V.data(), static_cast<std::streamsize>(Length));
		}
	}, Operand);
}

Instruction ReadInstruction(std::istream &In) {
	std::uint8_t OpcodeByte = 0;
	In.read(reinterpret_cast<char *>(&OpcodeByte), sizeof(OpcodeByte));
	std::uint32_t NumOperands = 0;
	In.read(reinterpret_cast<char *>(&NumOperands), sizeof(NumOperands));
	Instruction Inst;
	Inst.Opcode_ = static_cast<Opcode>(OpcodeByte);
	Inst.Operands.reserve(NumOperands);
	for(std::uint32_t J = 0; J < NumOperands; ++J) {
		std::uint8_t Type = 0;
		In.read(reinterpret_cast<char *>(&Type), sizeof(Type));
		if(Type == 0) {
			int64_t Value = 0;
			In.read(reinterpret_cast<char *>(&Value), sizeof(Value));
			Inst.Operands.push_back(Value);
		} else if(Type == 1) {
			std::uint32_t Length = 0;
			In.read(reinterpret_cast<char *>(&Length), sizeof(Length));
			std::string Value(Length, '\0');
			In.read(Value.data(), static_cast<std::streamsize>(Length));
			Inst.Operands.push_back(std::move(Value));
		} else {
			throw std::runtime_error(AstralDB::Err::Prefixed(
			    "ABC", "Unknown operand type tag " + std::to_string(Type) + " in bytecode file."));
		}
	}
	return Inst;
}

void ReadStringPoolTrailer(std::istream &In, std::vector<std::string> &PoolOut) {
	if(!In || In.peek() == std::char_traits<char>::eof())
		return;
	std::uint32_t Magic = 0;
	In.read(reinterpret_cast<char *>(&Magic), sizeof(Magic));
	if(!In)
		return;
	if(Magic != kAbcStringPoolTrailerMagic)
		throw std::runtime_error(AstralDB::Err::Prefixed("ABC", "Trailing bytes present but string-pool magic mismatch."));
	std::uint32_t Count = 0;
	In.read(reinterpret_cast<char *>(&Count), sizeof(Count));
	PoolOut.clear();
	PoolOut.reserve(Count);
	for(std::uint32_t I = 0; I < Count; ++I) {
		std::uint32_t Length = 0;
		In.read(reinterpret_cast<char *>(&Length), sizeof(Length));
		std::string S(Length, '\0');
		In.read(S.data(), static_cast<std::streamsize>(Length));
		PoolOut.push_back(std::move(S));
	}
}

} // namespace

LoadedAbcFile LoadAbcFile(const std::filesystem::path &Path) {
	std::ifstream In(Path, std::ios::binary);
	if(!In)
		throw std::runtime_error(
		    AstralDB::Err::Prefixed("ABC", "Cannot open bytecode file for read: " + Path.string()));
	LoadedAbcFile Out;
	In.read(reinterpret_cast<char *>(&Out.ContainerVersion), sizeof(Out.ContainerVersion));
	if(Out.ContainerVersion != kAbcContainerVersion)
		throw std::runtime_error(AstralDB::Err::Prefixed(
		    "ABC", "Unsupported bytecode container version " + std::to_string(Out.ContainerVersion) +
		               " (expected " + std::to_string(kAbcContainerVersion) + ")."));
	std::uint32_t NumInstructions = 0;
	In.read(reinterpret_cast<char *>(&NumInstructions), sizeof(NumInstructions));
	Out.Instructions.reserve(NumInstructions);
	for(std::uint32_t I = 0; I < NumInstructions; ++I)
		Out.Instructions.push_back(ReadInstruction(In));
	if(!In)
		throw std::runtime_error(AstralDB::Err::Prefixed("ABC", "Truncated bytecode instruction stream."));
	ReadStringPoolTrailer(In, Out.StringPool);
	return Out;
}

void SaveAbcFile(const std::filesystem::path &Path, const Bytecode &Code, const std::vector<std::string> *StringPool) {
	std::ofstream Out(Path, std::ios::binary);
	if(!Out)
		throw std::runtime_error(
		    AstralDB::Err::Prefixed("ABC", "Cannot open bytecode file for write: " + Path.string()));
	const std::uint32_t Version = kAbcContainerVersion;
	Out.write(reinterpret_cast<const char *>(&Version), sizeof(Version));
	const std::uint32_t NumInstructions = static_cast<std::uint32_t>(Code.size());
	Out.write(reinterpret_cast<const char *>(&NumInstructions), sizeof(NumInstructions));
	for(const Instruction &Inst : Code) {
		const std::uint8_t OpcodeByte = static_cast<std::uint8_t>(Inst.Opcode_);
		Out.write(reinterpret_cast<const char *>(&OpcodeByte), sizeof(OpcodeByte));
		const std::uint32_t NumOperands = static_cast<std::uint32_t>(Inst.Operands.size());
		Out.write(reinterpret_cast<const char *>(&NumOperands), sizeof(NumOperands));
		for(const auto &Operand : Inst.Operands)
			WriteOperand(Out, Operand);
	}
	if(StringPool && !StringPool->empty()) {
		const std::uint32_t Magic = kAbcStringPoolTrailerMagic;
		Out.write(reinterpret_cast<const char *>(&Magic), sizeof(Magic));
		const std::uint32_t Count = static_cast<std::uint32_t>(StringPool->size());
		Out.write(reinterpret_cast<const char *>(&Count), sizeof(Count));
		for(const std::string &S : *StringPool) {
			const std::uint32_t Length = static_cast<std::uint32_t>(S.size());
			Out.write(reinterpret_cast<const char *>(&Length), sizeof(Length));
			Out.write(S.data(), static_cast<std::streamsize>(Length));
		}
	}
}

void SaveAbcFile(const std::filesystem::path &Path, const CompiledBytecode &Compiled) {
	const std::vector<std::string> *Pool =
	    Compiled.StringPool.empty() ? nullptr : &Compiled.StringPool;
	SaveAbcFile(Path, Compiled.Instructions, Pool);
}

} // namespace SQL
} // namespace AstralDB
