#pragma once
#include <windows.h>
struct JPOGCameraSettings {
    DWORD size, version;
    float minDistance, maxDistance, focusMinDistance, focusMaxDistance;
    float minPitch, maxPitch, fixedFov, responseSeconds;
    float panSensitivity, rotateSensitivity;
    DWORD controllerReady, followReady;
};
typedef BOOL(WINAPI* JPOGCameraSettingsGet)(JPOGCameraSettings*);
typedef BOOL(WINAPI* JPOGCameraSettingsSet)(const JPOGCameraSettings*, BOOL);
// Grass distance (GrassDistance): the scale, and the largest scale the grass mesh pool built at
// startup covers. Set applies at the next game update; save writes JPOGMouseFix.ini.
typedef BOOL(WINAPI* JPOGGrassGet)(float* scale, float* poolScale);
typedef BOOL(WINAPI* JPOGGrassSet)(float scale, BOOL save);
