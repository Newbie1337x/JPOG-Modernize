// shader_jobs.h - pixel shaders compiled on background threads ([Performance] AsyncShaders,
// PrewarmShaders).
//
// A pixel shader variant (a PSVariant of a game shader, or a fixed-function FFKey) is first needed
// in the middle of a frame, and compiling it there stops the game for the 0.2 - 1.5 s D3DCompile
// takes. Instead the variant is submitted here and its draws are skipped until a worker has
// compiled it and created the D3D11 shader (ID3D11Device is free-threaded): a frame or two.
//
// Every variant used is also recorded in d3d8to11_shadercache\variants.bin, and the renderer ships
// d3d8to11_shadervariants.bin with those of a full play session. In every session the
// variants of a game pixel shader are submitted as soon as the game creates it (while loading),
// and the fixed-function ones when the device is created, so they are ready before they are drawn.
// The disk cache (CompileHLSL) turns most of those into a file read.
#pragma once
#include "shaders.h"
#include <atomic>
#include <memory>

namespace shader_jobs {
struct Job {
    std::string hlsl, name;
    ComPtr<ID3D11Device> device;
    std::atomic<bool> done{false};    // set after shader; shader stays null when compilation failed
    ComPtr<ID3D11PixelShader> shader;
    bool prewarm = false;
};
// Prewarm jobs run after every job a draw is waiting for.
std::shared_ptr<Job> Submit(ID3D11Device* device, std::string hlsl, std::string name, bool prewarm);
// A draw is waiting for this prewarm job: it runs next if it has not started.
void Promote(const std::shared_ptr<Job>& job);

// variants.bin.
UINT64 FunctionHash(const std::vector<DWORD>& func);
void RecordVariant(UINT64 function, const PSVariant& v);
void RecordFF(const FFKey& k);
std::vector<PSVariant> KnownVariants(UINT64 function);
std::vector<FFKey> KnownFF();
}
