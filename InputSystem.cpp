#include <windows.h>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <chrono>
#include <vector>

#pragma comment(lib, "user32.lib")

// 导出宏定义
#ifdef INPUTSYSTEM_EXPORTS
#define INPUT_API __declspec(dllexport)
#else
#define INPUT_API __declspec(dllimport)
#endif

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

    InputSystem() = default;
    ~InputSystem() { shutdown(); }

    // 禁用拷贝
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

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

            // ***** 重点：发送按键 *****
            sendKeyEvent(evt.keyCode, evt.isDown);
            processedCount++;

            // 延迟
            if (evt.delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(evt.delayMs));
            }
        }
    }

    // 发送单个按键事件 - 使用 SendInput (最现代、最可靠的方法)
    void sendKeyEvent(BYTE keyCode, BOOL isDown) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = keyCode;                          // 虚拟键码
        input.ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP; // 0=按下, KEYEVENTF_KEYUP=释放

        SendInput(1, &input, sizeof(INPUT));
    }

    // 方法2：使用 keybd_event (旧方法，但简单)
    void sendKeyEventOld(BYTE keyCode, BOOL isDown) {
        keybd_event(keyCode, 0, isDown ? 0 : KEYEVENTF_KEYUP, 0);
    }

    // 方法3：使用扫描码 (更底层)
    void sendKeyEventScanCode(BYTE keyCode, BOOL isDown) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;

        // 转换虚拟键码为扫描码
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

        SendInput(1, &input, sizeof(INPUT));
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
        sendKeyEvent(keyCode, isDown);
        return 0;
    }

    // 发送按键组合 (例如 Ctrl+C)
    int sendKeyCombination(const std::vector<BYTE>& keys, DWORD delayBetweenMs = 50) {
        if (!running || keys.empty()) return -1;

        // 按下所有键
        for (BYTE key : keys) {
            sendKeyEvent(key, TRUE);
            Sleep(delayBetweenMs);
        }

        // 反序释放所有键
        for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
            sendKeyEvent(*it, FALSE);
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
                sendKeyEvent(VK_SHIFT, TRUE);
            }

            sendKeyEvent(keyCode, TRUE);
            sendKeyEvent(keyCode, FALSE);

            if (shiftState & 1) {
                sendKeyEvent(VK_SHIFT, FALSE);
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

    // 清空队列
    void clearQueue() {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!eventQueue.empty()) {
            eventQueue.pop();
        }
        queueSize = 0;
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
    }

    // 紧急停止
    void emergencyStop() {
        clearQueue();
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
}