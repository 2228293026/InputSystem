#include <windows.h>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <chrono>
#include <vector>
#include <set>
#include <algorithm>

#pragma comment(lib, "user32.lib")

// 导出宏定义
#ifdef INPUTSYSTEM_EXPORTS
#define INPUT_API __declspec(dllexport)
#else
#define INPUT_API __declspec(dllimport)
#endif

// 定义 NtUserSendInput 函数类型
typedef UINT(WINAPI* NtUserSendInput_t)(UINT cInputs, LPINPUT pInputs, int cbSize);
static NtUserSendInput_t NtUserSendInput = nullptr;

// 按键事件结构
struct KeyEvent {
    BYTE keyCode;      // 虚拟键码 (如 0x41 = A)
    BOOL isDown;       // TRUE=按下, FALSE=释放
    DWORD delayMs;     // 延迟毫秒

    KeyEvent() : keyCode(0), isDown(FALSE), delayMs(0) {}
    KeyEvent(BYTE k, BOOL d, DWORD ms) : keyCode(k), isDown(d), delayMs(ms) {}
};

// 输入系统类
class InputSystem {
private:
    std::queue<KeyEvent> eventQueue;
    std::mutex queueMutex;
    std::condition_variable queueCond;

    std::unique_ptr<std::thread> workerThread;
    std::atomic<bool> running{ false };
    std::atomic<bool> processing{ true };

    std::atomic<int> processedCount{ 0 };
    std::atomic<int> queueSize{ 0 };

    int maxQueueSize = 1024;

    // 跟踪当前按下的键
    std::set<BYTE> pressedKeys;
    std::mutex pressedKeysMutex;

    // win32u.dll 模块句柄
    HMODULE hWin32u = nullptr;

    InputSystem() {
        initializeNtFunctions();
    }

    ~InputSystem() {
        shutdown();
        if (hWin32u) {
            FreeLibrary(hWin32u);
        }
    }

    // 初始化 NtUserSendInput 函数
    bool initializeNtFunctions() {
        // 尝试从 win32u.dll 加载 (Windows 10/11)
        hWin32u = LoadLibraryA("win32u.dll");
        if (hWin32u) {
            NtUserSendInput = (NtUserSendInput_t)GetProcAddress(hWin32u, "NtUserSendInput");
            if (NtUserSendInput) {
                return true;
            }
        }

        // 如果 win32u.dll 中没有，尝试从 user32.dll 获取 ntdll 的导出
        // 某些系统版本可能有不同的实现方式

        return false;
    }

    // 禁用拷贝
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    // 更新按键状态
    void updateKeyState(BYTE keyCode, BOOL isDown) {
        std::lock_guard<std::mutex> lock(pressedKeysMutex);
        if (isDown) {
            pressedKeys.insert(keyCode);
        }
        else {
            pressedKeys.erase(keyCode);
        }
    }

    // 释放所有当前按下的键
    void releaseAllPressedKeys() {
        std::set<BYTE> keysToRelease;

        // 获取当前按下的所有键的副本
        {
            std::lock_guard<std::mutex> lock(pressedKeysMutex);
            keysToRelease = pressedKeys;
            pressedKeys.clear(); // 清空状态
        }

        // 释放所有按下的键
        for (auto keyCode : keysToRelease) {
            sendKeyEventNt(keyCode, FALSE);
            Sleep(5); // 小延迟确保按键释放顺序
        }
    }

    // 工作线程 - 负责发送按键
    void workerProc() {
        while (running) {
            KeyEvent evt;

            // 从队列取出事件
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCond.wait(lock, [this] {
                    return !eventQueue.empty() || !running;
                    });

                if (!running) break;
                if (!processing) continue;

                evt = eventQueue.front();
                eventQueue.pop();
                queueSize = (int)eventQueue.size();
            }

            // 使用 NtUserSendInput 发送按键
            sendKeyEventNt(evt.keyCode, evt.isDown);

            // 更新按键状态
            updateKeyState(evt.keyCode, evt.isDown);

            processedCount++;

            // 延迟
            if (evt.delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(evt.delayMs));
            }
        }
    }

    // 使用 NtUserSendInput 发送按键
    void sendKeyEventNt(BYTE keyCode, BOOL isDown) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = keyCode;
        input.ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP;

        if (NtUserSendInput) {
            NtUserSendInput(1, &input, sizeof(INPUT));
        }
        else {
            // 降级到标准的 SendInput
            SendInput(1, &input, sizeof(INPUT));
        }
    }

    // 发送单个按键事件 - 使用 SendInput (备用方法)
    void sendKeyEvent(BYTE keyCode, BOOL isDown) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = keyCode;
        input.ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP;

        SendInput(1, &input, sizeof(INPUT));
    }

    // 使用 NtUserSendInput 发送扫描码版本
    void sendKeyEventScanCodeNt(BYTE keyCode, BOOL isDown) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;

        UINT scanCode = MapVirtualKey(keyCode, MAPVK_VK_TO_VSC);
        input.ki.wScan = (WORD)scanCode;
        input.ki.dwFlags = KEYEVENTF_SCANCODE;

        if (!isDown) {
            input.ki.dwFlags |= KEYEVENTF_KEYUP;
        }

        // 处理扩展键
        if (keyCode == VK_LEFT || keyCode == VK_RIGHT ||
            keyCode == VK_UP || keyCode == VK_DOWN ||
            keyCode == VK_HOME || keyCode == VK_END ||
            keyCode == VK_PRIOR || keyCode == VK_NEXT ||
            keyCode == VK_INSERT || keyCode == VK_DELETE) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }

        if (NtUserSendInput) {
            NtUserSendInput(1, &input, sizeof(INPUT));
        }
        else {
            SendInput(1, &input, sizeof(INPUT));
        }
    }

public:
    static InputSystem& getInstance() {
        static InputSystem instance;
        return instance;
    }

    // 初始化
    int initialize(int maxSize) {
        std::lock_guard<std::mutex> lock(queueMutex);

        if (running) {
            return 0; // 已经在运行
        }

        maxQueueSize = (maxSize > 0) ? maxSize : 1024;
        running = true;
        processing = true;
        processedCount = 0;

        // 清空队列
        while (!eventQueue.empty()) {
            eventQueue.pop();
        }
        queueSize = 0;

        // 清空按键状态
        {
            std::lock_guard<std::mutex> keysLock(pressedKeysMutex);
            pressedKeys.clear();
        }

        // 启动工作线程
        workerThread = std::make_unique<std::thread>(&InputSystem::workerProc, this);

        return 0;
    }

    // 推送按键事件到队列
    int pushKeyEvent(BYTE keyCode, BOOL isDown, DWORD delayMs) {
        if (!running) return -1;  // 未初始化

        {
            std::lock_guard<std::mutex> lock(queueMutex);

            if (eventQueue.size() >= maxQueueSize) {
                return -2;  // 队列满
            }

            eventQueue.emplace(keyCode, isDown, delayMs);
            queueSize = (int)eventQueue.size();
        }

        queueCond.notify_one();
        return 0;
    }

    // 直接发送，不入队列
    int sendKeyDirect(BYTE keyCode, BOOL isDown) {
        if (!running) return -1;

        if (NtUserSendInput) {
            sendKeyEventNt(keyCode, isDown);
        }
        else {
            sendKeyEvent(keyCode, isDown);
        }

        // 更新按键状态
        updateKeyState(keyCode, isDown);

        return 0;
    }

    // 发送按键组合 (例如 Ctrl+C)
    int sendKeyCombination(const std::vector<BYTE>& keys, DWORD delayBetweenMs = 50) {
        if (!running || keys.empty()) return -1;

        // 按下所有键
        for (BYTE key : keys) {
            if (NtUserSendInput) {
                sendKeyEventNt(key, TRUE);
            }
            else {
                sendKeyEvent(key, TRUE);
            }
            updateKeyState(key, TRUE);
            Sleep(delayBetweenMs);
        }

        // 反序释放所有键
        for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
            if (NtUserSendInput) {
                sendKeyEventNt(*it, FALSE);
            }
            else {
                sendKeyEvent(*it, FALSE);
            }
            updateKeyState(*it, FALSE);
            Sleep(delayBetweenMs);
        }

        return 0;
    }

    // 发送文本
    int sendText(const char* text) {
        if (!running || !text) return -1;

        while (*text) {
            char c = *text++;

            // 转换字符为虚拟键码
            SHORT vk = VkKeyScanA(c);
            if (vk == -1) continue;

            BYTE keyCode = LOBYTE(vk);
            BYTE shiftState = HIBYTE(vk);

            // 如果需要Shift
            if (shiftState & 1) {
                if (NtUserSendInput) {
                    sendKeyEventNt(VK_SHIFT, TRUE);
                }
                else {
                    sendKeyEvent(VK_SHIFT, TRUE);
                }
                updateKeyState(VK_SHIFT, TRUE);
            }

            if (NtUserSendInput) {
                sendKeyEventNt(keyCode, TRUE);
                sendKeyEventNt(keyCode, FALSE);
            }
            else {
                sendKeyEvent(keyCode, TRUE);
                sendKeyEvent(keyCode, FALSE);
            }
            updateKeyState(keyCode, TRUE);
            updateKeyState(keyCode, FALSE);

            if (shiftState & 1) {
                if (NtUserSendInput) {
                    sendKeyEventNt(VK_SHIFT, FALSE);
                }
                else {
                    sendKeyEvent(VK_SHIFT, FALSE);
                }
                updateKeyState(VK_SHIFT, FALSE);
            }

            Sleep(10); // 字符间延迟
        }

        return 0;
    }

    // 开始处理
    int startProcessing() {
        processing = true;
        queueCond.notify_one();
        return 0;
    }

    // 停止处理
    int stopProcessing() {
        processing = false;
        return 0;
    }

    // 清空队列 - 修复版本：确保释放所有按下的键
    void clearQueue() {
        // 首先暂停处理
        bool wasProcessing = processing;
        processing = false;

        // 清空队列
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            while (!eventQueue.empty()) {
                eventQueue.pop();
            }
            queueSize = 0;
        }

        // 释放所有当前按下的键
        releaseAllPressedKeys();

        // 恢复处理状态
        processing = wasProcessing;
    }

    // 获取状态
    int getStatus(int* outQueueSize, int* outProcessedCount) {
        if (outQueueSize) {
            *outQueueSize = queueSize;
        }
        if (outProcessedCount) {
            *outProcessedCount = processedCount;
        }
        return 0;
    }

    // 关闭系统
    void shutdown() {
        running = false;
        queueCond.notify_all();

        if (workerThread && workerThread->joinable()) {
            workerThread->join();
            workerThread.reset();
        }

        // 确保所有按键被释放
        releaseAllPressedKeys();
    }

    // 紧急停止
    void emergencyStop() {
        // 清空队列
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            while (!eventQueue.empty()) {
                eventQueue.pop();
            }
            queueSize = 0;
        }

        // 释放所有按下的键
        releaseAllPressedKeys();
    }

    // 检查是否成功加载 NtUserSendInput
    bool isUsingNtFunctions() {
        return NtUserSendInput != nullptr;
    }

    // 获取当前按下的键数量
    int getPressedKeysCount() {
        std::lock_guard<std::mutex> lock(pressedKeysMutex);
        return (int)pressedKeys.size();
    }
};

// DLL 入口点
static BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        InputSystem::getInstance().shutdown();
        break;
    }
    return TRUE;
}

// 导出函数
extern "C" {

    INPUT_API int __stdcall Initialize(int maxQueueSize) {
        return InputSystem::getInstance().initialize(maxQueueSize);
    }

    INPUT_API int __stdcall PushKeyEvent(BYTE keyCode, BOOL isDown, DWORD delayMs) {
        return InputSystem::getInstance().pushKeyEvent(keyCode, isDown, delayMs);
    }

    INPUT_API int __stdcall SendKeyDirect(BYTE keyCode, BOOL isDown) {
        return InputSystem::getInstance().sendKeyDirect(keyCode, isDown);
    }

    INPUT_API int __stdcall SendKeyCombination(BYTE* keys, int keyCount, DWORD delayMs) {
        if (!keys || keyCount <= 0) return -1;
        std::vector<BYTE> keyVec(keys, keys + keyCount);
        return InputSystem::getInstance().sendKeyCombination(keyVec, delayMs);
    }

    INPUT_API int __stdcall SendText(const char* text) {
        return InputSystem::getInstance().sendText(text);
    }

    INPUT_API int __stdcall StartProcessing() {
        return InputSystem::getInstance().startProcessing();
    }

    INPUT_API int __stdcall StopProcessing() {
        return InputSystem::getInstance().stopProcessing();
    }

    INPUT_API void __stdcall ClearQueue() {
        InputSystem::getInstance().clearQueue();
    }

    INPUT_API int __stdcall GetInputQueueStatus(int* queueSize, int* processedCount) {
        return InputSystem::getInstance().getStatus(queueSize, processedCount);
    }

    INPUT_API void __stdcall Shutdown() {
        InputSystem::getInstance().shutdown();
    }

    INPUT_API void __stdcall EmergencyStop() {
        InputSystem::getInstance().emergencyStop();
    }

    // 新增函数：检查是否使用 NtUserSendInput
    INPUT_API BOOL __stdcall IsUsingNtFunctions() {
        return InputSystem::getInstance().isUsingNtFunctions() ? TRUE : FALSE;
    }

    // 新增函数：获取当前按下的键数量
    INPUT_API int __stdcall GetPressedKeysCount() {
        return InputSystem::getInstance().getPressedKeysCount();
    }
}