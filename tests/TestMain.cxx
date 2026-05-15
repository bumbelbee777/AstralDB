#define DOCTEST_CONFIG_COLORS_NONE
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <filesystem>

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
} // namespace

int main(int argc, char **argv) {
	NormalizeWorkingDirectoryToRepoRoot();
	doctest::Context Ctx;
	Ctx.applyCommandLine(argc, argv);
	Ctx.setOption("duration", false);
	return Ctx.run();
}
