// common.cpp - logging, configuration and format conversion.
#include "common.h"
#include <stdio.h>
#include <share.h>
#include <stdarg.h>
#include <set>
#include <mutex>

Config g_config;
std::string g_gameDir;

static FILE* g_log = nullptr;
static std::mutex g_logMutex;
static std::set<std::string> g_logOnce;

void LogInit(const std::string& path)
{
    g_log = _fsopen(path.c_str(), "w", _SH_DENYNO);
}

static void LogV(const char* fmt, va_list ap)
{
    if (!g_log) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
}

void Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void LogOnce(const std::string& key, const char* fmt, ...)
{
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (!g_logOnce.insert(key).second) return;
    }
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void LoadConfig()
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    g_gameDir = exe;
    const std::string ini = g_gameDir + "d3d8to11.ini";
    char buf[64];
    g_config.effectsEnabled=GetPrivateProfileIntA("Renderer","EffectsEnabled",1,ini.c_str())!=0;
    GetPrivateProfileStringA("Renderer", "Backend", "d3d11", buf, sizeof(buf), ini.c_str());
    g_config.useDgVoodoo = _stricmp(buf, "dgvoodoo") == 0;
    g_config.syncInterval = GetPrivateProfileIntA("Renderer", "VSync", 0, ini.c_str()) ? 1 : 0;
    g_config.fpsLimit = GetPrivateProfileIntA("Renderer", "FPSLimit", 0, ini.c_str());
    g_config.forceAnisotropy = GetPrivateProfileIntA("Renderer", "Anisotropy", 16, ini.c_str());
    g_config.applyGamma = GetPrivateProfileIntA("Renderer", "ApplyGammaRamp", 1, ini.c_str()) != 0;
    g_config.msaa = GetPrivateProfileIntA("Renderer", "MSAA", 8, ini.c_str());
    g_config.alphaToCoverage = GetPrivateProfileIntA("Renderer", "AlphaToCoverage", 0, ini.c_str()) != 0;
    {
        char value[64];
        GetPrivateProfileStringA("Renderer", "FoliageMipAlpha", "0.25", value, sizeof(value), ini.c_str());
        g_config.foliageMipAlpha = (float)atof(value);
    }
    if (!(g_config.foliageMipAlpha >= 0.0f && g_config.foliageMipAlpha <= 1.0f)) g_config.foliageMipAlpha = 0.25f;
    g_config.debugLayer = GetPrivateProfileIntA("Debug", "D3D11DebugLayer", 0, ini.c_str()) != 0;
    g_config.dumpShaders = GetPrivateProfileIntA("Debug", "DumpShaders", 0, ini.c_str()) != 0;
    g_config.sunLighting = GetPrivateProfileIntA("Lighting", "Enabled", 1, ini.c_str()) != 0;
    g_config.replaceLighting = GetPrivateProfileIntA("Lighting", "ReplaceOriginal", 0, ini.c_str()) != 0;
    g_config.grassShadows = GetPrivateProfileIntA("Lighting", "GrassShadows", 0, ini.c_str()) != 0;
    g_config.terrainShadows = GetPrivateProfileIntA("Lighting", "TerrainShadows", 0, ini.c_str()) != 0;
    g_config.lightingDebug = GetPrivateProfileIntA("Lighting", "DebugView", 0, ini.c_str());
    GetPrivateProfileStringA("Lighting", "SunSource", "game", buf, sizeof(buf), ini.c_str());
    g_config.sunFromGame = _stricmp(buf, "fixed") != 0;
    g_config.shadowResolution = GetPrivateProfileIntA("Lighting", "ShadowResolution", 4096, ini.c_str());
    if (g_config.shadowResolution != 1024 && g_config.shadowResolution != 2048 &&
        g_config.shadowResolution != 4096 && g_config.shadowResolution != 8192)
        g_config.shadowResolution = 4096;
    auto readFloat = [&](const char* key, float fallback, float lo, float hi) {
        char value[64];
        GetPrivateProfileStringA("Lighting", key, "", value, sizeof(value), ini.c_str());
        float f = value[0] ? (float)atof(value) : fallback;
        return f >= lo && f <= hi ? f : fallback;
    };
    g_config.shadowDistance = readFloat("ShadowDistance", 250, 32, 16384);
    g_config.shadowCache = GetPrivateProfileIntA("Lighting", "ShadowCache", 1, ini.c_str()) != 0;
    g_config.shadowCacheFrames = (std::max)(1, (std::min)(16, (int)GetPrivateProfileIntA("Lighting", "ShadowCacheFrames", 4, ini.c_str())));
    g_config.shadowCacheNear = readFloat("ShadowCacheNear", 100, 0, 16384);
    g_config.sunAmbient = readFloat("Ambient", 0.60f, 0.1f, 2);
    g_config.sunDirect = readFloat("SunIntensity", 0.55f, 0, 2);
    g_config.relightSun = readFloat("RelightSun", 1.35f, 0, 4);
    g_config.relightAmbient = readFloat("RelightAmbient", .85f, 0, 4);
    g_config.relightSkyTint = readFloat("RelightSkyTint", .65f, 0, 1);
    g_config.relightExposure = readFloat("RelightExposure", .95f, 0.25f, 4);
    g_config.relightTonemap = GetPrivateProfileIntA("Lighting", "RelightTonemap", 1, ini.c_str()) != 0;
    g_config.heightfieldLighting = GetPrivateProfileIntA("Lighting", "HeightfieldLighting", 1, ini.c_str()) != 0;
    g_config.horizonShadows = readFloat("HorizonShadows", 1, 0, 1);
    g_config.rimLight = readFloat("RimLight", 0.5f, 0, 2);
    g_config.grassSaturation = readFloat("GrassSaturation", 0.8f, 0, 2);
    g_config.relightGrass = GetPrivateProfileIntA("Lighting", "RelightGrass", 1, ini.c_str()) != 0;
    g_config.emissive = GetPrivateProfileIntA("Lighting", "Emissive", 1, ini.c_str()) != 0;
    g_config.emissiveIntensity = readFloat("EmissiveIntensity", 1.0f, 0, 8);
    g_config.pointLights = GetPrivateProfileIntA("Lighting", "PointLights", 1, ini.c_str()) != 0;
    g_config.pointLightIntensity = readFloat("PointLightIntensity", 1.5f, 0, 16);
    g_config.pointLightRadius = readFloat("PointLightRadius", 8.0f, 0.5f, 64);
    g_config.pointLightMerge = readFloat("PointLightMerge", 4.0f, 0, 32);
    g_config.pointLightWarmth = readFloat("PointLightWarmth", 0.75f, 0, 1);
    g_config.pointLightDebug = (std::max)(0, (std::min)(2, (int)GetPrivateProfileIntA("Lighting", "PointLightDebug", 0, ini.c_str())));
    {
        char v[64] = {};
        GetPrivateProfileStringA("Lighting", "PointLightTint", "", v, sizeof(v), ini.c_str());
        float r, g, b;
        if (sscanf_s(v, "%f , %f , %f", &r, &g, &b) == 3) { g_config.pointLightTint[0] = r; g_config.pointLightTint[1] = g; g_config.pointLightTint[2] = b; }
    }
    g_config.bloom = GetPrivateProfileIntA("Lighting", "Bloom", 1, ini.c_str()) != 0;
    g_config.bloomStrength = readFloat("BloomStrength", 0.6f, 0, 4);
    g_config.bloomThreshold = readFloat("BloomThreshold", 0.75f, 0, 1);
    g_config.bloomDay = readFloat("BloomDay", 0.25f, 0, 1);
    g_config.specular = readFloat("Specular", 0.0f, 0, 4);
    g_config.specRoughness = readFloat("SpecularRoughness", 0.6f, 0.1f, 1);
    g_config.filmic = GetPrivateProfileIntA("Lighting", "Filmic", 0, ini.c_str()) != 0;
    g_config.filmicExposure = readFloat("FilmicExposure", 1.6f, 0.2f, 4);
    g_config.grassBrightness = readFloat("GrassBrightness", 0.92f, 0, 2);
    auto readColor = [&](const char* key, float* c) {
        char value[96];
        GetPrivateProfileStringA("TimeOfDay", key, "", value, sizeof(value), ini.c_str());
        float r, g, b;
        if (sscanf_s(value, "%f , %f , %f", &r, &g, &b) == 3 && r >= 0 && g >= 0 && b >= 0 && r <= 4 && g <= 4 && b <= 4) { c[0] = r; c[1] = g; c[2] = b; }
    };
    auto readTod = [&](const char* key, float fallback, float lo, float hi) {
        char value[64];
        GetPrivateProfileStringA("TimeOfDay", key, "", value, sizeof(value), ini.c_str());
        float f = value[0] ? (float)atof(value) : fallback;
        return f >= lo && f <= hi ? f : fallback;
    };
    readColor("MoonColor", g_config.moonColor);
    readColor("NightAmbient", g_config.nightAmbient);
    readColor("DuskAmbient", g_config.duskAmbient);
    readColor("LowSunColor", g_config.lowSunColor);
    readColor("NightTint", g_config.nightTint);
    g_config.moonIntensity = readTod("MoonIntensity", g_config.moonIntensity, 0, 3);
    g_config.moonRim = readTod("MoonRim", g_config.moonRim, 0, 8);
    g_config.moonRimWidth = readTod("MoonRimWidth", g_config.moonRimWidth, 0.05f, 1);
    g_config.moonRimGlow = readTod("MoonRimGlow", g_config.moonRimGlow, 0, 2);
    g_config.maxExposure = readTod("MaxExposure", g_config.maxExposure, 1, 6);
    g_config.nightSky = readTod("NightSky", g_config.nightSky, 0, 4);
    g_config.nightExposure = readTod("NightExposure", g_config.nightExposure, 0.1f, 2);
    g_config.nightDesaturate = readTod("NightDesaturate", g_config.nightDesaturate, 0, 1);
    g_config.stars = readTod("Stars", g_config.stars, 0, 4);
    g_config.moonSheen = readTod("MoonSheen", g_config.moonSheen, 0, 4);
    g_config.grassMoon = readTod("GrassMoon", g_config.grassMoon, 0, 4);
    g_config.nightSurface = readTod("NightSurface", g_config.nightSurface, 0, 1);
    g_config.proceduralSky = GetPrivateProfileIntA("Lighting", "ProceduralSky", 1, ini.c_str()) != 0;
    g_config.timeOfDay = GetPrivateProfileIntA("TimeOfDay", "Preview", g_config.timeOfDay ? 1 : 0, ini.c_str()) != 0;
    g_config.timeOfDayHour = readTod("Hour", g_config.timeOfDayHour, 0, 24);
    g_config.bakedTerrainDetail = readFloat("BakedTerrainDetail", 1, 0, 1);
    g_config.shadowStrength = readFloat("ShadowStrength", 0.25f, 0, 1);
    g_config.sunMinElevation = readFloat("SunMinHeight", 0.35f, 0.05f, 1);
    g_config.sunDirection[0] = readFloat("SunX", -0.35f, -1, 1);
    g_config.sunDirection[1] = readFloat("SunY", 0.82f, 0.05f, 1);
    g_config.sunDirection[2] = readFloat("SunZ", -0.45f, -1, 1);
    auto readPost = [&](const char* key, float fallback, float lo, float hi) {
        char value[64];
        GetPrivateProfileStringA("Post", key, "", value, sizeof(value), ini.c_str());
        float f = value[0] ? (float)atof(value) : fallback;
        return f >= lo && f <= hi ? f : fallback;
    };
    g_config.postEnabled = GetPrivateProfileIntA("Post", "Enabled", 1, ini.c_str()) != 0;
    g_config.sunBoost = readPost("SunBoost", 0.25f, 0, 1);
    g_config.shadowCool = readPost("ShadowCool", 0.5f, 0, 1);
    g_config.exposure = readPost("Exposure", 1.0f, 0.5f, 2);
    g_config.contrast = readPost("Contrast", 0.97f, 0.5f, 1.5f);
    g_config.saturation = readPost("Saturation", 1.09f, 0, 2);
    g_config.warmth = readPost("Warmth", 0.23f, -1, 1);
    auto readGrass = [&](const char* key, float fallback, float lo, float hi) {
        char value[64];
        GetPrivateProfileStringA("Grass", key, "", value, sizeof(value), ini.c_str());
        float f = value[0] ? (float)atof(value) : fallback;
        return f >= lo && f <= hi ? f : fallback;
    };
    g_config.aoEnabled = GetPrivateProfileIntA("Effects", "AO", 1, ini.c_str()) != 0;
    g_config.shafts = GetPrivateProfileIntA("Effects", "LightShafts", 1, ini.c_str()) != 0;
    {
        auto readFx = [&](const char* key, float fallback, float lo, float hi) {
            char value[64];
            GetPrivateProfileStringA("Effects", key, "", value, sizeof(value), ini.c_str());
            float f = value[0] ? (float)atof(value) : fallback;
            return f >= lo && f <= hi ? f : fallback;
        };
        g_config.aoRadius = readFx("AORadius", 2.0f, 0.2f, 20);
        g_config.aoStrength = readFx("AOStrength", 0.7f, 0, 1);
        g_config.shaftStrength = readFx("ShaftStrength", 0.35f, 0, 3);
        g_config.shaftScatter = readFx("ShaftScatter", 0.6f, 0, 0.95f);
    }
    g_config.normalMaps = GetPrivateProfileIntA("NormalMaps", "Enabled", 1, ini.c_str()) != 0;
    g_config.normalFlipGreen = GetPrivateProfileIntA("NormalMaps", "FlipGreen", 0, ini.c_str()) != 0;
    GetPrivateProfileStringA("NormalMaps", "Strength", "1.5", buf, sizeof(buf), ini.c_str());
    g_config.normalStrength = (float)atof(buf);
    g_config.materialsEnabled=GetPrivateProfileIntA("Materials","Enabled",1,ini.c_str())!=0;
    auto materialFloat=[&](const char* key,float fallback,float lo,float hi){char value[64],def[32];
        sprintf_s(def,"%.3f",fallback);GetPrivateProfileStringA("Materials",key,def,value,sizeof(value),ini.c_str());
        float f=float(atof(value));return f>=lo && f<=hi?f:fallback;};
    g_config.materialSkinRoughness=materialFloat("SkinRoughness",.58f,.1f,1);
    g_config.materialBuildingRoughness=materialFloat("BuildingRoughness",.78f,.1f,1);
    g_config.materialSpecular=materialFloat("Specular",.85f,0,2);
    g_config.materialEnvironment=materialFloat("Environment",.25f,0,1);
    g_config.materialNormalStrength=materialFloat("NormalStrength",.65f,0,2);
    if (!(g_config.normalStrength >= 0 && g_config.normalStrength <= 3)) g_config.normalStrength = 1.5f;
    g_config.waterEnabled=GetPrivateProfileIntA("Water","Enabled",1,ini.c_str())!=0;
    auto readWater=[&](const char* key,float fallback,float lo,float hi) {
        char value[64]; GetPrivateProfileStringA("Water",key,"",value,sizeof(value),ini.c_str());
        float f=value[0]?(float)atof(value):fallback;
        return f>=lo && f<=hi ? f:fallback;
    };
    g_config.waterDepthScale=readWater("DepthScale",6,0.1f,100);
    g_config.waterTransparency=readWater("Transparency",0.8f,0,1);
    g_config.waterReflection=readWater("Reflection",0.85f,0,1);
    g_config.waterFoam=readWater("Foam",0.35f,0,1);
    g_config.waterFoamWidth=readWater("FoamWidth",0.65f,0.05f,10);
    g_config.waterDistortion=readWater("Distortion",2,0,20);
    g_config.waterRipples=readWater("Ripples",1,0,3);
    g_config.waterSpeed=readWater("Speed",1,0,3);
    g_config.waterInteraction=GetPrivateProfileIntA("Water","Interaction",1,ini.c_str())!=0;
    g_config.waterSSR=GetPrivateProfileIntA("Water","SceneReflections",1,ini.c_str())!=0;
    g_config.waterSurfaceMode=GetPrivateProfileIntA("Water","SurfaceMode",0,ini.c_str());
    if(g_config.waterSurfaceMode<0 || g_config.waterSurfaceMode>2)g_config.waterSurfaceMode=0;
    g_config.waterLagoonFoam=readWater("LagoonFoam",.03f,0,1);
    g_config.waterLagoonRipples=readWater("LagoonRipples",.25f,0,2);
    g_config.waterOceanRipples=readWater("OceanRipples",1.4f,0,3);
    g_config.waterSwellAmplitude=readWater("SwellAmplitude",.65f,0,2);
    g_config.waterSwellLength=readWater("SwellLength",1.f,.5f,4);
    g_config.waterSwellCoastDepth=readWater("SwellCoastDepth",3.f,.5f,10);
    g_config.waterCrestFoam=readWater("CrestFoam",.35f,0,1);
    g_config.waterSSRStrength=readWater("SceneReflectionStrength",1,0,1);
    g_config.waterSSRDistance=readWater("SceneReflectionDistance",80,5,250);
    g_config.waterSSRSteps=GetPrivateProfileIntA("Water","SceneReflectionSteps",24,ini.c_str());
    if(g_config.waterSSRSteps<8)g_config.waterSSRSteps=8;
    if(g_config.waterSSRSteps>48)g_config.waterSSRSteps=48;
    g_config.performanceLog=GetPrivateProfileIntA("Performance","LogCSV",1,ini.c_str())!=0;
    g_config.performanceGPU=GetPrivateProfileIntA("Performance","MeasureGPU",1,ini.c_str())!=0;
    g_config.constantRing=GetPrivateProfileIntA("Performance","ConstantRing",1,ini.c_str())!=0;
    g_config.terrainBatching=GetPrivateProfileIntA("Performance","TerrainBatching",1,ini.c_str())!=0;
    g_config.hitchSampler=GetPrivateProfileIntA("Performance","HitchSampler",0,ini.c_str())!=0;
    g_config.hitchThresholdMs=GetPrivateProfileIntA("Performance","HitchThresholdMs",60,ini.c_str());
    if(g_config.hitchThresholdMs<5)g_config.hitchThresholdMs=5;
    g_config.hitchProfileSeconds=GetPrivateProfileIntA("Performance","HitchProfileSeconds",0,ini.c_str());
    if(g_config.hitchProfileSeconds<0)g_config.hitchProfileSeconds=0;
    g_config.asyncShaders=GetPrivateProfileIntA("Performance","AsyncShaders",1,ini.c_str())!=0;
    g_config.prewarmShaders=GetPrivateProfileIntA("Performance","PrewarmShaders",1,ini.c_str())!=0;
    g_config.skinInstancing=GetPrivateProfileIntA("Performance","SkinInstancing",0,ini.c_str())!=0;
    g_config.threaded=GetPrivateProfileIntA("Performance","Threaded",0,ini.c_str())!=0;
    g_config.taa=GetPrivateProfileIntA("Antialiasing","TAA",1,ini.c_str())!=0;
    g_config.fsr31=GetPrivateProfileIntA("Antialiasing","FSR31",1,ini.c_str())!=0;
    g_config.objectMotion=GetPrivateProfileIntA("Antialiasing","ObjectMotion",1,ini.c_str())!=0;
    g_config.frameGeneration=GetPrivateProfileIntA("Antialiasing","FrameGeneration",0,ini.c_str())!=0;
    g_config.frameGenerationDebug=GetPrivateProfileIntA("Antialiasing","FrameGenerationDebug",0,ini.c_str())!=0;
    g_config.fsr1=GetPrivateProfileIntA("Antialiasing","FSR1",1,ini.c_str())!=0;
    char aaValue[64];GetPrivateProfileStringA("Antialiasing","HistoryWeight","0.72",aaValue,sizeof(aaValue),ini.c_str());
    g_config.taaHistory=(float)atof(aaValue);
    if(!(g_config.taaHistory>=0 && g_config.taaHistory<=.9f))g_config.taaHistory=.72f;
    GetPrivateProfileStringA("Antialiasing","Sharpness","0.5",aaValue,sizeof(aaValue),ini.c_str());
    g_config.fsrSharpness=(float)atof(aaValue);
    if(!(g_config.fsrSharpness>=0 && g_config.fsrSharpness<=1))g_config.fsrSharpness=.5f;
    g_config.waterInteractionStrength=readWater("InteractionStrength",1,0,3);
    g_config.waterDebug=GetPrivateProfileIntA("Water","DebugView",0,ini.c_str());
    auto readSectionFloat=[&](const char* section,const char* key,float fallback,float lo,float hi){char value[64];
        GetPrivateProfileStringA(section,key,"",value,sizeof(value),ini.c_str());
        float f=value[0]?(float)atof(value):fallback;return f>=lo && f<=hi?f:fallback;};
    g_config.tessellation=GetPrivateProfileIntA("Tessellation","Enabled",1,ini.c_str())!=0;
    g_config.tessDinosOnly=GetPrivateProfileIntA("Tessellation","DinosOnly",1,ini.c_str())!=0;
    g_config.tessMaxFactor=readSectionFloat("Tessellation","MaxFactor",8,1,16);
    g_config.tessPixels=readSectionFloat("Tessellation","PixelsPerEdge",10,2,64);
    g_config.tessCurvature=readSectionFloat("Tessellation","Curvature",1,0,1.5f);
    g_config.tessFlipWinding=GetPrivateProfileIntA("Tessellation","FlipWinding",0,ini.c_str())!=0;
    g_config.tessDisplacement=readSectionFloat("Tessellation","Displacement",.03f,0,.5f);
    g_config.tessDetailMip=readSectionFloat("Tessellation","DetailMip",0,0,4);
    g_config.tessInvertRelief=GetPrivateProfileIntA("Tessellation","InvertRelief",0,ini.c_str())!=0;
    g_config.tessSeamSafe=GetPrivateProfileIntA("Tessellation","SeamSafe",1,ini.c_str())!=0;
    g_config.tessWeld=GetPrivateProfileIntA("Tessellation","WeldSeams",1,ini.c_str())!=0;
    g_config.reliefEnabled=GetPrivateProfileIntA("Relief","Enabled",1,ini.c_str())!=0;
    g_config.reliefDepth=readSectionFloat("Relief","Depth",3,.5f,12);
    g_config.reliefParallax=readSectionFloat("Relief","Parallax",.5f,0,1);
    g_config.reliefShadow=readSectionFloat("Relief","SelfShadow",.6f,0,1);
    g_config.reliefSoftness=readSectionFloat("Relief","Softness",1,.25f,4);
    g_config.reliefInvert=GetPrivateProfileIntA("Relief","Invert",0,ini.c_str())!=0;
    g_config.grassPhysics = GetPrivateProfileIntA("Grass", "Enabled", 1, ini.c_str()) != 0;
    g_config.grassPush = readGrass("Push", 0.8f, 0, 3);
    g_config.grassFlatten = readGrass("Flatten", 0.5f, 0, 1);
    g_config.grassRadius = readGrass("RadiusScale", 1.0f, 0.2f, 4);
    g_config.grassMinBones = GetPrivateProfileIntA("Grass", "MinBones", 6, ini.c_str());
    g_config.grassThin = GetPrivateProfileIntA("Grass", "Thin", 1, ini.c_str()) != 0;
    g_config.grassThinStart = readGrass("ThinStart", 0.45f, 0, 0.95f);
    g_config.grassThinKeep = readGrass("ThinKeep", 0.35f, 0.05f, 1);
    g_config.grassThinFill = readGrass("ThinFill", 0.6f, 0, 1);
    g_config.overlayKey = GetPrivateProfileIntA("Overlay", "Key", VK_INSERT, ini.c_str());
    g_config.showFps = GetPrivateProfileIntA("Overlay", "ShowFPS", 0, ini.c_str()) != 0;
    GetPrivateProfileStringA("Overlay", "Scale", "1.0", buf, sizeof(buf), ini.c_str());
    g_config.overlayScale = (float)atof(buf);
    if (!(g_config.overlayScale >= 0.5f && g_config.overlayScale <= 3.0f)) g_config.overlayScale = 1.0f;
}

void SaveConfig()
{
    const std::string ini = g_gameDir + "d3d8to11.ini";
    auto put = [&](const char* section, const char* key, const char* value) {
        WritePrivateProfileStringA(section, key, value, ini.c_str());
    };
    auto putInt = [&](const char* section, const char* key, int v) { put(section, key, std::to_string(v).c_str()); };
    auto putFloat = [&](const char* section, const char* key, float v) {
        char b[32];
        sprintf_s(b, "%.3f", v);
        put(section, key, b);
    };
    putInt("Renderer", "VSync", g_config.syncInterval);
    putInt("Renderer", "EffectsEnabled", g_config.effectsEnabled);
    putInt("Renderer", "FPSLimit", g_config.fpsLimit);
    putInt("Renderer", "Anisotropy", g_config.forceAnisotropy);
    putInt("Renderer", "ApplyGammaRamp", g_config.applyGamma);
    putInt("Renderer", "MSAA", g_config.msaa);
    putInt("Renderer", "AlphaToCoverage", g_config.alphaToCoverage);
    putFloat("Renderer", "FoliageMipAlpha", g_config.foliageMipAlpha);
    putInt("Lighting", "Enabled", g_config.sunLighting);
    putInt("Lighting", "ShadowResolution", g_config.shadowResolution);
    putFloat("Lighting", "ShadowDistance", g_config.shadowDistance);
    putInt("Lighting", "ShadowCache", g_config.shadowCache);
    putInt("Lighting", "ShadowCacheFrames", g_config.shadowCacheFrames);
    putFloat("Lighting", "ShadowCacheNear", g_config.shadowCacheNear);
    putFloat("Lighting", "ShadowStrength", g_config.shadowStrength);
    put("Lighting", "SunSource", g_config.sunFromGame ? "game" : "fixed");
    putFloat("Lighting", "SunX", g_config.sunDirection[0]);
    putFloat("Lighting", "SunY", g_config.sunDirection[1]);
    putFloat("Lighting", "SunZ", g_config.sunDirection[2]);
    putFloat("Lighting", "SunMinHeight", g_config.sunMinElevation);
    putInt("Lighting", "GrassShadows", g_config.grassShadows);
    putInt("Lighting", "TerrainShadows", g_config.terrainShadows);
    putInt("Lighting", "DebugView", g_config.lightingDebug);
    putInt("Lighting", "ReplaceOriginal", g_config.replaceLighting);
    putFloat("Lighting", "RelightSun", g_config.relightSun);
    putFloat("Lighting", "RelightAmbient", g_config.relightAmbient);
    putFloat("Lighting", "RelightSkyTint", g_config.relightSkyTint);
    putFloat("Lighting", "RelightExposure", g_config.relightExposure);
    putInt("Lighting", "RelightTonemap", g_config.relightTonemap);
    putInt("Lighting", "HeightfieldLighting", g_config.heightfieldLighting);
    putFloat("Lighting", "HorizonShadows", g_config.horizonShadows);
    putFloat("Lighting", "RimLight", g_config.rimLight);
    putFloat("Lighting", "GrassSaturation", g_config.grassSaturation);
    putInt("Lighting", "RelightGrass", g_config.relightGrass);
    putInt("Lighting", "Emissive", g_config.emissive);
    putFloat("Lighting", "EmissiveIntensity", g_config.emissiveIntensity);
    putInt("Lighting", "PointLights", g_config.pointLights);
    putFloat("Lighting", "PointLightIntensity", g_config.pointLightIntensity);
    putFloat("Lighting", "PointLightRadius", g_config.pointLightRadius);
    putFloat("Lighting", "PointLightMerge", g_config.pointLightMerge);
    putFloat("Lighting", "PointLightWarmth", g_config.pointLightWarmth);
    putInt("Lighting", "PointLightDebug", g_config.pointLightDebug);
    {
        char v[64];
        sprintf_s(v, "%.3f,%.3f,%.3f", g_config.pointLightTint[0], g_config.pointLightTint[1], g_config.pointLightTint[2]);
        WritePrivateProfileStringA("Lighting", "PointLightTint", v, ini.c_str());
    }
    putInt("Lighting", "Bloom", g_config.bloom);
    putFloat("Lighting", "BloomStrength", g_config.bloomStrength);
    putFloat("Lighting", "BloomThreshold", g_config.bloomThreshold);
    putFloat("Lighting", "BloomDay", g_config.bloomDay);
    putFloat("Lighting", "Specular", g_config.specular);
    putFloat("Lighting", "SpecularRoughness", g_config.specRoughness);
    putInt("Lighting", "Filmic", g_config.filmic);
    putFloat("Lighting", "FilmicExposure", g_config.filmicExposure);
    putFloat("Lighting", "GrassBrightness", g_config.grassBrightness);
    auto putColor = [&](const char* key, const float* c) {
        char b[96]; sprintf_s(b, "%.3f,%.3f,%.3f", c[0], c[1], c[2]); put("TimeOfDay", key, b);
    };
    putColor("MoonColor", g_config.moonColor);
    putColor("NightAmbient", g_config.nightAmbient);
    putColor("DuskAmbient", g_config.duskAmbient);
    putColor("LowSunColor", g_config.lowSunColor);
    putColor("NightTint", g_config.nightTint);
    putFloat("TimeOfDay", "MoonIntensity", g_config.moonIntensity);
    putFloat("TimeOfDay", "MoonRim", g_config.moonRim);
    putFloat("TimeOfDay", "MoonRimGlow", g_config.moonRimGlow);
    putFloat("TimeOfDay", "MoonRimWidth", g_config.moonRimWidth);
    putFloat("TimeOfDay", "MaxExposure", g_config.maxExposure);
    putFloat("TimeOfDay", "NightSky", g_config.nightSky);
    putFloat("TimeOfDay", "NightExposure", g_config.nightExposure);
    putFloat("TimeOfDay", "NightDesaturate", g_config.nightDesaturate);
    putFloat("TimeOfDay", "Stars", g_config.stars);
    putFloat("TimeOfDay", "MoonSheen", g_config.moonSheen);
    putFloat("TimeOfDay", "GrassMoon", g_config.grassMoon);
    putFloat("TimeOfDay", "NightSurface", g_config.nightSurface);
    putInt("Lighting", "ProceduralSky", g_config.proceduralSky);
    putInt("TimeOfDay", "Preview", g_config.timeOfDay);
    putFloat("TimeOfDay", "Hour", g_config.timeOfDayHour);
    putFloat("Lighting", "BakedTerrainDetail", g_config.bakedTerrainDetail);
    putFloat("Lighting", "Ambient", g_config.sunAmbient);
    putFloat("Lighting", "SunIntensity", g_config.sunDirect);
    putInt("Post", "Enabled", g_config.postEnabled);
    putFloat("Post", "SunBoost", g_config.sunBoost);
    putFloat("Post", "ShadowCool", g_config.shadowCool);
    putFloat("Post", "Exposure", g_config.exposure);
    putFloat("Post", "Contrast", g_config.contrast);
    putFloat("Post", "Saturation", g_config.saturation);
    putFloat("Post", "Warmth", g_config.warmth);
    putInt("Effects", "AO", g_config.aoEnabled);
    putFloat("Effects", "AORadius", g_config.aoRadius);
    putFloat("Effects", "AOStrength", g_config.aoStrength);
    putInt("Effects", "LightShafts", g_config.shafts);
    putFloat("Effects", "ShaftStrength", g_config.shaftStrength);
    putFloat("Effects", "ShaftScatter", g_config.shaftScatter);
    putInt("NormalMaps", "Enabled", g_config.normalMaps);
    putFloat("NormalMaps", "Strength", g_config.normalStrength);
    putInt("NormalMaps", "FlipGreen", g_config.normalFlipGreen);
    putInt("Materials","Enabled",g_config.materialsEnabled);
    putFloat("Materials","SkinRoughness",g_config.materialSkinRoughness);
    putFloat("Materials","BuildingRoughness",g_config.materialBuildingRoughness);
    putFloat("Materials","Specular",g_config.materialSpecular);
    putFloat("Materials","Environment",g_config.materialEnvironment);
    putFloat("Materials","NormalStrength",g_config.materialNormalStrength);
    putInt("Water","Enabled",g_config.waterEnabled);
    putFloat("Water","DepthScale",g_config.waterDepthScale);
    putFloat("Water","Transparency",g_config.waterTransparency);
    putFloat("Water","Reflection",g_config.waterReflection);
    putFloat("Water","Foam",g_config.waterFoam);
    putFloat("Water","FoamWidth",g_config.waterFoamWidth);
    putFloat("Water","Distortion",g_config.waterDistortion);
    putFloat("Water","Ripples",g_config.waterRipples);
    putFloat("Water","Speed",g_config.waterSpeed);
    putInt("Water","Interaction",g_config.waterInteraction);
    putInt("Water","SceneReflections",g_config.waterSSR);
    putInt("Water","SurfaceMode",g_config.waterSurfaceMode);
    putFloat("Water","LagoonFoam",g_config.waterLagoonFoam);
    putFloat("Water","LagoonRipples",g_config.waterLagoonRipples);
    putFloat("Water","OceanRipples",g_config.waterOceanRipples);
    putFloat("Water","SwellAmplitude",g_config.waterSwellAmplitude);
    putFloat("Water","SwellLength",g_config.waterSwellLength);
    putFloat("Water","SwellCoastDepth",g_config.waterSwellCoastDepth);
    putFloat("Water","CrestFoam",g_config.waterCrestFoam);
    putFloat("Water","SceneReflectionStrength",g_config.waterSSRStrength);
    putFloat("Water","SceneReflectionDistance",g_config.waterSSRDistance);
    putInt("Water","SceneReflectionSteps",g_config.waterSSRSteps);
    putInt("Performance","LogCSV",g_config.performanceLog);
    putInt("Performance","MeasureGPU",g_config.performanceGPU);
    putInt("Performance","ConstantRing",g_config.constantRing);
    putInt("Performance","TerrainBatching",g_config.terrainBatching);
    putInt("Performance","HitchSampler",g_config.hitchSampler);
    putInt("Performance","HitchThresholdMs",g_config.hitchThresholdMs);
    putInt("Performance","HitchProfileSeconds",g_config.hitchProfileSeconds);
    putInt("Performance","AsyncShaders",g_config.asyncShaders);
    putInt("Performance","PrewarmShaders",g_config.prewarmShaders);
    putInt("Performance","SkinInstancing",g_config.skinInstancing);
    putInt("Performance","Threaded",g_config.threaded);
    putInt("Antialiasing","TAA",g_config.taa);
    putInt("Antialiasing","FSR31",g_config.fsr31);
    putInt("Antialiasing","ObjectMotion",g_config.objectMotion);
    putInt("Antialiasing","FrameGeneration",g_config.frameGeneration);
    putInt("Antialiasing","FrameGenerationDebug",g_config.frameGenerationDebug);
    putInt("Antialiasing","FSR1",g_config.fsr1);
    putFloat("Antialiasing","HistoryWeight",g_config.taaHistory);
    putFloat("Antialiasing","Sharpness",g_config.fsrSharpness);
    putFloat("Water","InteractionStrength",g_config.waterInteractionStrength);
    putInt("Water","DebugView",g_config.waterDebug);
    putInt("Tessellation","Enabled",g_config.tessellation);
    putInt("Tessellation","DinosOnly",g_config.tessDinosOnly);
    putFloat("Tessellation","MaxFactor",g_config.tessMaxFactor);
    putFloat("Tessellation","PixelsPerEdge",g_config.tessPixels);
    putFloat("Tessellation","Curvature",g_config.tessCurvature);
    putInt("Tessellation","FlipWinding",g_config.tessFlipWinding);
    putFloat("Tessellation","Displacement",g_config.tessDisplacement);
    putFloat("Tessellation","DetailMip",g_config.tessDetailMip);
    putInt("Tessellation","InvertRelief",g_config.tessInvertRelief);
    putInt("Tessellation","SeamSafe",g_config.tessSeamSafe);
    putInt("Tessellation","WeldSeams",g_config.tessWeld);
    putInt("Relief","Enabled",g_config.reliefEnabled);
    putFloat("Relief","Depth",g_config.reliefDepth);
    putFloat("Relief","Parallax",g_config.reliefParallax);
    putFloat("Relief","SelfShadow",g_config.reliefShadow);
    putFloat("Relief","Softness",g_config.reliefSoftness);
    putInt("Relief","Invert",g_config.reliefInvert);
    putInt("Grass", "Enabled", g_config.grassPhysics);
    putFloat("Grass", "Push", g_config.grassPush);
    putFloat("Grass", "Flatten", g_config.grassFlatten);
    putFloat("Grass", "RadiusScale", g_config.grassRadius);
    putInt("Grass", "MinBones", g_config.grassMinBones);
    putInt("Grass", "Thin", g_config.grassThin);
    putFloat("Grass", "ThinStart", g_config.grassThinStart);
    putFloat("Grass", "ThinKeep", g_config.grassThinKeep);
    putFloat("Grass", "ThinFill", g_config.grassThinFill);
    putInt("Overlay", "Key", g_config.overlayKey);
    putInt("Overlay", "ShowFPS", g_config.showFps);
    putFloat("Overlay", "Scale", g_config.overlayScale);
}

// ---- formats -----------------------------------------------------------------------------------

FormatInfo GetFormatInfo(DWORD f)
{
    FormatInfo i;
    switch (f) {
    case D8FMT_A8R8G8B8: i.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; i.bytesPerPixel = 4; break;
    case D8FMT_X8R8G8B8: i.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; i.bytesPerPixel = 4; i.convert = true; break;
    case D8FMT_R8G8B8:   i.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; i.bytesPerPixel = 3; i.convert = true; break;
    case D8FMT_R5G6B5: case D8FMT_X1R5G5B5: case D8FMT_A1R5G5B5: case D8FMT_A4R4G4B4:
    case D8FMT_X4R4G4B4: case D8FMT_A8L8: case D8FMT_A8R3G3B2:
        i.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; i.bytesPerPixel = 2; i.convert = true; break;
    case D8FMT_L8: case D8FMT_A8: case D8FMT_A4L4: case D8FMT_R3G3B2:
        i.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; i.bytesPerPixel = 1; i.convert = true; break;
    case D8FMT_V8U8: i.dxgi = DXGI_FORMAT_R8G8_SNORM; i.bytesPerPixel = 2; break;
    case D8FMT_Q8W8V8U8: i.dxgi = DXGI_FORMAT_R8G8B8A8_SNORM; i.bytesPerPixel = 4; break;
    case D8FMT_X8L8V8U8: i.dxgi = DXGI_FORMAT_R8G8B8A8_SNORM; i.bytesPerPixel = 4; i.convert = true; break;
    case D8FMT_DXT1: i.dxgi = DXGI_FORMAT_BC1_UNORM; i.blockBytes = 8; break;
    case D8FMT_DXT2: case D8FMT_DXT3: i.dxgi = DXGI_FORMAT_BC2_UNORM; i.blockBytes = 16; break;
    case D8FMT_DXT4: case D8FMT_DXT5: i.dxgi = DXGI_FORMAT_BC3_UNORM; i.blockBytes = 16; break;
    case D8FMT_D16: case D8FMT_D16_LOCKABLE:
        i.dxgi = DXGI_FORMAT_D24_UNORM_S8_UINT; i.bytesPerPixel = 2; i.depth = true; break;
    case D8FMT_D24S8: case D8FMT_D24X8: case D8FMT_D24X4S4: case D8FMT_D32: case D8FMT_D15S1:
        i.dxgi = DXGI_FORMAT_D24_UNORM_S8_UINT; i.bytesPerPixel = 4; i.depth = true; break;
    default: break;
    }
    return i;
}

bool IsSupportedTextureFormat(DWORD f)
{
    FormatInfo i = GetFormatInfo(f);
    return i.dxgi != DXGI_FORMAT_UNKNOWN && !i.depth;
}

UINT D8RowPitch(DWORD fmt, UINT width)
{
    FormatInfo i = GetFormatInfo(fmt);
    if (i.blockBytes) return ((width + 3) / 4) * i.blockBytes;
    return width * i.bytesPerPixel;
}

UINT D8RowCount(DWORD fmt, UINT height)
{
    return GetFormatInfo(fmt).blockBytes ? (height + 3) / 4 : height;
}

UINT DxgiRowPitch(DXGI_FORMAT f, UINT width)
{
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: return ((width + 3) / 4) * 8;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: return ((width + 3) / 4) * 16;
    case DXGI_FORMAT_R8G8_SNORM: return width * 2;
    default: return width * 4;
    }
}

UINT DxgiRowCount(DXGI_FORMAT f, UINT height)
{
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: return (height + 3) / 4;
    default: return height;
    }
}

static inline BYTE Expand(UINT v, int bits) { return (BYTE)((v * 255 + ((1 << bits) - 1) / 2) / ((1 << bits) - 1)); }

void ConvertRows(DWORD fmt, const BYTE* src, UINT srcPitch, BYTE* dst, UINT dstPitch, UINT w, UINT h)
{
    for (UINT y = 0; y < h; y++) {
        const BYTE* s = src + (size_t)y * srcPitch;
        BYTE* d = dst + (size_t)y * dstPitch; // B, G, R, A
        for (UINT x = 0; x < w; x++, d += 4) {
            UINT v;
            switch (fmt) {
            case D8FMT_X8R8G8B8: d[0] = s[x * 4]; d[1] = s[x * 4 + 1]; d[2] = s[x * 4 + 2]; d[3] = 255; break;
            case D8FMT_R8G8B8: d[0] = s[x * 3]; d[1] = s[x * 3 + 1]; d[2] = s[x * 3 + 2]; d[3] = 255; break;
            case D8FMT_R5G6B5: v = ((const WORD*)s)[x];
                d[2] = Expand(v >> 11, 5); d[1] = Expand((v >> 5) & 63, 6); d[0] = Expand(v & 31, 5); d[3] = 255; break;
            case D8FMT_X1R5G5B5: case D8FMT_A1R5G5B5: v = ((const WORD*)s)[x];
                d[2] = Expand((v >> 10) & 31, 5); d[1] = Expand((v >> 5) & 31, 5); d[0] = Expand(v & 31, 5);
                d[3] = (fmt == D8FMT_A1R5G5B5) ? ((v & 0x8000) ? 255 : 0) : 255; break;
            case D8FMT_A4R4G4B4: case D8FMT_X4R4G4B4: v = ((const WORD*)s)[x];
                d[2] = Expand((v >> 8) & 15, 4); d[1] = Expand((v >> 4) & 15, 4); d[0] = Expand(v & 15, 4);
                d[3] = fmt == D8FMT_A4R4G4B4 ? Expand(v >> 12, 4) : 255; break;
            case D8FMT_A8R3G3B2: v = ((const WORD*)s)[x];
                d[2] = Expand((v >> 5) & 7, 3); d[1] = Expand((v >> 2) & 7, 3); d[0] = Expand(v & 3, 2); d[3] = (BYTE)(v >> 8); break;
            case D8FMT_R3G3B2: v = s[x];
                d[2] = Expand((v >> 5) & 7, 3); d[1] = Expand((v >> 2) & 7, 3); d[0] = Expand(v & 3, 2); d[3] = 255; break;
            case D8FMT_A8L8: v = ((const WORD*)s)[x]; d[0] = d[1] = d[2] = (BYTE)v; d[3] = (BYTE)(v >> 8); break;
            case D8FMT_L8: d[0] = d[1] = d[2] = s[x]; d[3] = 255; break;
            case D8FMT_A8: d[0] = d[1] = d[2] = 0; d[3] = s[x]; break;
            case D8FMT_A4L4: v = s[x]; d[0] = d[1] = d[2] = Expand(v & 15, 4); d[3] = Expand(v >> 4, 4); break;
            case D8FMT_X8L8V8U8: // U, V, L, X -> R8G8B8A8_SNORM (u, v, l, 1); L is unsigned in D3D8
                d[0] = s[x * 4]; d[1] = s[x * 4 + 1]; d[2] = (BYTE)(s[x * 4 + 2] >> 1); d[3] = 127; break;
            default: d[0] = d[1] = d[2] = d[3] = 255; break;
            }
        }
    }
}
