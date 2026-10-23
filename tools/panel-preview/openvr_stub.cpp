// Link-only stand-in for libopenvr_api.so: lets the panel build in a container for --dump-png previews.
#include "openvr.h"
using namespace vr;
extern "C" {
bool VR_IsHmdPresent() { return false; }
bool VR_IsRuntimeInstalled() { return false; }
bool VR_GetRuntimePath(char*, uint32_t, uint32_t*) { return false; }
const char* VR_GetVRInitErrorAsSymbol(EVRInitError) { return "stub"; }
const char* VR_GetVRInitErrorAsEnglishDescription(EVRInitError) { return "stub"; }
void* VR_GetGenericInterface(const char*, EVRInitError* e) { if (e) *e = VRInitError_Init_InstallationNotFound; return nullptr; }
bool VR_IsInterfaceVersionValid(const char*) { return false; }
uint32_t VR_GetInitToken() { return 0; }
uint32_t VR_InitInternal2(EVRInitError* e, EVRApplicationType, const char*) { if (e) *e = VRInitError_Init_InstallationNotFound; return 0; }
void VR_ShutdownInternal() {}
}
