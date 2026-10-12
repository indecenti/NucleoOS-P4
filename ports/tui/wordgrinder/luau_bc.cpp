// luau_bc.cpp — compile one Luau source file to Luau bytecode on the PC, with the options
// WordGrinder's own loader uses on the device (src/c/luau-em/lauxlib.cc: optimisation 2, debug
// level 1, declaration syntax allowed). The NucleoOS build embeds the bytecode instead of the
// sources, so the device no longer compiles 17,000 lines of Lua at every start.
//
//   luau_bc IN.lua OUT.bc      exit 1 (message on stderr) on a compile error
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include "Luau/Compiler.h"

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: luau_bc IN.lua OUT.bc\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in)
    {
        fprintf(stderr, "luau_bc: cannot read %s\n", argv[1]);
        return 1;
    }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    Luau::CompileOptions copts = {};
    copts.optimizationLevel = 2;
    copts.debugLevel = 1;
    copts.coverageLevel = 0;
    Luau::ParseOptions popts = {};
    popts.allowDeclarationSyntax = true;

    std::string bc = Luau::compile(src, copts, popts);
    if (bc.empty() || bc[0] == 0)   // an error: the message follows the 0 byte
    {
        fprintf(stderr, "luau_bc: %s%s\n", argv[1], bc.empty() ? ": empty" : bc.c_str() + 1);
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    out.write(bc.data(), (std::streamsize)bc.size());
    return out ? 0 : 1;
}
