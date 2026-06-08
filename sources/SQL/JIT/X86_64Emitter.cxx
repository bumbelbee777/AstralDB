#include <SQL/JIT/JitCompiler.hxx>

namespace AstralDB {
namespace SQL {

namespace {

void Emit(std::vector<std::uint8_t> &Out, std::initializer_list<std::uint8_t> Bytes) {
	Out.insert(Out.end(), Bytes.begin(), Bytes.end());
}

void Patch32(std::vector<std::uint8_t> &Out, std::size_t At, std::uint32_t Value) {
	Out[At + 0] = static_cast<std::uint8_t>(Value & 0xFF);
	Out[At + 1] = static_cast<std::uint8_t>((Value >> 8) & 0xFF);
	Out[At + 2] = static_cast<std::uint8_t>((Value >> 16) & 0xFF);
	Out[At + 3] = static_cast<std::uint8_t>((Value >> 24) & 0xFF);
}

std::uint8_t MatchJccForOp(FilterCompareOp Op) {
	switch(Op) {
	case FilterCompareOp::Eq:
		return 0x74;
	case FilterCompareOp::Ne:
		return 0x75;
	case FilterCompareOp::Gt:
		return 0x7F;
	case FilterCompareOp::Ge:
		return 0x7D;
	case FilterCompareOp::Lt:
		return 0x7C;
	case FilterCompareOp::Le:
		return 0x7E;
	}
	return 0x74;
}

#if defined(_WIN32)

/** Win64: rcx=Values, rdx=Count, r8=Literal, r9=OutIndices; returns rax=match count. */
bool EmitFilterDense(std::vector<std::uint8_t> &Out, FilterCompareOp Op, int64_t /*Literal*/) {
	Out.clear();
	Emit(Out, {0x4D, 0x31, 0xDB}); // xor r11, r11
	Emit(Out, {0x48, 0x31, 0xC0}); // xor rax, rax
	Emit(Out, {0x48, 0x85, 0xD2}); // test rdx, rdx
	const std::size_t JzDone = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	const std::size_t Loop = Out.size();
	Emit(Out, {0x4C, 0x8B, 0x11}); // mov r10, [rcx]
	Emit(Out, {0x4D, 0x3B, 0xD0}); // cmp r10, r8
	const std::size_t JccMatch = Out.size();
	Emit(Out, {MatchJccForOp(Op), 0x00});
	Emit(Out, {0xEB, 0x00});
	const std::size_t Match = Out.size();
	Emit(Out, {0x4D, 0x89, 0x1C, 0xC1}); // mov [r9 + rax*8], r11
	Emit(Out, {0x48, 0xFF, 0xC0});       // inc rax
	const std::size_t Skip = Out.size();
	Out[JccMatch + 1] = static_cast<std::uint8_t>(Match - (JccMatch + 2));
	Out[Match - 1] = static_cast<std::uint8_t>(Skip - Match);
	Emit(Out, {0x49, 0xFF, 0xC3});       // inc r11
	Emit(Out, {0x48, 0x83, 0xC1, 0x08}); // add rcx, 8
	Emit(Out, {0x48, 0xFF, 0xCA});       // dec rdx
	Emit(Out, {0x75});
	Out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(Loop - (Out.size() + 1))));
	Emit(Out, {0xC3});
	const std::size_t Done = Out.size();
	Patch32(Out, JzDone + 2, static_cast<std::uint32_t>(Done - (JzDone + 6)));
	return true;
}

/** Win64: rcx=Values, rdx=Count; returns rax=sum. */
bool EmitSum(std::vector<std::uint8_t> &Out) {
	Out.clear();
	Emit(Out, {0x48, 0x31, 0xC0}); // xor rax, rax
	Emit(Out, {0x48, 0x85, 0xD2}); // test rdx, rdx
	const std::size_t JzDone = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	const std::size_t Loop = Out.size();
	Emit(Out, {0x48, 0x03, 0x01});       // add rax, [rcx]
	Emit(Out, {0x48, 0x83, 0xC1, 0x08}); // add rcx, 8
	Emit(Out, {0x48, 0xFF, 0xCA});       // dec rdx
	Emit(Out, {0x75});
	Out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(Loop - (Out.size() + 1))));
	Emit(Out, {0xC3});
	Patch32(Out, JzDone + 2, static_cast<std::uint32_t>(Out.size() - (JzDone + 6)));
	return true;
}

/** Win64: rcx=Values, rdx=Count; returns rax=min. */
bool EmitMin(std::vector<std::uint8_t> &Out) {
	Out.clear();
	Emit(Out, {0x48, 0x85, 0xD2}); // test rdx, rdx
	const std::size_t JzZero = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	Emit(Out, {0x48, 0x8B, 0x01}); // mov rax, [rcx]
	Emit(Out, {0x48, 0xFF, 0xCA}); // dec rdx
	const std::size_t JzDone = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	const std::size_t Loop = Out.size();
	Emit(Out, {0x48, 0x83, 0xC1, 0x08}); // add rcx, 8
	Emit(Out, {0x4C, 0x8B, 0x11});       // mov r10, [rcx]
	Emit(Out, {0x4C, 0x39, 0xD0});       // cmp rax, r10
	Emit(Out, {0x49, 0x0F, 0x4F, 0xC2}); // cmovg rax, r10 (rax > r10)
	Emit(Out, {0x48, 0xFF, 0xCA});       // dec rdx
	Emit(Out, {0x75});
	Out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(Loop - (Out.size() + 1))));
	Emit(Out, {0xC3});
	const std::size_t Done = Out.size();
	Patch32(Out, JzDone + 2, static_cast<std::uint32_t>(Done - (JzDone + 6)));
	Patch32(Out, JzZero + 2, static_cast<std::uint32_t>(Done - (JzZero + 6)));
	return true;
}

/** Win64: rcx=Values, rdx=Count; returns rax=max. */
bool EmitMax(std::vector<std::uint8_t> &Out) {
	Out.clear();
	Emit(Out, {0x48, 0x85, 0xD2}); // test rdx, rdx
	const std::size_t JzZero = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	Emit(Out, {0x48, 0x8B, 0x01}); // mov rax, [rcx]
	Emit(Out, {0x48, 0xFF, 0xCA}); // dec rdx
	const std::size_t JzDone = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	const std::size_t Loop = Out.size();
	Emit(Out, {0x48, 0x83, 0xC1, 0x08}); // add rcx, 8
	Emit(Out, {0x4C, 0x8B, 0x11});       // mov r10, [rcx]
	Emit(Out, {0x4C, 0x39, 0xD0});       // cmp rax, r10
	Emit(Out, {0x49, 0x0F, 0x4C, 0xC2}); // cmovl rax, r10 (rax < r10)
	Emit(Out, {0x48, 0xFF, 0xCA});       // dec rdx
	Emit(Out, {0x75});
	Out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(Loop - (Out.size() + 1))));
	Emit(Out, {0xC3});
	const std::size_t Done = Out.size();
	Patch32(Out, JzDone + 2, static_cast<std::uint32_t>(Done - (JzDone + 6)));
	Patch32(Out, JzZero + 2, static_cast<std::uint32_t>(Done - (JzZero + 6)));
	return true;
}

#else

/** SysV AMD64: rdi=Values, rsi=Count, rdx=Literal, rcx=OutIndices; returns rax=match count. */
bool EmitFilterDense(std::vector<std::uint8_t> &Out, FilterCompareOp Op, int64_t /*Literal*/) {
	Out.clear();
	Emit(Out, {0x4D, 0x31, 0xDB}); // xor r11, r11
	Emit(Out, {0x48, 0x31, 0xC0}); // xor rax, rax
	Emit(Out, {0x48, 0x85, 0xF6}); // test rsi, rsi
	const std::size_t JzDone = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	const std::size_t Loop = Out.size();
	Emit(Out, {0x4C, 0x8B, 0x17});       // mov r10, [rdi]
	Emit(Out, {0x4C, 0x3B, 0xD2});       // cmp r10, rdx
	const std::size_t JccMatch = Out.size();
	Emit(Out, {MatchJccForOp(Op), 0x00});
	Emit(Out, {0xEB, 0x00});
	const std::size_t Match = Out.size();
	Emit(Out, {0x4C, 0x89, 0x1C, 0xC1}); // mov [rcx + rax*8], r11
	Emit(Out, {0x48, 0xFF, 0xC0});       // inc rax
	const std::size_t Skip = Out.size();
	Out[JccMatch + 1] = static_cast<std::uint8_t>(Match - (JccMatch + 2));
	Out[Match - 1] = static_cast<std::uint8_t>(Skip - Match);
	Emit(Out, {0x49, 0xFF, 0xC3});       // inc r11
	Emit(Out, {0x48, 0x83, 0xC7, 0x08}); // add rdi, 8
	Emit(Out, {0x48, 0xFF, 0xCE});       // dec rsi
	Emit(Out, {0x75});
	Out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(Loop - (Out.size() + 1))));
	Emit(Out, {0xC3});
	const std::size_t Done = Out.size();
	Patch32(Out, JzDone + 2, static_cast<std::uint32_t>(Done - (JzDone + 6)));
	return true;
}

/** SysV AMD64: rdi=Values, rsi=Count; returns rax=sum. */
bool EmitSum(std::vector<std::uint8_t> &Out) {
	Out.clear();
	Emit(Out, {0x48, 0x31, 0xC0}); // xor rax, rax
	Emit(Out, {0x48, 0x85, 0xF6}); // test rsi, rsi
	const std::size_t JzDone = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	const std::size_t Loop = Out.size();
	Emit(Out, {0x48, 0x03, 0x07});       // add rax, [rdi]
	Emit(Out, {0x48, 0x83, 0xC7, 0x08}); // add rdi, 8
	Emit(Out, {0x48, 0xFF, 0xCE});       // dec rsi
	Emit(Out, {0x75});
	Out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(Loop - (Out.size() + 1))));
	Emit(Out, {0xC3});
	Patch32(Out, JzDone + 2, static_cast<std::uint32_t>(Out.size() - (JzDone + 6)));
	return true;
}

/** SysV AMD64: rdi=Values, rsi=Count; returns rax=min. */
bool EmitMin(std::vector<std::uint8_t> &Out) {
	Out.clear();
	Emit(Out, {0x48, 0x85, 0xF6}); // test rsi, rsi
	const std::size_t JzZero = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	Emit(Out, {0x48, 0x8B, 0x07}); // mov rax, [rdi]
	Emit(Out, {0x48, 0xFF, 0xCE}); // dec rsi
	const std::size_t JzDone = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	const std::size_t Loop = Out.size();
	Emit(Out, {0x48, 0x83, 0xC7, 0x08}); // add rdi, 8
	Emit(Out, {0x4C, 0x8B, 0x17});       // mov r10, [rdi]
	Emit(Out, {0x4C, 0x39, 0xD0});       // cmp rax, r10
	Emit(Out, {0x49, 0x0F, 0x4F, 0xC2}); // cmovg rax, r10 (rax > r10)
	Emit(Out, {0x48, 0xFF, 0xCE});       // dec rsi
	Emit(Out, {0x75});
	Out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(Loop - (Out.size() + 1))));
	Emit(Out, {0xC3});
	const std::size_t Done = Out.size();
	Patch32(Out, JzDone + 2, static_cast<std::uint32_t>(Done - (JzDone + 6)));
	Patch32(Out, JzZero + 2, static_cast<std::uint32_t>(Done - (JzZero + 6)));
	return true;
}

/** SysV AMD64: rdi=Values, rsi=Count; returns rax=max. */
bool EmitMax(std::vector<std::uint8_t> &Out) {
	Out.clear();
	Emit(Out, {0x48, 0x85, 0xF6}); // test rsi, rsi
	const std::size_t JzZero = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	Emit(Out, {0x48, 0x8B, 0x07}); // mov rax, [rdi]
	Emit(Out, {0x48, 0xFF, 0xCE}); // dec rsi
	const std::size_t JzDone = Out.size();
	Emit(Out, {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
	const std::size_t Loop = Out.size();
	Emit(Out, {0x48, 0x83, 0xC7, 0x08}); // add rdi, 8
	Emit(Out, {0x4C, 0x8B, 0x17});       // mov r10, [rdi]
	Emit(Out, {0x4C, 0x39, 0xD0});       // cmp rax, r10
	Emit(Out, {0x49, 0x0F, 0x4C, 0xC2}); // cmovl rax, r10 (rax < r10)
	Emit(Out, {0x48, 0xFF, 0xCE});       // dec rsi
	Emit(Out, {0x75});
	Out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(Loop - (Out.size() + 1))));
	Emit(Out, {0xC3});
	const std::size_t Done = Out.size();
	Patch32(Out, JzDone + 2, static_cast<std::uint32_t>(Done - (JzDone + 6)));
	Patch32(Out, JzZero + 2, static_cast<std::uint32_t>(Done - (JzZero + 6)));
	return true;
}

#endif

} // namespace

bool EmitX86_64FilterDenseKernel(FilterCompareOp Op, int64_t Literal, std::vector<std::uint8_t> &Out) {
	return EmitFilterDense(Out, Op, Literal);
}

bool EmitX86_64SumKernel(std::vector<std::uint8_t> &Out) { return EmitSum(Out); }

bool EmitX86_64MinKernel(std::vector<std::uint8_t> &Out) { return EmitMin(Out); }

bool EmitX86_64MaxKernel(std::vector<std::uint8_t> &Out) { return EmitMax(Out); }

} // namespace SQL
} // namespace AstralDB
