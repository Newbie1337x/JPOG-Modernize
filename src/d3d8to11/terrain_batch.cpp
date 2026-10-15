// terrain_batch.cpp - the terrain in a few instanced draws ([Performance] TerrainBatching).
//
// TTerrainShaderD3D draws every 16x16 section as four strips (one per 8x8 quarter), each its own
// DrawIndexedPrimitive: thousands of draws once the camera sees the whole island. Between them
// only stream 0 (the section's packed heights), the index buffer (one of a few shared LOD
// patterns) and c4/c5 (the section's position and height scale) change. Those draws are recorded
// here instead of drawn; the first one goes through PrepareDraw as usual and leaves every other
// binding in place. The batch is drawn when anything else happens (FlushBatches) with the
// pulled variant of the terrain shader: SV_InstanceID picks the section record, SV_VertexID its
// index, and the vertices are read from pools that hold a copy of each buffer.
#include "device.h"

namespace {
const UINT kVertexPoolBytes = 16u << 20;
const UINT kIndexPoolCount = 2u << 20;

bool CreateRawPool(ID3D11Device* dev, UINT bytes, ComPtr<ID3D11Buffer>& buf, ComPtr<ID3D11ShaderResourceView>& srv)
{
    D3D11_BUFFER_DESC bd = { bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 };
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &buf))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = bytes / 4;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    return SUCCEEDED(dev->CreateShaderResourceView(buf.Get(), &sd, &srv));
}

// Triangle-list indices of a strip, keeping its winding and dropping the joint triangles.
void AppendStrip(const std::vector<UINT>& strip, std::vector<UINT>& out)
{
    for (size_t i = 0; i + 2 < strip.size(); i++) {
        UINT a = strip[i], b = strip[i + 1], c = strip[i + 2];
        if (a == b || b == c || a == c) continue;
        if (i & 1) std::swap(a, b);
        out.push_back(a); out.push_back(b); out.push_back(c);
    }
}
}

bool Device::CreateBatchResources()
{
    if (m_batchVertexPool) return true;
    D3D11_BUFFER_DESC cb = { 32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if (!CreateRawPool(m_device.Get(), kVertexPoolBytes, m_batchVertexPool, m_batchVertexSRV) ||
        !CreateRawPool(m_device.Get(), kIndexPoolCount * 4, m_batchIndexPool, m_batchIndexSRV) ||
        FAILED(m_device->CreateBuffer(&cb, nullptr, &m_batchCB))) {
        m_batchVertexPool.Reset();
        Log("terrain batching: pool creation failed, disabled");
        m_batchUnavailable = true;
        return false;
    }
    return true;
}

// A copy of the buffer in the vertex pool, made again when the game writes the buffer. The game
// creates the section buffers dynamic; a dynamic buffer can be a copy source (not a destination).
bool Device::PoolVertices(VertexBuffer* vb, UINT& offset)
{
    BufferImpl& b = vb->m_impl;
    if (!b.m_buf) return false;
    auto it = m_batchVertices.find(b.m_identity);
    if (it != m_batchVertices.end() && it->second.revision == b.m_revision) { offset = it->second.offset; return true; }
    const UINT bytes = (b.m_size + 3) & ~3u; // the D3D11 buffer is rounded up to 16 bytes
    if (m_batchVertexUsed + bytes > kVertexPoolBytes) return false; // emptied by the next StartTerrainBatch
    D3D11_BOX box = { 0, 0, 0, bytes, 1, 1 };
    m_ctx->CopySubresourceRegion(m_batchVertexPool.Get(), 0, m_batchVertexUsed, 0, 0, b.m_buf.Get(), 0, &box);
    offset = m_batchVertexUsed;
    m_batchVertices[b.m_identity] = { b.m_revision, offset, bytes };
    m_batchVertexUsed += (bytes + 15) & ~15u;
    return true;
}

// The draw's index range as a triangle list in the index pool (strips are expanded once).
bool Device::PoolIndices(DWORD type, UINT startIndex, UINT primCount, UINT& offset, UINT& count)
{
    BufferImpl& b = m_indices->m_impl;
    const auto key = std::make_tuple(b.m_identity, startIndex, primCount, type);
    auto it = m_batchIndices.find(key);
    if (it != m_batchIndices.end() && it->second.revision == b.m_revision) { offset = it->second.offset; count = it->second.count; return true; }
    const UINT isz = m_indices->m_format == D8FMT_INDEX32 ? 4 : 2;
    const UINT n = type == D8PT_TRIANGLESTRIP ? primCount + 2 : primCount * 3;
    if (((size_t)startIndex + n) * isz > b.m_shadow.size()) return false;
    std::vector<UINT> idx(n), list;
    const BYTE* src = b.m_shadow.data() + (size_t)startIndex * isz;
    for (UINT i = 0; i < n; i++) idx[i] = isz == 4 ? ((const UINT*)src)[i] : ((const WORD*)src)[i];
    if (type == D8PT_TRIANGLESTRIP) { list.reserve(n * 3); AppendStrip(idx, list); }
    else list.swap(idx);
    if (m_batchIndexUsed + list.size() > kIndexPoolCount) return false;
    if (!list.empty()) {
        D3D11_BOX box = { m_batchIndexUsed * 4, 0, 0, (m_batchIndexUsed + (UINT)list.size()) * 4, 1, 1 };
        m_ctx->UpdateSubresource(m_batchIndexPool.Get(), 0, &box, list.data(), 0, 0);
    }
    offset = m_batchIndexUsed;
    count = (UINT)list.size();
    m_batchIndices[key] = { b.m_revision, offset, count };
    m_batchIndexUsed += count;
    return true;
}

bool Device::RecordTerrainSection(DWORD type, UINT startIndex, UINT primCount)
{
    VertexBuffer* vb = m_streams[0].vb;
    if (!vb || m_streams[0].stride != m_terrainStride0) return false;
    BatchSection s;
    if (!PoolVertices(vb, s.vertexBase) || !PoolIndices(type, startIndex, primCount, s.indexBase, s.indexCount)) return false;
    if (!s.indexCount) return true; // only joint triangles: nothing to draw
    memcpy(s.r4, m_vsConst[4], 16);
    memcpy(s.r5, m_vsConst[5], 16);
    s.baseVertex = (INT)m_baseVertex;
    m_terrainSections.push_back(s);
    if (s.indexCount > m_terrainMaxIndices) m_terrainMaxIndices = s.indexCount;
    return true;
}

// After PrepareDraw of a terrain draw: record it instead of drawing it, when the batch can take it.
bool Device::StartTerrainBatch(DWORD type, UINT startIndex, UINT primCount)
{
    if (!g_config.terrainBatching || m_batchUnavailable || m_worldKind != 2) return false;
    auto reject = [&](const char* why) {
        LogOnce(std::string("batch-reject-") + why, "terrain batching: VS 0x%lx not batched (%s)", m_vsHandle, why);
        return false;
    };
    if (type != D8PT_TRIANGLESTRIP && type != D8PT_TRIANGLELIST) return reject("primitive type");
    if (m_worldCaster || m_tessCandidate || m_waterDraw) return reject("shadow caster, tessellated or water");
    VertexShader8* vs = m_vertexShaders[m_vsHandle >> 1];
    if (vs->pulledState < 0) return false;
    const UINT streams = vs->parsed.streamMask;
    if (!(streams & 1) || (streams & ~3u) || vs->constRegs >= 256) { vs->pulledState = -1; return false; }
    if (!vs->pulledState) {
        std::string error;
        const std::string hlsl = TranslateVS(vs->func.data(), vs->parsed, error, true);
        char name[64];
        sprintf_s(name, "vs_%04lx_batch", m_vsHandle);
        ComPtr<ID3DBlob> blob = hlsl.empty() ? nullptr : CompileHLSL(hlsl, "vs_5_0", name);
        const bool ok = blob && m_device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 &&
            SUCCEEDED(m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs->pulled));
        vs->pulledState = ok ? 1 : -1;
        Log("terrain batching: VS 0x%lx %s", m_vsHandle, ok ? "batched" : (error.empty() ? "variant failed" : error.c_str()));
        if (!ok) return false;
    }
    if (!CreateBatchResources()) return false;
    // Pools are emptied only here, between batches: recorded sections point into them. The
    // copies already queued are ordered before anything written later.
    if (m_batchVertexUsed > kVertexPoolBytes - (1u << 20) || m_batchVertices.size() > 65536) {
        m_batchVertices.clear(); m_batchVertexUsed = 0;
    }
    if (m_batchIndexUsed > kIndexPoolCount - (64u << 10) || m_batchIndices.size() > 65536) {
        m_batchIndices.clear(); m_batchIndexUsed = 0;
    }
    VertexBuffer* s1 = (streams & 2) ? m_streams[1].vb : nullptr;
    if ((streams & 2) && (!s1 || !PoolVertices(s1, m_terrainStream1[0]))) return reject("stream 1");
    m_terrainStream1[1] = (streams & 2) ? m_streams[1].stride : 0;
    m_terrainVS = vs;
    m_terrainType = type;
    m_terrainStride0 = m_streams[0].stride;
    m_terrainMaxIndices = 0;
    m_terrainSections.clear();
    if (!RecordTerrainSection(type, startIndex, primCount)) return reject("section buffers");
    m_terrainBatchOpen = true;
    return true;
}

// A later terrain draw: the batch is still open, so nothing but stream 0, the indices and c4/c5
// changed since the first one.
bool Device::AppendTerrainBatch(DWORD type, UINT startIndex, UINT primCount)
{
    if (type != m_terrainType || !g_config.terrainBatching) return false;
    return RecordTerrainSection(type, startIndex, primCount);
}

void Device::DrawTerrainBatch()
{
    m_terrainBatchOpen = false;
    const UINT n = (UINT)m_terrainSections.size();
    if (!n || !m_terrainMaxIndices) return;
    if (n > m_batchSectionCapacity) {
        UINT cap = 1024;
        while (cap < n) cap *= 2;
        m_batchSectionBuf.Reset(); m_batchSectionSRV.Reset();
        D3D11_BUFFER_DESC bd = { cap * (UINT)sizeof(BatchSection), D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                                 D3D11_CPU_ACCESS_WRITE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, (UINT)sizeof(BatchSection) };
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = cap;
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_batchSectionBuf)) ||
            FAILED(m_device->CreateShaderResourceView(m_batchSectionBuf.Get(), &sd, &m_batchSectionSRV))) {
            Log("terrain batching: section buffer (%u) failed, disabled", cap);
            m_batchUnavailable = true;
            m_batchSectionCapacity = 0;
            m_terrainSections.clear();
            return;
        }
        m_batchSectionCapacity = cap;
    }
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(m_batchSectionBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { m_terrainSections.clear(); return; }
    memcpy(map.pData, m_terrainSections.data(), n * sizeof(BatchSection));
    m_ctx->Unmap(m_batchSectionBuf.Get(), 0);
    const UINT batch[8] = { m_terrainStride0, m_terrainStream1[0], m_terrainStream1[1], 0, 0, 0, 0, 0 };
    if (SUCCEEDED(m_ctx->Map(m_batchCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, batch, sizeof(batch));
        m_ctx->Unmap(m_batchCB.Get(), 0);
    }
    ID3D11ShaderResourceView* views[3] = { m_batchSectionSRV.Get(), m_batchIndexSRV.Get(), m_batchVertexSRV.Get() };
    m_ctx->VSSetShaderResources(20, 3, views);
    m_ctx->VSSetConstantBuffers(6, 1, m_batchCB.GetAddressOf());
    m_ctx->VSSetShader(m_terrainVS->pulled.Get(), nullptr, 0);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->DrawInstanced(m_terrainMaxIndices, n, 0, 0);
    // The next draw binds its own shader and layout; every draw sets its topology.
    m_lastVS = m_terrainVS->pulled.Get();
    m_lastLayout = nullptr;
    TraceSun("Terrain batch: %u sections, up to %u indices", n, m_terrainMaxIndices);
    ++m_batchDraws;
    m_batchSectionCount += n;
    if (n == 1) ++m_batchSingles;
    if (m_batchStatsFrame + 300 <= m_frame) {
        if (m_batchDraws && m_batchDrawsLogged < 40) {
            ++m_batchDrawsLogged;
            Log("terrain batching: %u batches, %.1f sections each, %u with one section (last 300 frames)",
                m_batchDraws, (double)m_batchSectionCount / m_batchDraws, m_batchSingles);
        }
        m_batchStatsFrame = (UINT)m_frame;
        m_batchDraws = m_batchSectionCount = m_batchSingles = 0;
    }
    m_terrainSections.clear();
}
