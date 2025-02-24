# sources macro for windows, recursively interates thru any subdirs to find *.cxx source files
SOURCES_WIN = \
	$(shell find sources -type f -name "*.cxx")

# sources macro for posix, recursively interates thru any subdirs to find *.cxx source files
SOURCES_POSIX = \
	$(shell find sources -type f -name "*.cxx")

build-win:
	clang++ -std=c++23 -O2 - Isources/ $(SOURCES_WIN) -o AstralDB.exe

build-posix:
	clang++ -std=c++23 -Isources/ -O2 $(SOURCES_POSIX) -o AstralDB

clean-win:
	del AstralDB.exe

clean-posix:
	rm AstralDB