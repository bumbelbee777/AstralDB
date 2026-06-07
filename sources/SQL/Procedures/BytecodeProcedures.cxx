#include <SQL/BytecodeProcedures.hxx>
#include <SQL/BytecodeInspect.hxx>
#include <SQL/SQL.hxx>
#include <DS/JSON.hxx>
#include <Database/Database.hxx>
#include <IO/Error.hxx>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace AstralDB {
namespace SQL {

static constexpr int kCatalogVersion = 1;

std::string EncodeExceptionHandlersJson(const std::vector<ProcedureExceptionWhen> &Handlers) {
	if(Handlers.empty())
		return {};
	DS::JSONArray Arr;
	for(const auto &H : Handlers) {
		DS::JSONObject O;
		O.emplace("when", DS::JSON(H.Condition));
		O.emplace("sql", DS::JSON(H.HandlerSql));
		Arr.push_back(DS::JSON(std::move(O)));
	}
	return DS::SerializeJSON(DS::JSON(std::move(Arr)));
}

std::vector<ProcedureExceptionWhen> DecodeExceptionHandlersJson(std::string_view Json) {
	std::vector<ProcedureExceptionWhen> Out;
	if(Json.empty())
		return Out;
	const DS::JSON Root = DS::DecodeJSONStrict(std::string(Json));
	if(!Root.IsArray())
		return Out;
	for(const auto &Item : Root.AsArray()) {
		if(!Item.IsObject())
			continue;
		const auto &O = Item.AsObject();
		ProcedureExceptionWhen H;
		if(const auto It = O.find("when"); It != O.end() && It->second.IsString())
			H.Condition = It->second.AsString();
		if(const auto It = O.find("sql"); It != O.end() && It->second.IsString())
			H.HandlerSql = It->second.AsString();
		if(!H.Condition.empty() && !H.HandlerSql.empty())
			Out.push_back(std::move(H));
	}
	return Out;
}

namespace {

std::filesystem::path ResolveBesideCatalog(const std::filesystem::path &CatalogPath,
                                           const std::filesystem::path &Stored) {
	if(Stored.is_absolute())
		return Stored;
	return CatalogPath.parent_path() / Stored;
}

std::string NormalizeAbcKey(const std::filesystem::path &P) {
	std::error_code Ec;
	return std::filesystem::weakly_canonical(P, Ec).string();
}

void LoadJsonStringArray(const DS::JSONObject &O, const char *Key, std::vector<std::string> &Out) {
	const auto It = O.find(Key);
	if(It == O.end() || !It->second.IsArray())
		return;
	for(const auto &J : It->second.AsArray())
		if(J.IsString())
			Out.push_back(J.AsString());
}

void SaveJsonStringArray(DS::JSONObject &O, const char *Key, const std::vector<std::string> &Vals) {
	if(Vals.empty())
		return;
	DS::JSONArray Arr;
	for(const auto &S : Vals)
		Arr.push_back(DS::JSON(S));
	O.emplace(Key, DS::JSON(std::move(Arr)));
}

} // namespace

void ScanProcedureInvokeKeyword(std::string_view BodySql, std::string_view Upper, std::string_view Keyword,
                               std::vector<std::string> &Out) {
	const std::size_t KwLen = Keyword.size();
	for(std::size_t I = 0; I + KwLen < Upper.size(); ++I) {
		if(Upper.substr(I, KwLen) != Keyword)
			continue;
		if(I > 0 && std::isalnum(static_cast<unsigned char>(Upper[I - 1])))
			continue;
		std::size_t J = I + KwLen;
		while(J < Upper.size() && std::isspace(static_cast<unsigned char>(Upper[J])))
			++J;
		if(J + 9 <= Upper.size() && Upper.substr(J, 9) == "PROCEDURE") {
			I = J + 8;
			continue;
		}
		const std::size_t K0 = J;
		while(J < Upper.size() && (std::isalnum(static_cast<unsigned char>(Upper[J])) || Upper[J] == '_'))
			++J;
		if(J > K0) {
			const std::string Name(BodySql.substr(K0, J - K0));
			if(std::find(Out.begin(), Out.end(), Name) == Out.end())
				Out.push_back(Name);
		}
	}
}

std::vector<std::string> ScanProcedureCallsInSql(std::string_view BodySql) {
	std::vector<std::string> Out;
	std::string Upper(BodySql);
	std::transform(Upper.begin(), Upper.end(), Upper.begin(),
	               [](unsigned char C) { return static_cast<char>(std::toupper(C)); });
	for(std::size_t I = 0; I + 17 < Upper.size(); ++I) {
		if(Upper.substr(I, 17) != "EXECUTE PROCEDURE")
			continue;
		if(I > 0 && std::isalnum(static_cast<unsigned char>(Upper[I - 1])))
			continue;
		std::size_t J = I + 17;
		while(J < Upper.size() && std::isspace(static_cast<unsigned char>(Upper[J])))
			++J;
		std::size_t K0 = J;
		while(J < Upper.size() && (std::isalnum(static_cast<unsigned char>(Upper[J])) || Upper[J] == '_'))
			++J;
		if(J > K0) {
			const std::string Name(BodySql.substr(K0, J - K0));
			if(std::find(Out.begin(), Out.end(), Name) == Out.end())
				Out.push_back(Name);
		}
	}
	ScanProcedureInvokeKeyword(BodySql, Upper, "CALL", Out);
	ScanProcedureInvokeKeyword(BodySql, Upper, "EXECUTE", Out);
	ScanProcedureInvokeKeyword(BodySql, Upper, "EXEC", Out);
	return Out;
}

namespace {

void AppendBytecodeSansHalt(Bytecode &Dest, const Bytecode &Src) {
	for(const Instruction &Inst : Src) {
		if(Inst.Opcode_ == Opcode::HALT)
			continue;
		Dest.push_back(Inst);
	}
}

void AppendCompiledBytecode(Bytecode &Dest, std::vector<std::string> &DestPool, const CompiledBytecode &Chunk) {
	const std::size_t PoolBase = DestPool.size();
	DestPool.insert(DestPool.end(), Chunk.StringPool.begin(), Chunk.StringPool.end());
	for(const Instruction &Inst : Chunk.Instructions) {
		if(Inst.Opcode_ == Opcode::HALT)
			continue;
		Instruction Copy = Inst;
		if(Copy.Opcode_ == Opcode::PUSH_POOL && !Copy.Operands.empty()) {
			if(auto *Idx = std::get_if<int64_t>(&Copy.Operands[0]))
				*Idx += static_cast<int64_t>(PoolBase);
		}
		Dest.push_back(std::move(Copy));
	}
}

CompiledBytecode CompileSqlChunk(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                std::string_view Sql) {
	Parser P(Sql);
	(void)P;
	return BuildCompiledBytecode(Logger, OptLevel, CatalogDb);
}

std::string FindProbeTableName(const Bytecode &Bc) {
	std::string Last;
	for(const Instruction &Inst : Bc) {
		if(Inst.Opcode_ == Opcode::CLONE_TABLE && !Inst.Operands.empty())
			if(const auto *Dest = std::get_if<std::string>(&Inst.Operands[0]))
				Last = *Dest;
	}
	return Last;
}

std::size_t CountSansHalt(const Bytecode &Bc) {
	std::size_t N = 0;
	for(const Instruction &Inst : Bc)
		if(Inst.Opcode_ != Opcode::HALT)
			++N;
	return N;
}

CompiledBytecode CompileLoweredSegments(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                        const std::vector<ProcedureControlSegment> &Segments, std::size_t &ProbeSeq);

CompiledBytecode CompileIfBranchChain(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                      const std::vector<ProcedureIfBranch> &Branches, std::size_t &ProbeSeq,
                                      std::size_t CodeBaseIp) {
	if(Branches.empty())
		return {};
	std::vector<CompiledBytecode> CondCompiled;
	std::vector<CompiledBytecode> BodyCompiled;
	std::vector<std::string> ProbeTables;
	CondCompiled.reserve(Branches.size());
	BodyCompiled.reserve(Branches.size());
	ProbeTables.reserve(Branches.size());
	for(const ProcedureIfBranch &Br : Branches) {
		if(Br.ConditionSql.empty()) {
			CondCompiled.push_back({});
			ProbeTables.emplace_back();
		} else {
			std::string Fold = Br.ConditionSql;
			for(char &Ch : Fold)
				Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(Ch)));
			std::string Sql;
			if(Fold == "TRUE" || Fold == "1" || Fold == "1=1")
				Sql = "SELECT 1 AS __proc_if_hit FROM DUAL;";
			else {
				Sql = "SELECT 1 AS __proc_if_hit FROM DUAL WHERE (";
				Sql += Br.ConditionSql;
				Sql += ");";
			}
			CondCompiled.push_back(CompileSqlChunk(Logger, OptLevel, CatalogDb, Sql));
			ProbeTables.push_back(FindProbeTableName(CondCompiled.back().Instructions));
		}
		BodyCompiled.push_back(CompileLoweredSegments(Logger, OptLevel, CatalogDb, Br.Segments, ProbeSeq));
	}
	std::vector<std::size_t> CondSizes;
	std::vector<std::size_t> BodySizes;
	CondSizes.reserve(Branches.size());
	BodySizes.reserve(Branches.size());
	for(std::size_t I = 0; I < Branches.size(); ++I) {
		CondSizes.push_back(Branches[I].ConditionSql.empty() ? 0 : CountSansHalt(CondCompiled[I].Instructions));
		BodySizes.push_back(CountSansHalt(BodyCompiled[I].Instructions));
	}
	auto BlockSize = [&](std::size_t I) -> std::size_t {
		std::size_t S = BodySizes[I];
		if(!Branches[I].ConditionSql.empty()) {
			S += CondSizes[I];
			if(!ProbeTables[I].empty())
				++S;
		}
		if(I + 1 < Branches.size())
			++S;
		return S;
	};
	std::vector<std::size_t> BlockStart(Branches.size());
	std::size_t Cursor = 0;
	for(std::size_t I = 0; I < Branches.size(); ++I) {
		BlockStart[I] = Cursor;
		Cursor += BlockSize(I);
	}
	const std::size_t EndIp = CodeBaseIp + Cursor;
	std::vector<std::size_t> FalseJumpIps(Branches.size(), EndIp);
	for(std::size_t I = 0; I < Branches.size(); ++I) {
		if(Branches[I].ConditionSql.empty())
			continue;
		for(std::size_t J = I + 1; J < Branches.size(); ++J) {
			FalseJumpIps[I] = CodeBaseIp + BlockStart[J];
			if(!Branches[J].ConditionSql.empty() || J + 1 == Branches.size())
				break;
		}
	}
	Bytecode Out;
	std::vector<std::string> Pool;
	for(std::size_t I = 0; I < Branches.size(); ++I) {
		if(!Branches[I].ConditionSql.empty()) {
			AppendCompiledBytecode(Out, Pool, CondCompiled[I]);
			if(!ProbeTables[I].empty()) {
				Instruction Jump;
				Jump.Opcode_ = Opcode::PROC_JUMP_IF_TABLE_EMPTY;
				Jump.Operands.push_back(ProbeTables[I]);
				Jump.Operands.push_back(static_cast<int64_t>(FalseJumpIps[I]));
				Out.push_back(Jump);
			}
		}
		AppendCompiledBytecode(Out, Pool, BodyCompiled[I]);
		if(I + 1 < Branches.size()) {
			Instruction Jmp;
			Jmp.Opcode_ = Opcode::JMP;
			Jmp.Operands.push_back(static_cast<int64_t>(EndIp));
			Out.push_back(Jmp);
		}
	}
	CompiledBytecode Result;
	Result.Instructions = std::move(Out);
	Result.StringPool = std::move(Pool);
	return Result;
}

CompiledBytecode CompileLoweredSegments(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                        const std::vector<ProcedureControlSegment> &Segments, std::size_t &ProbeSeq) {
	(void)ProbeSeq;
	Bytecode Out;
	std::vector<std::string> Pool;
	for(const ProcedureControlSegment &Seg : Segments) {
		if(!Seg.LinearSql.empty())
			AppendCompiledBytecode(Out, Pool, CompileSqlChunk(Logger, OptLevel, CatalogDb, Seg.LinearSql));
		if(!Seg.IfBranches.empty())
			AppendCompiledBytecode(Out, Pool,
			                       CompileIfBranchChain(Logger, OptLevel, CatalogDb, Seg.IfBranches, ProbeSeq, Out.size()));
	}
	CompiledBytecode Result;
	Result.Instructions = std::move(Out);
	Result.StringPool = std::move(Pool);
	return Result;
}

CompiledBytecode WrapProcedureExceptions(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                         const CompiledBytecode &Core,
                                         const std::vector<ProcedureExceptionWhen> &ExceptionHandlers) {
	std::vector<CompiledBytecode> HandlerCompiled;
	HandlerCompiled.reserve(ExceptionHandlers.size());
	for(const auto &H : ExceptionHandlers)
		HandlerCompiled.push_back(CompileSqlChunk(Logger, OptLevel, CatalogDb, H.HandlerSql));
	const std::size_t TryCount = CountSansHalt(Core.Instructions);
	std::vector<std::size_t> HandlerSizes;
	HandlerSizes.reserve(HandlerCompiled.size());
	for(const auto &Hc : HandlerCompiled)
		HandlerSizes.push_back(CountSansHalt(Hc.Instructions));
	const std::size_t Handler0Ip = 1 + TryCount + 1;
	std::size_t ExCursor = Handler0Ip;
	std::vector<std::size_t> HandlerIps;
	for(std::size_t Hi = 0; Hi < HandlerSizes.size(); ++Hi) {
		HandlerIps.push_back(ExCursor);
		ExCursor += HandlerSizes[Hi];
		if(Hi + 1 < HandlerSizes.size())
			++ExCursor;
	}
	const std::size_t EndIp = ExCursor + 1;
	const std::string Savepoint = "__astr_proc_ex";
	Bytecode Out;
	Instruction TryOp;
	TryOp.Opcode_ = Opcode::PROC_TRY;
	TryOp.Operands.push_back(Savepoint);
	TryOp.Operands.push_back(static_cast<int64_t>(EndIp));
	for(std::size_t Hi = 0; Hi < HandlerIps.size(); ++Hi) {
		TryOp.Operands.push_back(static_cast<int64_t>(HandlerIps[Hi]));
		TryOp.Operands.push_back(ExceptionHandlers[Hi].Condition);
	}
	Out.push_back(TryOp);
	std::vector<std::string> Pool = Core.StringPool;
	AppendBytecodeSansHalt(Out, Core.Instructions);
	Instruction EndTry;
	EndTry.Opcode_ = Opcode::PROC_END_TRY;
	EndTry.Operands.push_back(Savepoint);
	EndTry.Operands.push_back(static_cast<int64_t>(EndIp));
	Out.push_back(EndTry);
	for(std::size_t Hi = 0; Hi < HandlerCompiled.size(); ++Hi) {
		AppendCompiledBytecode(Out, Pool, HandlerCompiled[Hi]);
		if(Hi + 1 < HandlerCompiled.size()) {
			Instruction Jmp;
			Jmp.Opcode_ = Opcode::JMP;
			Jmp.Operands.push_back(static_cast<int64_t>(EndIp));
			Out.push_back(Jmp);
		}
	}
	Out.push_back(Instruction{Opcode::HALT, {}});
	CompiledBytecode Result;
	Result.Instructions = std::move(Out);
	Result.StringPool = std::move(Pool);
	return Result;
}

} // namespace

ProcedureBytecodeMeta AnalyzeProcedureBytecode(const Bytecode &Code) {
	ProcedureBytecodeMeta Meta;
	Meta.InstructionCount = Code.size();
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ == Opcode::PROC_TRY) {
			Meta.HasExceptionHandlers = true;
			for(std::size_t O = 3; O + 1 < Inst.Operands.size(); O += 2) {
				if(const auto *C = std::get_if<std::string>(&Inst.Operands[O + 1])) {
					Meta.ExceptionConditions.push_back(*C);
					++Meta.ExceptionHandlerCount;
				}
			}
		}
		if(Inst.Opcode_ == Opcode::CALL_PROCEDURE) {
			if(const auto *N = std::get_if<std::string>(&Inst.Operands[0]))
				if(std::find(Meta.CalledProcedures.begin(), Meta.CalledProcedures.end(), *N) ==
				   Meta.CalledProcedures.end())
					Meta.CalledProcedures.push_back(*N);
		}
	}
	return Meta;
}

CompiledBytecode CompileProcedureBody(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                      const LoweredProcedureBody &Body) {
	std::size_t ProbeSeq = 0;
	CompiledBytecode Core = CompileLoweredSegments(Logger, OptLevel, CatalogDb, Body.Segments, ProbeSeq);
	if(!Core.Instructions.empty() && Core.Instructions.back().Opcode_ != Opcode::HALT)
		Core.Instructions.push_back(Instruction{Opcode::HALT, {}});
	if(Body.ExceptionHandlers.empty())
		return Core;
	return WrapProcedureExceptions(Logger, OptLevel, CatalogDb, Core, Body.ExceptionHandlers);
}

CompiledBytecode CompileProcedureBody(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                      std::string_view BodySql,
                                      const std::vector<ProcedureExceptionWhen> &ExceptionHandlers) {
	LoweredProcedureBody Body = DecodeProcedureControlJson({}, BodySql);
	if(Body.Segments.empty()) {
		ProcedureControlSegment Seg;
		Seg.LinearSql = std::string(BodySql);
		Body.Segments.push_back(std::move(Seg));
	}
	Body.ExceptionHandlers = ExceptionHandlers;
	Body.TryBodySql = std::string(BodySql);
	return CompileProcedureBody(Logger, OptLevel, CatalogDb, Body);
}

std::string EncodeProcedureControlJson(const LoweredProcedureBody &Body) {
	std::function<DS::JSON(const ProcedureIfBranch &)> EncodeBranch;
	EncodeBranch = [&](const ProcedureIfBranch &Br) -> DS::JSON {
		DS::JSONObject O;
		O.emplace("when", DS::JSON(Br.ConditionSql));
		DS::JSONArray SegArr;
		for(const auto &Seg : Br.Segments) {
			DS::JSONObject S;
			S.emplace("linear", DS::JSON(Seg.LinearSql));
			DS::JSONArray BrArr;
			for(const auto &Arm : Seg.IfBranches)
				BrArr.push_back(EncodeBranch(Arm));
			if(!BrArr.empty())
				S.emplace("branches", DS::JSON(std::move(BrArr)));
			SegArr.push_back(DS::JSON(std::move(S)));
		}
		O.emplace("segments", DS::JSON(std::move(SegArr)));
		return DS::JSON(std::move(O));
	};
	if(Body.Segments.empty())
		return {};
	DS::JSONArray SegArr;
	for(const auto &Seg : Body.Segments) {
		DS::JSONObject S;
		S.emplace("linear", DS::JSON(Seg.LinearSql));
		DS::JSONArray BrArr;
		for(const auto &Arm : Seg.IfBranches)
			BrArr.push_back(EncodeBranch(Arm));
		if(!BrArr.empty())
			S.emplace("branches", DS::JSON(std::move(BrArr)));
		SegArr.push_back(DS::JSON(std::move(S)));
	}
	return DS::SerializeJSON(DS::JSON(std::move(SegArr)));
}

LoweredProcedureBody DecodeProcedureControlJson(std::string_view Json, std::string_view FallbackLinearSql) {
	LoweredProcedureBody Out;
	if(Json.empty()) {
		if(!FallbackLinearSql.empty()) {
			ProcedureControlSegment Seg;
			Seg.LinearSql = std::string(FallbackLinearSql);
			Out.Segments.push_back(std::move(Seg));
			Out.TryBodySql = std::string(FallbackLinearSql);
		}
		return Out;
	}
	const DS::JSON Root = DS::DecodeJSONStrict(std::string(Json));
	if(!Root.IsArray())
		return Out;
	std::function<ProcedureIfBranch(const DS::JSON &)> DecodeBranch = [&](const DS::JSON &J) {
		ProcedureIfBranch Br;
		if(!J.IsObject())
			return Br;
		const auto &O = J.AsObject();
		if(const auto It = O.find("when"); It != O.end() && It->second.IsString())
			Br.ConditionSql = It->second.AsString();
		if(const auto It = O.find("segments"); It != O.end() && It->second.IsArray()) {
			for(const auto &SegJ : It->second.AsArray()) {
				if(!SegJ.IsObject())
					continue;
				const auto &SO = SegJ.AsObject();
				ProcedureControlSegment Seg;
				if(const auto Lin = SO.find("linear"); Lin != SO.end() && Lin->second.IsString())
					Seg.LinearSql = Lin->second.AsString();
				if(const auto BrIt = SO.find("branches"); BrIt != SO.end() && BrIt->second.IsArray())
					for(const auto &ArmJ : BrIt->second.AsArray())
						Seg.IfBranches.push_back(DecodeBranch(ArmJ));
				Br.Segments.push_back(std::move(Seg));
			}
		}
		return Br;
	};
	for(const auto &SegJ : Root.AsArray()) {
		if(!SegJ.IsObject())
			continue;
		const auto &SO = SegJ.AsObject();
		ProcedureControlSegment Seg;
		if(const auto Lin = SO.find("linear"); Lin != SO.end() && Lin->second.IsString())
			Seg.LinearSql = Lin->second.AsString();
		if(const auto BrIt = SO.find("branches"); BrIt != SO.end() && BrIt->second.IsArray())
			for(const auto &ArmJ : BrIt->second.AsArray())
				Seg.IfBranches.push_back(DecodeBranch(ArmJ));
		Out.Segments.push_back(std::move(Seg));
	}
	if(!FallbackLinearSql.empty())
		Out.TryBodySql = std::string(FallbackLinearSql);
	return Out;
}

std::string HashProcedureSource(std::string_view BodySql) {
	return std::to_string(std::hash<std::string_view>{}(BodySql));
}

std::filesystem::path DefaultProcedureCatalogPath(const std::filesystem::path &SessionDbPath) {
	std::error_code Ec;
	const auto Db = std::filesystem::absolute(SessionDbPath, Ec);
	const auto Base = Ec || Db.empty() ? SessionDbPath : Db;
	const auto Parent = Base.parent_path();
	return (Parent.empty() ? std::filesystem::path(".") : Parent) / "astraldb_procs.json";
}

std::filesystem::path DefaultProcedureCacheDir(const std::filesystem::path &SessionDbPath) {
	std::error_code Ec;
	const auto Db = std::filesystem::absolute(SessionDbPath, Ec);
	const auto Base = Ec || Db.empty() ? SessionDbPath : Db;
	const auto Parent = Base.parent_path();
	return (Parent.empty() ? std::filesystem::path(".") : Parent) / "astraldb_procs_cache";
}

ProcedureCatalog LoadProcedureCatalog(const std::filesystem::path &CatalogPath) {
	ProcedureCatalog Catalog;
	Catalog.CatalogPath = CatalogPath;
	std::ifstream In(CatalogPath);
	if(!In)
		return Catalog;
	std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
	if(Text.empty())
		return Catalog;
	const DS::JSON Root = DS::DecodeJSONStrict(Text);
	if(!Root.IsObject())
		throw std::runtime_error(AstralDB::Err::Prefixed("PROC", "Procedure catalog root must be a JSON object."));
	const auto &Obj = Root.AsObject();
	if(const auto Ver = Obj.find("version"); Ver != Obj.end() && Ver->second.IsNumber()) {
		if(static_cast<int>(Ver->second.AsNumber()) != kCatalogVersion)
			throw std::runtime_error(AstralDB::Err::Prefixed("PROC", "Unsupported procedure catalog version."));
	}
	const auto ProcIt = Obj.find("procedures");
	if(ProcIt != Obj.end() && ProcIt->second.IsObject()) {
		for(const auto &[Name, Entry] : ProcIt->second.AsObject()) {
			if(!Entry.IsObject())
				continue;
			StoredProcedureEntry E;
			E.Name = Name;
			const auto &EO = Entry.AsObject();
			if(const auto PathIt = EO.find("abc_path"); PathIt != EO.end() && PathIt->second.IsString())
				E.AbcPath = ResolveBesideCatalog(CatalogPath, PathIt->second.AsString());
			else if(const auto Legacy = EO.find("path"); Legacy != EO.end() && Legacy->second.IsString())
				E.AbcPath = ResolveBesideCatalog(CatalogPath, Legacy->second.AsString());
			if(const auto SqlIt = EO.find("sql_path"); SqlIt != EO.end() && SqlIt->second.IsString())
				E.SqlPath = ResolveBesideCatalog(CatalogPath, SqlIt->second.AsString());
			if(const auto DescIt = EO.find("description"); DescIt != EO.end() && DescIt->second.IsString())
				E.Description = DescIt->second.AsString();
			if(const auto DialIt = EO.find("source_dialect"); DialIt != EO.end() && DialIt->second.IsString())
				E.SourceDialect = DialIt->second.AsString();
			if(const auto SrcIt = EO.find("source_sql"); SrcIt != EO.end() && SrcIt->second.IsString())
				E.SourceSql = SrcIt->second.AsString();
			if(const auto HashIt = EO.find("source_hash"); HashIt != EO.end() && HashIt->second.IsString())
				E.SourceHash = HashIt->second.AsString();
			LoadJsonStringArray(EO, "tables", E.ReferencedTables);
			LoadJsonStringArray(EO, "depends_on", E.DependsOn);
			LoadJsonStringArray(EO, "called_by", E.CalledBy);
			if(const auto BcIt = EO.find("bytecode_instructions"); BcIt != EO.end() && BcIt->second.IsNumber())
				E.BytecodeMeta.InstructionCount = static_cast<std::size_t>(BcIt->second.AsNumber());
			if(const auto ExIt = EO.find("exception_handlers"); ExIt != EO.end() && ExIt->second.IsNumber())
				E.BytecodeMeta.ExceptionHandlerCount = static_cast<std::size_t>(ExIt->second.AsNumber());
			if(const auto HexIt = EO.find("has_exception_handlers"); HexIt != EO.end() && HexIt->second.IsBool())
				E.BytecodeMeta.HasExceptionHandlers = HexIt->second.AsBool();
			LoadJsonStringArray(EO, "exception_conditions", E.BytecodeMeta.ExceptionConditions);
			LoadJsonStringArray(EO, "calls_procedures", E.BytecodeMeta.CalledProcedures);
			if(!E.AbcPath.empty())
				Catalog.Procedures.push_back(std::move(E));
		}
	}
	if(const auto IdxIt = Obj.find("abc_index"); IdxIt != Obj.end() && IdxIt->second.IsObject()) {
		for(const auto &[Path, Names] : IdxIt->second.AsObject()) {
			if(!Names.IsArray())
				continue;
			std::vector<std::string> List;
			for(const auto &N : Names.AsArray())
				if(N.IsString())
					List.push_back(N.AsString());
			Catalog.AbcIndex[Path] = std::move(List);
		}
	}
	if(Catalog.AbcIndex.empty())
		RebuildProcedureRelations(Catalog);
	return Catalog;
}

void RebuildProcedureRelations(ProcedureCatalog &Catalog) {
	for(auto &E : Catalog.Procedures)
		E.CalledBy.clear();
	Catalog.AbcIndex.clear();
	for(StoredProcedureEntry &E : Catalog.Procedures) {
		if(!E.AbcPath.empty())
			Catalog.AbcIndex[NormalizeAbcKey(E.AbcPath)].push_back(E.Name);
	}
	for(StoredProcedureEntry &E : Catalog.Procedures) {
		if(E.DependsOn.empty() && !E.SourceSql.empty())
			E.DependsOn = ScanProcedureCallsInSql(E.SourceSql);
		for(const auto &Dep : E.DependsOn) {
			for(StoredProcedureEntry &Other : Catalog.Procedures) {
				if(Other.Name == Dep) {
					if(std::find(Other.CalledBy.begin(), Other.CalledBy.end(), E.Name) == Other.CalledBy.end())
						Other.CalledBy.push_back(E.Name);
				}
			}
		}
	}
}

void SaveProcedureCatalog(const ProcedureCatalog &Catalog) {
	DS::JSONObject Root;
	Root.emplace("version", DS::JSON(kCatalogVersion));
	DS::JSONObject Procs;
	const auto RelPath = [&](const std::filesystem::path &P) {
		std::error_code Ec;
		const auto R = std::filesystem::relative(P, Catalog.CatalogPath.parent_path(), Ec);
		return Ec ? P.string() : R.string();
	};
	for(const StoredProcedureEntry &E : Catalog.Procedures) {
		DS::JSONObject Entry;
		Entry.emplace("abc_path", DS::JSON(RelPath(E.AbcPath)));
		if(!E.SqlPath.empty())
			Entry.emplace("sql_path", DS::JSON(RelPath(E.SqlPath)));
		if(!E.Description.empty())
			Entry.emplace("description", DS::JSON(E.Description));
		if(!E.SourceDialect.empty())
			Entry.emplace("source_dialect", DS::JSON(E.SourceDialect));
		if(!E.SourceSql.empty())
			Entry.emplace("source_sql", DS::JSON(E.SourceSql));
		if(!E.SourceHash.empty())
			Entry.emplace("source_hash", DS::JSON(E.SourceHash));
		SaveJsonStringArray(Entry, "tables", E.ReferencedTables);
		SaveJsonStringArray(Entry, "depends_on", E.DependsOn);
		SaveJsonStringArray(Entry, "called_by", E.CalledBy);
		if(E.BytecodeMeta.InstructionCount > 0)
			Entry.emplace("bytecode_instructions", DS::JSON(static_cast<double>(E.BytecodeMeta.InstructionCount)));
		if(E.BytecodeMeta.ExceptionHandlerCount > 0)
			Entry.emplace("exception_handlers", DS::JSON(static_cast<double>(E.BytecodeMeta.ExceptionHandlerCount)));
		if(E.BytecodeMeta.HasExceptionHandlers)
			Entry.emplace("has_exception_handlers", DS::JSON(true));
		SaveJsonStringArray(Entry, "exception_conditions", E.BytecodeMeta.ExceptionConditions);
		SaveJsonStringArray(Entry, "calls_procedures", E.BytecodeMeta.CalledProcedures);
		Procs.emplace(E.Name, DS::JSON(std::move(Entry)));
	}
	Root.emplace("procedures", DS::JSON(std::move(Procs)));
	DS::JSONObject Idx;
	for(const auto &[Path, Names] : Catalog.AbcIndex) {
		DS::JSONArray Arr;
		for(const auto &N : Names)
			Arr.push_back(DS::JSON(N));
		Idx.emplace(Path, DS::JSON(std::move(Arr)));
	}
	Root.emplace("abc_index", DS::JSON(std::move(Idx)));
	{
		std::error_code Ec;
		std::filesystem::create_directories(Catalog.CatalogPath.parent_path(), Ec);
	}
	std::ofstream Out(Catalog.CatalogPath);
	if(!Out)
		throw std::runtime_error(
		    AstralDB::Err::Prefixed("PROC", "Cannot write procedure catalog: " + Catalog.CatalogPath.string()));
	Out << DS::SerializeJSON(DS::JSON(Root));
}

void RegisterProcedure(ProcedureCatalog &Catalog, std::string Name, const std::filesystem::path &AbcPath,
                       std::string Description, std::string SourceSql) {
	UnregisterProcedure(Catalog, Name);
	StoredProcedureEntry E;
	E.Name = std::move(Name);
	E.AbcPath = AbcPath;
	E.Description = std::move(Description);
	E.SourceSql = std::move(SourceSql);
	if(!E.SourceSql.empty())
		E.SourceHash = HashProcedureSource(E.SourceSql);
	const auto Loaded = LoadAbcFile(AbcPath);
	const auto Analysis = AnalyzeBytecode(Loaded.Instructions);
	E.ReferencedTables = Analysis.ReferencedTables;
	E.DependsOn = ScanProcedureCallsInSql(E.SourceSql);
	E.BytecodeMeta = AnalyzeProcedureBytecode(Loaded.Instructions);
	Catalog.Procedures.push_back(std::move(E));
	RebuildProcedureRelations(Catalog);
}

bool UnregisterProcedure(ProcedureCatalog &Catalog, std::string_view Name) {
	const auto It = std::find_if(Catalog.Procedures.begin(), Catalog.Procedures.end(),
	                              [&](const StoredProcedureEntry &E) { return E.Name == Name; });
	if(It == Catalog.Procedures.end())
		return false;
	DropProcedureCacheFiles(*It);
	Catalog.Procedures.erase(It);
	RebuildProcedureRelations(Catalog);
	return true;
}

std::optional<StoredProcedureEntry> FindProcedure(const ProcedureCatalog &Catalog, std::string_view Name) {
	for(const StoredProcedureEntry &E : Catalog.Procedures)
		if(E.Name == Name)
			return E;
	return std::nullopt;
}

LoadedAbcFile LoadProcedureBytecode(const StoredProcedureEntry &Entry, const ProcedureCatalog &Catalog) {
	const auto Resolved = ResolveBesideCatalog(Catalog.CatalogPath, Entry.AbcPath);
	return LoadAbcFile(Resolved);
}

void DropProcedureCacheFiles(const StoredProcedureEntry &Entry) {
	std::error_code Ec;
	if(!Entry.AbcPath.empty())
		std::filesystem::remove(Entry.AbcPath, Ec);
	if(!Entry.SqlPath.empty())
		std::filesystem::remove(Entry.SqlPath, Ec);
}

StoredProcedureEntry CacheProcedureFromSql(ProcedureCatalog &Catalog, const std::filesystem::path &SessionDbPath,
                                           std::string Name, std::string BodySql, Logger *Logger,
                                           OptimizationLevel OptLevel, const Database *CatalogDb, bool IfNotExists,
                                           bool OrReplace, std::string SourceDialect,
                                           const std::vector<ProcedureExceptionWhen> &ExceptionHandlers,
                                           std::string_view ControlFlowJson) {
	if(auto Existing = FindProcedure(Catalog, Name)) {
		if(IfNotExists)
			return *Existing;
		if(!OrReplace)
			throw std::runtime_error(AstralDB::Err::Prefixed("PROC", "Procedure \"" + Name + "\" already exists."));
	}
	const std::filesystem::path CacheDir = DefaultProcedureCacheDir(SessionDbPath);
	std::error_code Ec;
	std::filesystem::create_directories(CacheDir, Ec);
	LoweredProcedureBody Lowered = DecodeProcedureControlJson(ControlFlowJson, BodySql);
	if(Lowered.Segments.empty() && !BodySql.empty()) {
		ProcedureControlSegment Seg;
		Seg.LinearSql = BodySql;
		Lowered.Segments.push_back(std::move(Seg));
	}
	Lowered.ExceptionHandlers = ExceptionHandlers;
	Lowered.TryBodySql = BodySql;
	const auto Compiled = CompileProcedureBody(Logger, OptLevel, CatalogDb, Lowered);
	const std::filesystem::path AbcPath = CacheDir / (Name + ".abc");
	const std::filesystem::path SqlPath = CacheDir / (Name + ".sql");
	SaveAbcFile(AbcPath, Compiled);
	{
		std::ofstream SqlOut(SqlPath);
		if(!SqlOut)
			throw std::runtime_error(AstralDB::Err::Prefixed("PROC", "Cannot write procedure SQL cache: " + SqlPath.string()));
		SqlOut << BodySql;
	}
	UnregisterProcedure(Catalog, Name);
	StoredProcedureEntry E;
	E.Name = std::move(Name);
	E.AbcPath = AbcPath;
	E.SqlPath = SqlPath;
	E.SourceSql = std::move(BodySql);
	E.SourceDialect = std::move(SourceDialect);
	E.SourceHash = HashProcedureSource(E.SourceSql);
	const auto Analysis = AnalyzeBytecode(Compiled.Instructions);
	E.ReferencedTables = Analysis.ReferencedTables;
	E.DependsOn = ScanProcedureCallsInSql(E.SourceSql);
	E.BytecodeMeta = AnalyzeProcedureBytecode(Compiled.Instructions);
	Catalog.Procedures.push_back(std::move(E));
	RebuildProcedureRelations(Catalog);
	return Catalog.Procedures.back();
}

std::string FormatProcedureEntrySummary(const StoredProcedureEntry &Entry) {
	std::ostringstream Out;
	Out << "name=" << Entry.Name << "\nabc=" << Entry.AbcPath.string() << "\n";
	if(!Entry.SqlPath.empty())
		Out << "sql=" << Entry.SqlPath.string() << "\n";
	if(!Entry.SourceHash.empty())
		Out << "source_hash=" << Entry.SourceHash << "\n";
	if(!Entry.SourceDialect.empty())
		Out << "source_dialect=" << Entry.SourceDialect << "\n";
	Out << "bytecode_instructions=" << Entry.BytecodeMeta.InstructionCount << "\n";
	Out << "exception_handlers=" << Entry.BytecodeMeta.ExceptionHandlerCount << "\n";
	if(Entry.BytecodeMeta.HasExceptionHandlers)
		Out << "has_exception_handlers=yes\n";
	auto Emit = [&Out](const char *Label, const std::vector<std::string> &V) {
		Out << Label << "=";
		if(V.empty()) {
			Out << "(none)\n";
			return;
		}
		for(std::size_t I = 0; I < V.size(); ++I) {
			if(I)
				Out << ",";
			Out << V[I];
		}
		Out << "\n";
	};
	Emit("tables", Entry.ReferencedTables);
	Emit("depends_on", Entry.DependsOn);
	Emit("called_by", Entry.CalledBy);
	Emit("calls_procedures", Entry.BytecodeMeta.CalledProcedures);
	Emit("exception_conditions", Entry.BytecodeMeta.ExceptionConditions);
	return Out.str();
}

std::string FormatProcedureRelations(const ProcedureCatalog &Catalog) {
	std::ostringstream Out;
	Out << "procedures=" << Catalog.Procedures.size() << "\n";
	for(const auto &[Abc, Names] : Catalog.AbcIndex) {
		Out << "abc\t" << Abc << "\t";
		for(std::size_t I = 0; I < Names.size(); ++I) {
			if(I)
				Out << ",";
			Out << Names[I];
		}
		Out << "\n";
	}
	for(const StoredProcedureEntry &E : Catalog.Procedures) {
		Out << "edge\t" << E.Name << "\tdepends\t";
		if(E.DependsOn.empty())
			Out << "(none)";
		else {
			for(std::size_t I = 0; I < E.DependsOn.size(); ++I) {
				if(I)
					Out << ",";
				Out << E.DependsOn[I];
			}
		}
		Out << "\n";
	}
	return Out.str();
}

} // namespace SQL
} // namespace AstralDB
