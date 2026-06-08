#include <SQL/JIT/JitCompiler.hxx>

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)

namespace AstralDB {
namespace SQL {

namespace {

void EmitU32(std::vector<std::uint8_t> &Out, std::uint32_t Insn) {
	Out.push_back(static_cast<std::uint8_t>(Insn & 0xFF));
	Out.push_back(static_cast<std::uint8_t>((Insn >> 8) & 0xFF));
	Out.push_back(static_cast<std::uint8_t>((Insn >> 16) & 0xFF));
	Out.push_back(static_cast<std::uint8_t>((Insn >> 24) & 0xFF));
}

/** Indirect-call landing pad (Apple silicon / BTI). */
void EmitBtiC(std::vector<std::uint8_t> &Out) { EmitU32(Out, 0xD503245Fu); }

void PatchU32(std::vector<std::uint8_t> &Out, std::size_t At, std::uint32_t Insn) {
	Out[At + 0] = static_cast<std::uint8_t>(Insn & 0xFF);
	Out[At + 1] = static_cast<std::uint8_t>((Insn >> 8) & 0xFF);
	Out[At + 2] = static_cast<std::uint8_t>((Insn >> 16) & 0xFF);
	Out[At + 3] = static_cast<std::uint8_t>((Insn >> 24) & 0xFF);
}

bool PatchBranch19(std::vector<std::uint8_t> &Out, std::size_t At, std::int32_t ByteOffset) {
	const std::int32_t Imm19 = ByteOffset >> 2;
	if(Imm19 < -(1 << 18) || Imm19 >= (1 << 18))
		return false;
	std::uint32_t Insn = static_cast<std::uint8_t>(Out[At + 0]);
	Insn |= static_cast<std::uint32_t>(Out[At + 1]) << 8;
	Insn |= static_cast<std::uint32_t>(Out[At + 2]) << 16;
	Insn |= static_cast<std::uint32_t>(Out[At + 3]) << 24;
	Insn = (Insn & 0xFF00001Fu) | ((static_cast<std::uint32_t>(Imm19) & 0x7FFFFu) << 5);
	PatchU32(Out, At, Insn);
	return true;
}

bool PatchCbz19(std::vector<std::uint8_t> &Out, std::size_t At, std::int32_t ByteOffset, unsigned Rt) {
	const std::int32_t Imm19 = ByteOffset >> 2;
	if(Imm19 < -(1 << 18) || Imm19 >= (1 << 18))
		return false;
	const std::uint32_t Insn = 0xB4000000u | ((static_cast<std::uint32_t>(Imm19) & 0x7FFFFu) << 5) | (Rt & 0x1Fu);
	PatchU32(Out, At, Insn);
	return true;
}

std::uint32_t SkipCondForOp(FilterCompareOp Op) {
	switch(Op) {
	case FilterCompareOp::Eq:
		return 0x1u; // NE
	case FilterCompareOp::Ne:
		return 0x0u; // EQ
	case FilterCompareOp::Gt:
		return 0xDu; // LE
	case FilterCompareOp::Ge:
		return 0xAu; // LT
	case FilterCompareOp::Lt:
		return 0xCu; // GE
	case FilterCompareOp::Le:
		return 0xBu; // GT
	}
	return 0x1u;
}

/** x0=Values, x1=Count, x2=Literal, x3=OutIndices; returns x0=match count. */
bool EmitFilterDense(std::vector<std::uint8_t> &Out, FilterCompareOp Op) {
	Out.clear();
	EmitBtiC(Out);
	EmitU32(Out, 0xAA1F03E4u); // mov x4, xzr (match count)
	EmitU32(Out, 0xAA1F03E5u); // mov x5, xzr (row index)
	const std::size_t CbzAt = Out.size();
	EmitU32(Out, 0xB4000001u); // cbz x1, end (patched)
	const std::size_t Loop = Out.size();
	EmitU32(Out, 0xF9400006u); // ldr x6, [x0]
	EmitU32(Out, 0xEB0200DFu); // cmp x6, x2
	const std::size_t BcondAt = Out.size();
	EmitU32(Out, 0x54000000u | SkipCondForOp(Op)); // b.<skip_cond> skip
	EmitU32(Out, 0xF8247865u); // str x5, [x3, x4, lsl #3]
	EmitU32(Out, 0x91000484u); // add x4, x4, #1
	const std::size_t Skip = Out.size();
	EmitU32(Out, 0x91002000u); // add x0, x0, #8
	EmitU32(Out, 0x910004A5u); // add x5, x5, #1
	EmitU32(Out, 0xD1000421u); // sub x1, x1, #1
	const std::size_t BneAt = Out.size();
	EmitU32(Out, 0x54000001u); // b.ne loop (patched)
	const std::size_t End = Out.size();
	EmitU32(Out, 0xAA0403E0u); // mov x0, x4
	EmitU32(Out, 0xD65F03C0u); // ret
	return PatchCbz19(Out, CbzAt, static_cast<std::int32_t>(End - CbzAt), 1) &&
	       PatchBranch19(Out, BcondAt, static_cast<std::int32_t>(Skip - BcondAt)) &&
	       PatchBranch19(Out, BneAt, static_cast<std::int32_t>(Loop - BneAt));
}

/** x0=Values, x1=Count; returns x0=sum. */
bool EmitSum(std::vector<std::uint8_t> &Out) {
	Out.clear();
	EmitBtiC(Out);
	EmitU32(Out, 0xAA1F03E2u); // mov x2, xzr
	const std::size_t CbzAt = Out.size();
	EmitU32(Out, 0xB4000001u); // cbz x1, done (patched)
	const std::size_t Loop = Out.size();
	EmitU32(Out, 0xF9400003u); // ldr x3, [x0]
	EmitU32(Out, 0x8B030042u); // add x2, x2, x3
	EmitU32(Out, 0x91002000u); // add x0, x0, #8
	EmitU32(Out, 0xD1000421u); // sub x1, x1, #1
	const std::size_t BneAt = Out.size();
	EmitU32(Out, 0x54000001u); // b.ne loop (patched)
	const std::size_t Done = Out.size();
	EmitU32(Out, 0xAA0203E0u); // mov x0, x2
	EmitU32(Out, 0xD65F03C0u); // ret
	return PatchCbz19(Out, CbzAt, static_cast<std::int32_t>(Done - CbzAt), 1) &&
	       PatchBranch19(Out, BneAt, static_cast<std::int32_t>(Loop - BneAt));
}

/** x0=Values, x1=Count; returns x0=min. */
bool EmitMin(std::vector<std::uint8_t> &Out) {
	Out.clear();
	EmitBtiC(Out);
	EmitU32(Out, 0xF9400002u); // ldr x2, [x0]
	EmitU32(Out, 0xD1000421u); // sub x1, x1, #1
	const std::size_t CbzAt = Out.size();
	EmitU32(Out, 0xB4000001u); // cbz x1, done (patched)
	const std::size_t Loop = Out.size();
	EmitU32(Out, 0x91002000u); // add x0, x0, #8
	EmitU32(Out, 0xF9400003u); // ldr x3, [x0]
	EmitU32(Out, 0xEB03005Fu); // cmp x2, x3
	const std::size_t BleAt = Out.size();
	EmitU32(Out, 0x5400000Du); // b.le skip
	EmitU32(Out, 0xAA0303E2u); // mov x2, x3
	const std::size_t Skip = Out.size();
	EmitU32(Out, 0xD1000421u); // sub x1, x1, #1
	const std::size_t BneAt = Out.size();
	EmitU32(Out, 0x54000001u); // b.ne loop (patched)
	const std::size_t Done = Out.size();
	EmitU32(Out, 0xAA0203E0u); // mov x0, x2
	EmitU32(Out, 0xD65F03C0u); // ret
	return PatchCbz19(Out, CbzAt, static_cast<std::int32_t>(Done - CbzAt), 1) &&
	       PatchBranch19(Out, BleAt, static_cast<std::int32_t>(Skip - BleAt)) &&
	       PatchBranch19(Out, BneAt, static_cast<std::int32_t>(Loop - BneAt));
}

/** x0=Values, x1=Count; returns x0=max. */
bool EmitMax(std::vector<std::uint8_t> &Out) {
	Out.clear();
	EmitBtiC(Out);
	EmitU32(Out, 0xF9400002u); // ldr x2, [x0]
	EmitU32(Out, 0xD1000421u); // sub x1, x1, #1
	const std::size_t CbzAt = Out.size();
	EmitU32(Out, 0xB4000001u); // cbz x1, done (patched)
	const std::size_t Loop = Out.size();
	EmitU32(Out, 0x91002000u); // add x0, x0, #8
	EmitU32(Out, 0xF9400003u); // ldr x3, [x0]
	EmitU32(Out, 0xEB03005Fu); // cmp x2, x3
	const std::size_t BgeAt = Out.size();
	EmitU32(Out, 0x5400000Au); // b.ge skip
	EmitU32(Out, 0xAA0303E2u); // mov x2, x3
	const std::size_t Skip = Out.size();
	EmitU32(Out, 0xD1000421u); // sub x1, x1, #1
	const std::size_t BneAt = Out.size();
	EmitU32(Out, 0x54000001u); // b.ne loop (patched)
	const std::size_t Done = Out.size();
	EmitU32(Out, 0xAA0203E0u); // mov x0, x2
	EmitU32(Out, 0xD65F03C0u); // ret
	return PatchCbz19(Out, CbzAt, static_cast<std::int32_t>(Done - CbzAt), 1) &&
	       PatchBranch19(Out, BgeAt, static_cast<std::int32_t>(Skip - BgeAt)) &&
	       PatchBranch19(Out, BneAt, static_cast<std::int32_t>(Loop - BneAt));
}

} // namespace

bool EmitArm64FilterDenseKernel(FilterCompareOp Op, int64_t Literal, std::vector<std::uint8_t> &Out) {
	(void)Literal;
	return EmitFilterDense(Out, Op);
}

bool EmitArm64SumKernel(std::vector<std::uint8_t> &Out) { return EmitSum(Out); }

bool EmitArm64MinKernel(std::vector<std::uint8_t> &Out) { return EmitMin(Out); }

bool EmitArm64MaxKernel(std::vector<std::uint8_t> &Out) { return EmitMax(Out); }

} // namespace SQL
} // namespace AstralDB

#endif
