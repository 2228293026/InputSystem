#pragma once
#ifdef INPUTSYSTEM_EXPORTS
#define INPUT_API __declspec(dllexport)
#else
#define INPUT_API __declspec(dllimport)
#endif
#include <windows.h>

extern "C" {
    INPUT_API int  __stdcall Initialize(int maxQueueSize);
    INPUT_API int  __stdcall PushKeyEvent(BYTE keyCode, BOOL isDown, DWORD delayMs);
    INPUT_API int  __stdcall SendKeyDirect(BYTE keyCode, BOOL isDown);
    INPUT_API int  __stdcall SendKeyCombination(BYTE* keys, int keyCount, DWORD delayMs);
    INPUT_API int  __stdcall SendText(const char* text);
    INPUT_API int  __stdcall StartProcessing();
    INPUT_API int  __stdcall StopProcessing();
    INPUT_API void __stdcall ClearQueue();
    INPUT_API int  __stdcall GetInputQueueStatus(int* queueSize, int* processedCount);
    INPUT_API void __stdcall Shutdown();
    INPUT_API void __stdcall EmergencyStop();
    INPUT_API BOOL __stdcall IsUsingNtFunctions();
    INPUT_API int  __stdcall GetPressedKeysCount();
}