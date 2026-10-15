// skin_batch.cpp - skinned draws of one mesh in a row (trees, fences, props) as one instanced draw.
//
// The game draws every tree on its own: the same vertex and index buffers, index range, textures,
// shaders and states, with the transform and bone palette rewritten in c0..c96 before each draw.
// With the whole island drawn those are about 1200 draws a frame (plus as many shadow casters), in
// runs of up to about 100 of the same mesh. While a run lasts its draws are recorded: the first
// draw's PrepareDraw bound everything, every setter that changes something else draws the batch
// first (FlushBatches), and each draw only adds a copy of its constant window. The batch is drawn
// with the instanced shader variant, which reads the window registers from that copy (t23).
#include "device.h"

namespace {
const UINT kInstanceElements = 1u << 20; // float4 elements of the instance buffer (16 MB)
const UINT kMaxWindow = 128;             // registers per instance
const UINT kMaxInstances = 2048;
}

bool Device::CreateSkinBatchResources()
{
    if (m_skinBuf) return true;
    D3D11_BUFFER_DESC bd = { kInstanceElements * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                             D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16 };
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = kInstanceElements;
    D3D11_BUFFER_DESC cd = { 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_skinBuf)) ||
        FAILED(m_device->CreateShaderResourceView(m_skinBuf.Get(), &sd, &m_skinSRV)) ||
        FAILED(m_device->CreateBuffer(&cd, nullptr, &m_skinCB))) {
        Log("skin instancing: buffers failed, disabled");
        m_skinBuf.Reset(); m_skinSRV.Reset(); m_skinCB.Reset();
        m_skinUnavailable = true;
        return false;
    }
    return true;
}

void Device::SetSkinInstanceInfo(const UINT info[4], ID3D11ShaderResourceView* consts)
{
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(m_ctx->Map(m_skinCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, info, 16);
        m_ctx->Unmap(m_skinCB.Get(), 0);
    }
    m_ctx->VSSetConstantBuffers(6, 1, m_skinCB.GetAddressOf());
    m_ctx->VSSetShaderResources(23, 1, &consts);
}

// After PrepareDraw of an indexed draw: record it instead of drawing it, when it can start a batch.
// writeLo/writeHi: the vertex constants the game wrote since the previous draw.
bool Device::StartSkinBatch(DWORD type, UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount,
                            UINT writeLo, UINT writeHi, UINT count, D3D11_PRIMITIVE_TOPOLOGY topo)
{
    if (!g_config.skinInstancing || m_skinUnavailable || m_worldKind != 1) return false;
    if (type != D8PT_TRIANGLESTRIP && type != D8PT_TRIANGLELIST) return false;
    if (m_tessCandidate || m_waterDraw || ActiveEmissive()) return false;
    if (writeHi <= writeLo || writeHi - writeLo > kMaxWindow) return false;
    VertexShader8* vs = m_vertexShaders[m_vsHandle >> 1];
    if (vs->instancedState < 0) return false;
    if (!vs->instancedState) {
        std::string error;
        const std::string hlsl = TranslateVS(vs->func.data(), vs->parsed, error, false, true);
        char name[64];
        sprintf_s(name, "vs_%04lx_instanced", m_vsHandle);
        ComPtr<ID3DBlob> blob = hlsl.empty() ? nullptr : CompileHLSL(hlsl, "vs_5_0", name);
        const bool ok = blob && m_device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 &&
            SUCCEEDED(m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs->instanced));
        vs->instancedState = ok ? 1 : -1;
        Log("skin instancing: VS 0x%lx %s", m_vsHandle, ok ? "instanced" : (error.empty() ? "variant failed" : error.c_str()));
        if (!ok) return false;
    }
    if (!CreateSkinBatchResources()) return false;
    m_skinVS = vs;
    m_skinType = type; m_skinMinIndex = minIndex; m_skinNumVertices = numVertices;
    m_skinStart = startIndex; m_skinPrims = primCount; m_skinCount = count; m_skinTopo = topo;
    m_skinBase = (INT)m_baseVertex;
    m_skinLo = writeLo; m_skinHi = writeHi;
    m_skinConsts.assign(&m_vsConst[writeLo][0], &m_vsConst[writeHi][0]);
    m_skinInstances = 1;
    m_skinBatchOpen = true;
    return true;
}

// A later draw while the batch is open: nothing but the window constants changed since the first
// one (the setters draw the batch on any other change), so the same range adds an instance.
bool Device::AppendSkinBatch(DWORD type, UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount)
{
    if (type != m_skinType || minIndex != m_skinMinIndex || numVertices != m_skinNumVertices ||
        startIndex != m_skinStart || primCount != m_skinPrims || m_skinInstances >= kMaxInstances)
        return false;
    if (m_vsWriteHi > m_vsWriteLo && (m_vsWriteLo < m_skinLo || m_vsWriteHi > m_skinHi)) return false;
    m_skinConsts.insert(m_skinConsts.end(), &m_vsConst[m_skinLo][0], &m_vsConst[m_skinHi][0]);
    ++m_skinInstances;
    if (GrassCaptureActive()) CapturePusher(m_baseVertex + minIndex, numVertices);
    m_vsWriteLo = kMaxVSConst; m_vsWriteHi = 0;
    ++m_skinAppended;
    return true;
}

void Device::DrawSkinBatch()
{
    m_skinBatchOpen = false;
    const UINT n = m_skinInstances;
    if (!n || !m_indices) return;
    const DXGI_FORMAT fmt = m_indices->m_format == D8FMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    ID3D11Buffer* ib = m_indices->m_impl.m_buf.Get();
    m_ctx->IASetPrimitiveTopology(m_skinTopo);
    if (m_lastIB != ib || m_lastIBFormat != fmt) {
        m_ctx->IASetIndexBuffer(ib, fmt, 0);
        m_lastIB = ib;
        m_lastIBFormat = fmt;
    }
    if (n == 1) {
        // The constant ring still holds this draw's constants (uploaded by its PrepareDraw).
        m_ctx->DrawIndexed(m_skinCount, m_skinStart, m_skinBase);
        DrawSunShadow(true, m_skinCount, m_skinStart, m_skinBase);
        return;
    }
    const UINT window = m_skinHi - m_skinLo, elements = n * window;
    // Draws already issued keep the contents they were issued with (the context orders the update
    // after them), and recorded casters have their own copy (DrawSunShadow), so the buffer wraps.
    if (m_skinBufUsed + elements > kInstanceElements) m_skinBufUsed = 0;
    const D3D11_BOX box = { m_skinBufUsed * 16, 0, 0, (m_skinBufUsed + elements) * 16, 1, 1 };
    m_ctx->UpdateSubresource(m_skinBuf.Get(), 0, &box, m_skinConsts.data(), 0, 0);
    const UINT info[4] = { m_skinBufUsed, m_skinLo, window, 0 };
    m_skinBufUsed += elements;
    SetSkinInstanceInfo(info, m_skinSRV.Get());
    ID3D11VertexShader* vs = m_skinVS->instanced.Get();
    m_ctx->VSSetShader(vs, nullptr, 0);
    m_lastVS = vs; // the next draw binds its own shader
    m_ctx->DrawIndexedInstanced(m_skinCount, n, m_skinStart, m_skinBase, 0);
    DrawSunShadow(true, m_skinCount, m_skinStart, m_skinBase, n, info, m_skinConsts.data());
    ++m_skinBatches;
    m_skinBatchedDraws += n;
    if (m_skinStatsFrame + 300 <= m_frame) {
        if (m_skinBatches && m_skinLogged < 40) {
            ++m_skinLogged;
            Log("skin instancing: %u instanced draws, %.1f instances each, %u draws joined (last 300 frames)",
                m_skinBatches, (double)m_skinBatchedDraws / m_skinBatches, m_skinAppended);
        }
        m_skinStatsFrame = (UINT)m_frame;
        m_skinBatches = m_skinBatchedDraws = m_skinAppended = 0;
    }
}
