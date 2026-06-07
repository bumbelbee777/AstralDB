#include <SQL/Profiler/PragmaHandler.hxx>

#include <SQL/Bytecode/BytecodeInterpreter.hxx>
#include <SQL/Bytecode/QueryCheckpoint.hxx>
#include <SQL/Profiler/QueryProfiler.hxx>

#include <algorithm>
#include <cctype>

namespace AstralDB {
namespace SQL {

namespace {

std::string Lower(std::string S) {
	for(char &C : S)
		C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
	return S;
}

bool ParseOnOff(std::string_view V, bool &Out) {
	const std::string L = Lower(std::string(V));
	if(L == "on" || L == "1" || L == "true" || L == "yes")
		Out = true;
	else if(L == "off" || L == "0" || L == "false" || L == "no")
		Out = false;
	else
		return false;
	return true;
}

} // namespace

void ApplyPragmaAST(const PragmaAST &Pragma, SqlSessionConfig &Cfg, BytecodeInterpreter *Interpreter) {
	switch(Pragma.Kind_) {
	case PragmaAST::Kind::Profile:
		if(Pragma.Value.empty() || Lower(Pragma.Value) == "null")
			Cfg.ProfileName.reset();
		else {
			Cfg.ProfileName = Pragma.Value;
			QueryProfiler::Instance().BeginQuery(Pragma.Value);
		}
		break;
	case PragmaAST::Kind::Region:
		Cfg.RegionName = Pragma.Value.empty() ? std::optional<std::string>{} : std::optional<std::string>{Pragma.Value};
		break;
	case PragmaAST::Kind::VectorBatchSize:
		Cfg.VectorBatchSize = static_cast<std::uint32_t>(std::max<int64_t>(1, Pragma.IntValue));
		break;
	case PragmaAST::Kind::JoinStrategy: {
		const std::string L = Lower(Pragma.Value);
		if(L == "radix")
			Cfg.JoinStrategy_ = SqlSessionConfig::JoinStrategy::Radix;
		else if(L == "hash")
			Cfg.JoinStrategy_ = SqlSessionConfig::JoinStrategy::Hash;
		else
			Cfg.JoinStrategy_ = SqlSessionConfig::JoinStrategy::Auto;
		break;
	}
	case PragmaAST::Kind::ZoneMapAdaptive:
		ParseOnOff(Pragma.Value, Cfg.ZoneMapAdaptive);
		break;
	case PragmaAST::Kind::LazyMaterialization:
		ParseOnOff(Pragma.Value, Cfg.LazyMaterialization);
		break;
	case PragmaAST::Kind::Vm: {
		const std::string L = Lower(Pragma.Value);
		if(L == "dbvm")
			Cfg.Vm = SqlSessionConfig::VmChoice::Dbvm;
		else if(L == "silly")
			Cfg.Vm = SqlSessionConfig::VmChoice::Silly;
		else if(L == "jit")
			Cfg.Vm = SqlSessionConfig::VmChoice::Jit;
		else
			Cfg.Vm = SqlSessionConfig::VmChoice::Auto;
		break;
	}
	case PragmaAST::Kind::OptLevel: {
		const int L = static_cast<int>(Pragma.IntValue);
		if(L <= 0)
			Cfg.OptLevel = OptimizationLevel::None;
		else if(L == 1)
			Cfg.OptLevel = OptimizationLevel::Basic;
		else if(L == 2)
			Cfg.OptLevel = OptimizationLevel::Advanced;
		else if(L == 3)
			Cfg.OptLevel = OptimizationLevel::Aggressive;
		else
			Cfg.OptLevel = OptimizationLevel::Maximum;
		break;
	}
	case PragmaAST::Kind::VerifyFastPath:
		ParseOnOff(Pragma.Value, Cfg.VerifyFastPath);
		break;
	case PragmaAST::Kind::Explain: {
		const std::string L = Lower(Pragma.Value);
		if(L == "on" || L == "1")
			Cfg.Explain = SqlSessionConfig::ExplainMode::On;
		else if(L == "analyze")
			Cfg.Explain = SqlSessionConfig::ExplainMode::Analyze;
		else
			Cfg.Explain = SqlSessionConfig::ExplainMode::Off;
		break;
	}
	case PragmaAST::Kind::DumpProfile:
		Cfg.ProfileDumpPath = Pragma.Value;
		QueryProfiler::Instance().DumpToJSON(Pragma.Value);
		break;
	case PragmaAST::Kind::ResetProfile:
		QueryProfiler::Instance().Reset();
		break;
	case PragmaAST::Kind::UsePrecomputed:
		ParseOnOff(Pragma.Value, Cfg.UsePrecomputed);
		break;
	case PragmaAST::Kind::QueryCheckpointOnSignal:
		ParseOnOff(Pragma.Value, Cfg.QueryCheckpointOnSignal);
		if(Cfg.QueryCheckpointOnSignal)
			gRequestQueryCheckpoint.store(false, std::memory_order_release);
		break;
	case PragmaAST::Kind::QueryCheckpointInterval:
		Cfg.QueryCheckpointInterval = static_cast<std::size_t>(std::max<int64_t>(0, Pragma.IntValue));
		break;
	case PragmaAST::Kind::ResumeQueryCheckpoint:
		ParseOnOff(Pragma.Value, Cfg.ResumeQueryCheckpoint);
		if(Cfg.ResumeQueryCheckpoint && Interpreter) {
			const auto Path = DefaultQueryCheckpointPath(Interpreter->DatabasePath());
			if(const auto Loaded = LoadQueryCheckpoint(Path))
				(void)ResumeQueryCheckpoint(*Interpreter, Bytecode{}, *Loaded);
		}
		break;
	case PragmaAST::Kind::JitEnabled:
		if(Pragma.Value.empty())
			Cfg.JitEnabled = true;
		else
			(void)ParseOnOff(Pragma.Value, Cfg.JitEnabled);
		if(Cfg.JitEnabled)
			Cfg.Vm = SqlSessionConfig::VmChoice::Jit;
		else
			Cfg.Vm = SqlSessionConfig::VmChoice::Auto;
		break;
	case PragmaAST::Kind::ForeignKeys:
		if(Pragma.Value.empty())
			Cfg.ForeignKeys = true;
		else
			(void)ParseOnOff(Pragma.Value, Cfg.ForeignKeys);
		break;
	case PragmaAST::Kind::OptConfidenceFloor:
		if(!Pragma.Value.empty()) {
			try {
				Cfg.OptConfidenceFloor = std::stod(Pragma.Value);
			} catch(...) {
				Cfg.OptConfidenceFloor = static_cast<double>(Pragma.IntValue) / 1000.0;
			}
			if(Cfg.OptConfidenceFloor < 0.0)
				Cfg.OptConfidenceFloor = 0.0;
			if(Cfg.OptConfidenceFloor > 1.0)
				Cfg.OptConfidenceFloor = 1.0;
		}
		break;
	case PragmaAST::Kind::CompatAck:
		break;
	}
}

void ApplyPragmaStatementsFromAst(SqlSessionConfig &Cfg, BytecodeInterpreter *Interpreter) {
	for(auto &Node : AST) {
		if(!Node || !Node->Value)
			continue;
		if(auto *P = dynamic_cast<PragmaAST *>(Node->Value.get()))
			ApplyPragmaAST(*P, Cfg, Interpreter);
	}
}

} // namespace SQL
} // namespace AstralDB
