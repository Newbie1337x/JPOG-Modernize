// motion.cpp - object motion vectors for FSR 3.1 ([Antialiasing] ObjectMotion).
//
// FSR's own inputs (fsr31.cpp) reproject the depth through the previous camera: exact for anything
// that does not move by itself. Animals do: their skinned draws are recorded with this frame's
// vertex constants (bone palette, c0..c3) and matched to the same animal's draw of the previous
// frame (same mesh, nearest root bone). At the resolve they are drawn again into a motion target
// with a vertex shader that runs the game's code twice, with this and last frame's constants
// (shaders.cpp TranslateVS motion), and writes previous minus current UV where the scene's depth
// is that of the draw. FSR takes those vectors where they exist and the camera's elsewhere.
#include "device.h"
#include <DirectXMath.h>
using namespace DirectX;

namespace {
const UINT kMotionRegs = 128; // vertex constants kept per draw (the game writes c0..c96)

const char* kMotionPS = R"(
struct MotionIn { float4 pos : SV_Position; float4 current : TEXCOORD0; float4 previous : TEXCOORD1; };
float4 main(MotionIn i) : SV_Target {
  float d = LoadDepth(int2(i.pos.xy));
  if (abs(i.pos.z - d) > 1e-5 + 2 * fwidth(i.pos.z)) discard; // hidden or cut out here
  if (i.current.w <= 1e-6 || i.previous.w <= 1e-6) discard;
  float2 now = float2(i.current.x / i.current.w * 0.5 + 0.5, 0.5 - i.current.y / i.current.w * 0.5);
  float2 before = float2(i.previous.x / i.previous.w * 0.5 + 0.5, 0.5 - i.previous.y / i.previous.w * 0.5);
  return float4(before - now, 0, 1);
})";
}

// Distinct bones of a skinned draw's sampled vertices (the creature test of CapturePusher), from
// the same cache; -1 when the draw has no skinning inputs.
int Device::DrawBoneCount(UINT firstVertex, UINT numVertices)
{
    const size_t idx = m_vsHandle >> 1;
    if (!numVertices || idx >= m_vertexShaders.size() || !m_vertexShaders[idx]) return -1;
    const VSDecl& decl = m_vertexShaders[idx]->parsed;
    const VSInput *weights = nullptr, *indices = nullptr;
    for (const VSInput& in : decl.inputs) {
        if (in.stream != 0) continue;
        if (in.reg == 2 && in.type == 4) weights = &in;
        if (in.reg == 3 && in.type == 4) indices = &in;
    }
    VertexBuffer* vb = m_streams[0].vb;
    const UINT stride = m_streams[0].stride;
    if (!weights || !indices || !vb || !stride || numVertices >= (1u << 24)) return -1;
    const std::vector<BYTE>& data = vb->m_impl.m_shadow;
    if ((size_t)(firstVertex + numVertices) * stride > data.size()) return -1;
    const BufferImpl& impl = vb->m_impl;
    if (m_boneCounts.empty()) m_boneCounts.resize(kBoneCounts);
    BoneCount& known = m_boneCounts[(UINT)((impl.m_identity * 0x9E3779B1u) ^ (firstVertex * 0x85EBCA6Bu) ^ numVertices) % kBoneCounts];
    if (known.identity == impl.m_identity && known.revision == impl.m_revision && known.first == firstVertex &&
        known.count == numVertices && known.vs == m_vsHandle && !memcmp(known.c4, m_vsConst[4], sizeof(known.c4)))
        return (int)known.distinct;
    BYTE used[256] = {};
    UINT distinct = 0;
    const UINT samples = numVertices < 48 ? numVertices : 48;
    for (UINT k = 0; k < samples; k++) {
        const BYTE* v = data.data() + (size_t)(firstVertex + k * numVertices / samples) * stride;
        const BYTE* w = v + weights->offset;
        const BYTE* i = v + indices->offset;
        const float wv[4] = { w[2] / 255.0f, w[1] / 255.0f, w[0] / 255.0f, w[3] / 255.0f };
        const float iv[4] = { i[2] / 255.0f, i[1] / 255.0f, i[0] / 255.0f, i[3] / 255.0f };
        for (int j = 0; j < 4; j++) {
            if (wv[j] <= 0) continue;
            const int a0 = (int)std::floor(m_vsConst[4][j] * iv[j] + 0.001f);
            if (a0 < 0 || 5 + a0 + 2 >= kMaxVSConst) continue;
            if (!used[a0 & 255]) { used[a0 & 255] = 1; distinct++; }
        }
    }
    known = { impl.m_identity, impl.m_revision, firstVertex, numVertices, {}, m_vsHandle, distinct };
    memcpy(known.c4, m_vsConst[4], sizeof(known.c4));
    return (int)distinct;
}

bool Device::ObjectMotionActive() const
{
    return g_config.objectMotion && g_config.effectsEnabled && g_config.fsr31 && !m_fsr31Failed;
}

// An animal's skinned draw of the world pass (drawn now or added to an instanced batch): keep its
// constants for the next frame and, when the same animal was drawn last frame, record the draw for
// the motion pass. bones: DrawBoneCount of the draw. The input state is the one PrepareDraw bound
// for it (or for the first draw of its batch: the same buffers and layout).
void Device::RecordMotion(UINT count, UINT start, INT base, int bones, D3D11_PRIMITIVE_TOPOLOGY topo)
{
    if (!ObjectMotionActive() || !m_worldDraw || m_worldKind != 1 || m_waterDraw || !m_indices) return;
    if (bones < g_config.grassMinBones) return;
    if (m_indices->m_impl.m_dynamic || !m_indices->m_impl.m_buf) return;
    const size_t idx = m_vsHandle >> 1;
    VertexShader8* vs = idx < m_vertexShaders.size() ? m_vertexShaders[idx] : nullptr;
    VertexBuffer* vb = m_streams[0].vb;
    if (!vs || !vb) return;
    if (!vs->motionState) {
        std::string error;
        const std::string hlsl = TranslateVS(vs->func.data(), vs->parsed, error, false, false, true);
        char name[64];
        sprintf_s(name, "vs_%04lx_motion", m_vsHandle);
        ComPtr<ID3DBlob> blob = hlsl.empty() ? nullptr : CompileHLSL(hlsl, "vs_5_0", name);
        const bool ok = blob && SUCCEEDED(m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs->motion));
        vs->motionState = ok ? 1 : -1;
        Log("object motion: VS 0x%lx %s", m_vsHandle, ok ? "motion variant ready" : (error.empty() ? "variant failed" : error.c_str()));
    }
    if (vs->motionState < 0) return;

    // The animal's root: bone 0's origin through c0..c3 to clip, then to the world.
    const float origin[4] = { m_vsConst[5][3], m_vsConst[6][3], m_vsConst[7][3], 1 };
    float clip[4], world[4];
    for (int r = 0; r < 4; r++) clip[r] = origin[0] * m_vsConst[r][0] + origin[1] * m_vsConst[r][1] + origin[2] * m_vsConst[r][2] + origin[3] * m_vsConst[r][3];
    for (int r = 0; r < 4; r++) world[r] = clip[0] * m_sun.clipToWorld[r][0] + clip[1] * m_sun.clipToWorld[r][1] + clip[2] * m_sun.clipToWorld[r][2] + clip[3] * m_sun.clipToWorld[r][3];
    if (!(std::fabs(world[3]) > 1e-8f)) return;
    for (int k = 0; k < 3; k++) world[k] /= world[3];

    const UINT64 key = (vb->m_impl.m_identity * 0x9E3779B97F4A7C15ull) ^ (m_indices->m_impl.m_identity * 0xC2B2AE3D27D4EB4Full) ^
                       ((UINT64)start << 32) ^ ((UINT64)count << 8) ^ (UINT64)(UINT)base ^ ((UINT64)m_vsHandle << 48);
    MotionEntry cur = { key, { world[0], world[1], world[2] }, (UINT)(m_motionCurRegs.size() / 4), false };
    const UINT regs = (std::min)(kMotionRegs, (UINT)kMaxVSConst);
    m_motionCurRegs.insert(m_motionCurRegs.end(), &m_vsConst[0][0], &m_vsConst[0][0] + regs * 4);
    m_motionCur.push_back(cur);
    ++m_motionRecorded;

    // Last frame's draw of the same mesh whose root is nearest (an animal moves well under 6 units a frame).
    MotionEntry* prev = nullptr;
    float best = 6.0f * 6.0f;
    for (MotionEntry& e : m_motionPrev) {
        if (e.key != key || e.used) continue;
        float d2 = 0;
        for (int k = 0; k < 3; k++) d2 += (e.anchor[k] - cur.anchor[k]) * (e.anchor[k] - cur.anchor[k]);
        if (d2 < best) { best = d2; prev = &e; }
    }
    if (!prev) return;
    prev->used = true;
    ++m_motionMatched;
    // Nothing moved at all (camera and animal still): the camera vectors are exact.
    if (!memcmp(&m_motionPrevRegs[(size_t)prev->offset * 4], &m_motionCurRegs[(size_t)cur.offset * 4], regs * 16)) return;

    MotionDraw d = {};
    for (int s = 0; s < kMaxStreams; s++) {
        if (!(vs->parsed.streamMask & (1u << s))) continue;
        VertexBuffer* sv = m_streams[s].vb;
        // Buffers rewritten during the frame would hold other vertices by the motion pass.
        if (!sv || sv->m_impl.m_dynamic || !sv->m_impl.m_buf) return;
        d.vb[s] = sv->m_impl.m_buf;
        d.stride[s] = m_streams[s].stride; // offsets are always 0
    }
    d.ib = m_indices->m_impl.m_buf;
    d.ibFormat = m_indices->m_format == D8FMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    d.layout = vs->layout;
    d.topo = topo;
    if (!d.layout) return;
    d.vs = vs->motion;
    d.count = count; d.start = start; d.base = base;
    d.current = cur.offset; d.previous = prev->offset; d.regs = regs;
    m_motionDraws.push_back(std::move(d));
}

// At the FSR resolve, before its inputs are built: the recorded animals into the motion target
// (cleared: alpha 0 means "use the camera's vector"). depth: the scene's depth, samples per pixel.
ID3D11ShaderResourceView* Device::DrawObjectMotion(ID3D11ShaderResourceView* depth, UINT samples, UINT w, UINT h)
{
    if (!ObjectMotionActive()) return nullptr;
    if (!m_objMotionTex || m_objMotionW != w || m_objMotionH != h) {
        m_objMotionTex.Reset(); m_objMotionRTV.Reset(); m_objMotionSRV.Reset();
        D3D11_TEXTURE2D_DESC td = { w, h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_objMotionTex)) ||
            FAILED(m_device->CreateRenderTargetView(m_objMotionTex.Get(), nullptr, &m_objMotionRTV)) ||
            FAILED(m_device->CreateShaderResourceView(m_objMotionTex.Get(), nullptr, &m_objMotionSRV))) {
            Log("object motion: target creation failed, disabled");
            g_config.objectMotion = false;
            return nullptr;
        }
        m_objMotionW = w; m_objMotionH = h;
    }
    if (!m_motionPS || m_motionPSSamples != samples) {
        m_motionPS.Reset();
        const std::string load = samples > 1
            ? "Texture2DMS<float," + std::to_string(samples) + "> sceneDepth : register(t0);\n"
              "float LoadDepth(int2 p) { float d = 1; [unroll] for (int i = 0; i < " + std::to_string(samples) + "; i++) d = min(d, sceneDepth.Load(p, i)); return d; }\n"
            : "Texture2D<float> sceneDepth : register(t0);\nfloat LoadDepth(int2 p) { return sceneDepth.Load(int3(p, 0)); }\n";
        auto blob = CompileHLSL(load + kMotionPS, "ps_5_0", "object_motion_ps");
        D3D11_BUFFER_DESC cd = { 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd = {};
        if (!blob || FAILED(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_motionPS)) ||
            (!m_motionCB && FAILED(m_device->CreateBuffer(&cd, nullptr, &m_motionCB))) ||
            (!m_motionRaster && FAILED(m_device->CreateRasterizerState(&rd, &m_motionRaster))) ||
            (!m_motionDepth && FAILED(m_device->CreateDepthStencilState(&dd, &m_motionDepth)))) {
            Log("object motion: pass shaders/states failed, disabled");
            g_config.objectMotion = false;
            return nullptr;
        }
        m_motionPSSamples = samples;
    }
    const float zero[4] = {};
    m_ctx->ClearRenderTargetView(m_objMotionRTV.Get(), zero);
    if (m_motionDraws.empty()) return m_objMotionSRV.Get();

    // This and last frame's constants of every recorded draw, side by side.
    std::vector<float>& upload = m_motionUpload;
    upload.clear();
    for (MotionDraw& d : m_motionDraws) {
        const UINT at = (UINT)(upload.size() / 4);
        upload.insert(upload.end(), &m_motionCurRegs[(size_t)d.current * 4], &m_motionCurRegs[(size_t)d.current * 4] + d.regs * 4);
        upload.insert(upload.end(), &m_motionPrevRegs[(size_t)d.previous * 4], &m_motionPrevRegs[(size_t)d.previous * 4] + d.regs * 4);
        d.current = at; d.previous = at + d.regs;
    }
    const UINT elements = (UINT)(upload.size() / 4);
    if (elements > m_motionBufElements) {
        UINT size = m_motionBufElements ? m_motionBufElements : 4096;
        while (size < elements) size *= 2;
        m_motionBuf.Reset(); m_motionSRV.Reset(); m_motionBufElements = 0;
        D3D11_BUFFER_DESC bd = { size * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16 };
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sd.Buffer.NumElements = size;
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_motionBuf)) || FAILED(m_device->CreateShaderResourceView(m_motionBuf.Get(), &sd, &m_motionSRV)))
            return m_objMotionSRV.Get();
        m_motionBufElements = size;
    }
    const D3D11_BOX box = { 0, 0, 0, elements * 16, 1, 1 };
    m_ctx->UpdateSubresource(m_motionBuf.Get(), 0, &box, upload.data(), 0, 0);

    PerformanceScope perf(m_perf, m_device.Get(), m_ctx.Get(), Performance::Temporal);
    m_ctx->OMSetRenderTargets(1, m_objMotionRTV.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->RSSetState(m_motionRaster.Get());
    m_ctx->OMSetDepthStencilState(m_motionDepth.Get(), 0);
    m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
    m_ctx->PSSetShader(m_motionPS.Get(), nullptr, 0);
    m_ctx->PSSetShaderResources(0, 1, &depth);
    ID3D11Buffer* vscb[2] = { m_vsCB.Get(), m_vsExtraCB.Get() }; // c[] beyond the kept registers; this frame's jitter
    m_ctx->VSSetConstantBuffers(0, 2, vscb);
    m_ctx->VSSetConstantBuffers(6, 1, m_motionCB.GetAddressOf());
    m_ctx->VSSetShaderResources(24, 1, m_motionSRV.GetAddressOf());
    for (const MotionDraw& d : m_motionDraws) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(m_ctx->Map(m_motionCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) continue;
        const UINT info[4] = { d.current, d.previous, d.regs, 0 };
        memcpy(map.pData, info, sizeof(info));
        m_ctx->Unmap(m_motionCB.Get(), 0);
        ID3D11Buffer* vbs[kMaxStreams];
        for (int s = 0; s < kMaxStreams; s++) vbs[s] = d.vb[s].Get();
        m_ctx->IASetVertexBuffers(0, kMaxStreams, vbs, d.stride, d.offset);
        m_ctx->IASetIndexBuffer(d.ib.Get(), d.ibFormat, d.ibOffset);
        m_ctx->IASetInputLayout(d.layout.Get());
        m_ctx->IASetPrimitiveTopology(d.topo);
        m_ctx->VSSetShader(d.vs.Get(), nullptr, 0);
        m_ctx->DrawIndexed(d.count, d.start, d.base);
    }
    m_motionDrawn += (UINT)m_motionDraws.size();
    ID3D11ShaderResourceView* none = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &none);
    m_ctx->VSSetShaderResources(24, 1, &none);
    ID3D11RenderTargetView* noRT = nullptr;
    m_ctx->OMSetRenderTargets(1, &noRT, nullptr);
    // Everything the game's draws had bound is rebound by the next PrepareDraw.
    InvalidateBindings();
    m_rtDirty = m_viewportDirty = true;
    m_lastBlend = nullptr; m_lastDepth = nullptr; m_lastRaster = nullptr;
    m_lastVS = nullptr; m_lastPS = nullptr; m_lastLayout = nullptr; m_lastIB = nullptr;
    for (int s = 0; s < kMaxStreams; s++) m_lastVB[s] = nullptr;
    return m_objMotionSRV.Get();
}

// At Present: this frame's draws become the previous frame's.
void Device::EndMotionFrame()
{
    m_motionDraws.clear();
    m_motionPrev.swap(m_motionCur);
    m_motionPrevRegs.swap(m_motionCurRegs);
    m_motionCur.clear();
    m_motionCurRegs.clear();
    if (m_motionStatsFrame + 600 <= m_frame) {
        if (m_motionRecorded && m_motionLogged < 30) {
            ++m_motionLogged;
            Log("object motion: %u animal draws, %u matched to the previous frame, %u drawn into the motion target (last 600 frames)",
                m_motionRecorded, m_motionMatched, m_motionDrawn);
        }
        m_motionStatsFrame = (UINT)m_frame;
        m_motionRecorded = m_motionMatched = m_motionDrawn = 0;
    }
}
