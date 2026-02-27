#pragma once

#ifdef INPUTSYSTEM_EXPORTS
#define INPUT_API __declspec(dllexport)
#else
#define INPUT_API __declspec(dllimport)
#endif

#include <windows.h>

extern "C" {
    // 初始化
    INPUT_API int __stdcall Initialize(int maxQueueSize);

    // 按键事件 (入队列)
    INPUT_API int __stdcall PushKeyEvent(BYTE keyCode, BOOL isDown, DWORD delayMs);

    // 直接发送 (不入队列)
    INPUT_API int __stdcall SendKeyDirect(BYTE keyCode, BOOL isDown);

    // 发送按键组合 (如 Ctrl+C)
    INPUT_API int __stdcall SendKeyCombination(BYTE* keys, int keyCount, DWORD delayMs);

    // 发送文本
    INPUT_API int __stdcall SendText(const char* text);

    // 控制
    INPUT_API int __stdcall StartProcessing();
    INPUT_API int __stdcall StopProcessing();
    INPUT_API void __stdcall ClearQueue();

    // 状态查询
    INPUT_API int __stdcall GetInputQueueStatus(int* queueSize, int* processedCount);

    // 清理
    INPUT_API void __stdcall Shutdown();
    INPUT_API void __stdcall EmergencyStop();
}