#define DOCTEST_CONFIG_COLORS_NONE
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <filesystem>
#include <string_view>

namespace {
namespace fs = std::filesystem;

/** When the binary is started from e.g. \c bin/, doctest's console reporter can fault; tests also expect repo layout. */
void NormalizeWorkingDirectoryToRepoRoot() {
	std::error_code Ec;
	fs::path P = fs::current_path(Ec);
	for(int Depth = 0; Depth < 12 && !P.empty(); ++Depth) {
		if(fs::is_directory(P / "examples", Ec) && fs::is_directory(P / "sources", Ec)) {
			if(P != fs::current_path(Ec))
				fs::current_path(P, Ec);
			return;
		}
		P = P.parent_path();
	}
}
bool ArgRequestsTestSuite(int Argc, char **Argv) {
	for(int I = 1; I < Argc; ++I) {
		const std::string_view A(Argv[I]);
		if(A == "--test-suite" || A == "-ts" || A.starts_with("--test-suite=") || A.starts_with("-ts="))
			return true;
	}
	return false;
}

} // namespace

int main(int argc, char **argv) {
	NormalizeWorkingDirectoryToRepoRoot();
	doctest::Context Ctx;
	Ctx.applyCommandLine(argc, argv);
	Ctx.setOption("duration", false);
	if(!ArgRequestsTestSuite(argc, argv))
		Ctx.addFilter("test-suite-exclude", "perf");
	return Ctx.run();
}
