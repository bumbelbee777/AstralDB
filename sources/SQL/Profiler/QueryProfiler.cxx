#include <SQL/Profiler/QueryProfiler.hxx>
#include <Database/Profile/RegionTimer.hxx>

#include <fstream>
#include <iomanip>
#include <sstream>

namespace AstralDB {
namespace SQL {

QueryProfiler &QueryProfiler::Instance() {
	static QueryProfiler Inst;
	static const bool HookOnce = ([] {
		SetRegionTimingHook([](const std::string &Name, const std::chrono::nanoseconds Elapsed) {
			QueryProfiler::Instance().RecordRegionTiming(Name, Elapsed);
		});
		return true;
	})();
	(void)HookOnce;
	return Inst;
}

void QueryProfiler::BeginQuery(const std::string &Name) { ActiveName_ = Name; }

void QueryProfiler::EndQuery(const QueryProfile &Profile) {
	QueryProfile P = Profile;
	if(P.Name.empty())
		P.Name = ActiveName_;
	History_.push_back(std::move(P));
}

void QueryProfiler::Reset() {
	History_.clear();
	ActiveName_.clear();
	RegionTimings_.clear();
}

void QueryProfiler::RecordRegionTiming(const std::string &Name, const std::chrono::nanoseconds Elapsed) {
	if(Name.empty())
		return;
	RegionTimings_[Name] += Elapsed;
}

void QueryProfiler::ClearRegionTimings() { RegionTimings_.clear(); }

namespace {

void JsonEscape(std::ostream &O, std::string_view S) {
	for(char C : S) {
		if(C == '"')
			O << "\\\"";
		else if(C == '\\')
			O << "\\\\";
		else if(C == '\n')
			O << "\\n";
		else
			O.put(C);
	}
}

} // namespace

void QueryProfiler::DumpToJSON(const std::string &Path) const {
	std::ofstream Out(Path, std::ios::binary);
	if(!Out)
		return;
	Out << "[\n";
	for(std::size_t I = 0; I < History_.size(); ++I) {
		const QueryProfile &P = History_[I];
		if(I)
			Out << ",\n";
		Out << "  {\n";
		Out << "    \"name\": \"";
		JsonEscape(Out, P.Name);
		Out << "\",\n    \"region\": \"";
		JsonEscape(Out, P.Region);
		Out << "\",\n    \"parse_ns\": " << P.ParseTime.count() << ",\n";
		Out << "    \"compile_ns\": " << P.CompileTime.count() << ",\n";
		Out << "    \"optimize_ns\": " << P.OptimizeTime.count() << ",\n";
		Out << "    \"execute_ns\": " << P.ExecuteTime.count() << ",\n";
		Out << "    \"scanned_rows\": " << P.ScannedRows << ",\n";
		Out << "    \"result_rows\": " << P.ResultRows << ",\n";
		Out << "    \"vm_used\": \"";
		JsonEscape(Out, P.VmUsed);
		Out << "\",\n    \"verified\": " << (P.Verified ? "true" : "false");
		if(!RegionTimings_.empty()) {
			Out << ",\n    \"region_timings\": {";
			bool First = true;
			for(const auto &[Rn, Ns] : RegionTimings_) {
				if(!First)
					Out << ',';
				First = false;
				Out << "\n      \"";
				JsonEscape(Out, Rn);
				Out << "\": " << Ns.count();
			}
			Out << "\n    }";
		}
		Out << "\n  }";
	}
	Out << "\n]\n";
}

} // namespace SQL
} // namespace AstralDB
