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
