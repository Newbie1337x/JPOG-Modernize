#include "device.h"
#include <algorithm>
bool Device::PickedDraw() const {
    if(m_pickSkin<0 || !m_worldDraw || m_worldKind!=1 || m_sun.flags[1]>.5f || !m_textures[0] ||
       m_textures[0]->GetType()!=D8RTYPE_TEXTURE)return false;
    Texture* t=static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[0]));
    return (size_t)m_pickSkin<m_skinList.size() && m_skinList[m_pickSkin]==t;
}
int Device::MaterialTextureStage() const {
    if(PickedDraw())return 0;
    for(int stage=0;stage<2;stage++) {
        if(!m_textures[stage] || m_textures[stage]->GetType()!=D8RTYPE_TEXTURE)continue;
        if(!static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[stage]))->m_materialProfile)continue;
        if(stage==0)return 0;
        // Adult/juvenile skins are blended from t0 and t1. Never use an unused,
        // stale stage-1 binding from another draw (including the HUD).
        if(m_psHandle && m_psHandle<=m_pixelShaders.size() && m_pixelShaders[m_psHandle-1] &&
           m_pixelShaders[m_psHandle-1]->samplesStage1)return stage;
    }
    return -1;
}
int Device::ActiveMaterial() const {
    if(!g_config.effectsEnabled)return 0;
    if(PickedDraw())return 1;
    if(!g_config.effectsEnabled || !g_config.materialsEnabled || m_materialFailed || !m_worldDraw ||
       m_sun.flags[1]>.5f || (m_worldKind!=1 && m_worldKind!=3) ||
       MaterialTextureStage()<0)return 0;
    int profile=static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[MaterialTextureStage()]))->m_materialProfile;
    if(profile && m_psHandle && m_psHandle<=m_pixelShaders.size() && m_pixelShaders[m_psHandle-1] &&
       m_pixelShaders[m_psHandle-1]->water)return 0;
    return profile;
}
bool Device::PrepareMaterial(){
    int profile=ActiveMaterial();if(!profile)return true;
    if(!m_materialCB){D3D11_BUFFER_DESC d={32,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};
        if(FAILED(m_device->CreateBuffer(&d,nullptr,&m_materialCB))){m_materialFailed=true;Log("Material CB creation failed; pilot disabled");return false;}}
    float cb[8]={profile==1?g_config.materialSkinRoughness:g_config.materialBuildingRoughness,
        g_config.materialSpecular,g_config.materialEnvironment,g_config.materialNormalStrength,
        .04f,(m_materialDebug||PickedDraw())?1.f:0.f,PickedDraw()?1.f:0.f,float(profile)};
    D3D11_MAPPED_SUBRESOURCE map;
    if(FAILED(m_ctx->Map(m_materialCB.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map))){m_materialFailed=true;Log("Material CB mapping failed; pilot disabled");return false;}
    memcpy(map.pData,cb,sizeof(cb));m_ctx->Unmap(m_materialCB.Get(),0);
    BindPSCB(3,1,m_materialCB.GetAddressOf());
    ++m_materialDraws[profile];
    LogOnce("material-stage-"+std::to_string(profile)+"-"+std::to_string(MaterialTextureStage()),"GGX material pilot active: profile %d stage %d, VS 0x%lx PS 0x%lx",profile,MaterialTextureStage(),m_vsHandle,m_psHandle);
    return true;
}
// Skin finder (Insert panel): distinct stage-0 textures of skinned world draws, in draw order.
// The panel reads last frame's list: hold references so a texture freed by the game stays valid.
void Device::TrackSkinTexture(){
    if(m_skinListFrame!=m_frame){m_skinListFrame=m_frame;for(Texture* t:m_skinListPrev)t->Release();m_skinListPrev.swap(m_skinList);m_skinList.clear();}
    if(m_worldKind==1 && m_worldDraw && m_sun.flags[1]<.5f && m_textures[0] && m_textures[0]->GetType()==D8RTYPE_TEXTURE){
        Texture* t=static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[0]));
        if(std::find(m_skinList.begin(),m_skinList.end(),t)==m_skinList.end()){t->AddRef();m_skinList.push_back(t);}
    }
}
