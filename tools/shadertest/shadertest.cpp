// shadertest.exe - runs every shader dumped by the D3D8 profiler through the d3d8to11 translator
// and the HLSL compiler, plus a few fixed-function pipeline keys. Prints failures.
// Usage: shadertest.exe <game_dir>\d3d8_profile [out_dir_for_hlsl]
#include "../../src/d3d8to11/shaders.h"
#include <stdio.h>
#include <fstream>
#include <iterator>

static std::vector<DWORD> ReadTokens(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<DWORD> t(bytes.size() / 4 + 1, 0xFFFFFFFF);
    memcpy(t.data(), bytes.data(), bytes.size());
    return t;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: shadertest <d3d8_profile dir> [hlsl out dir]\n"); return 1; }
    std::string dir = std::string(argv[1]) + "\\";
    std::string outDir = argc > 2 ? std::string(argv[2]) + "\\" : "";
    LogInit(dir + "shadertest.log");
    int ok = 0, fail = 0;
    WIN32_FIND_DATAA fd;

    HANDLE h = FindFirstFileA((dir + "vs_*_func.bin").c_str(), &fd);
    for (BOOL more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &fd)) {
        std::string func = fd.cFileName;
        std::string declName = func.substr(0, func.size() - 8) + "decl.bin";
        std::vector<DWORD> decl = ReadTokens(dir + declName), code = ReadTokens(dir + func);
        VSDecl parsed;
        ParseVSDecl(decl.data(), parsed);
        std::string err, hlsl = TranslateVS(code.data(), parsed, err);
        bool good = !hlsl.empty() && CompileHLSL(hlsl, "vs_4_0", func);
        printf("%-32s %s %s\n", func.c_str(), good ? "OK" : "FAIL", err.c_str());
        if (!outDir.empty() && !hlsl.empty()) std::ofstream(outDir + func + ".hlsl") << hlsl;
        (good ? ok : fail)++;
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);

    h = FindFirstFileA((dir + "ps_*.bin").c_str(), &fd);
    for (BOOL more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &fd)) {
        std::vector<DWORD> code = ReadTokens(dir + fd.cFileName);
        PSVariant v;
        for (int s = 0; s < 4; s++) v.texDim[s] = 2;
        v.alphaFunc = 7;
        std::string err, hlsl = TranslatePS(code.data(), v, err);
        bool good = !hlsl.empty() && CompileHLSL(hlsl, "ps_4_0", fd.cFileName);
        printf("%-32s %s %s\n", fd.cFileName, good ? "OK" : "FAIL", err.c_str());
        if (!outDir.empty() && !hlsl.empty()) std::ofstream(outDir + fd.cFileName + ".hlsl") << hlsl;
        (good ? ok : fail)++;
        v.shadowOnly = 1;
        hlsl = TranslatePS(code.data(), v, err);
        good = !hlsl.empty() && CompileHLSL(hlsl, "ps_4_0", std::string("shadow_") + fd.cFileName);
        printf("shadow %-25s %s\n", fd.cFileName, good ? "OK" : "FAIL");
        (good ? ok : fail)++;
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);

    // Fixed-function keys seen in the profile: stage 0 MODULATE/SELECTARG1, stage 1 bump env map.
    BYTE ops[][2] = { { 4, 2 }, { 2, 2 }, { 4, 4 }, { 22, 2 } };
    for (auto& o : ops) {
        FFKey k = {};
        k.stages[0] = { o[0], 2, 0, 1, o[1], 2, 0, 1, 0, 2, 0, 0 };
        k.stages[1] = { 4, 2, 1, 1, 2, 1, 1, 1, 1, 2, 0, 0 };
        k.alphaFunc = 5;
        k.specular = 1;
        std::string hlsl = GenerateFFPS(k);
        bool good = CompileHLSL(hlsl, "ps_4_0", "ff_test") != nullptr;
        printf("ff colorop %-3d alphaop %-3d       %s\n", o[0], o[1], good ? "OK" : "FAIL");
        (good ? ok : fail)++;
    }
    printf("\n%d ok, %d failed (details in shadertest.log)\n", ok, fail);
    return fail ? 2 : 0;
}
