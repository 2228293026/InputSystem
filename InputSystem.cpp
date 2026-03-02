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
#include <array>

#pragma comment(lib, "user32.lib")
#include "InputSystem.h"

// ─────────────────────────────────────────────
//  函数指针
// ─────────────────────────────────────────────
typedef UINT(NTAPI* NtUserInjectKeyboardInput_t)(KEYBDINPUT* pInputs, UINT nInputs);
typedef UINT(WINAPI* NtUserSendInput_t)(UINT cInputs, LPINPUT pInputs, int cbSize);

static NtUserInjectKeyboardInput_t pNtUserInjectKeyboardInput = nullptr;
static NtUserSendInput_t           pNtUserSendInput = nullptr;

// ─────────────────────────────────────────────
//  扫描码缓存（256 项，初始化一次）
// ─────────────────────────────────────────────
static std::array<WORD, 256> g_scanCache = {};

static void buildScanCache() {
    for (int i = 0; i < 256; i++)
        g_scanCache[i] = static_cast<WORD>(MapVirtualKey(i, MAPVK_VK_TO_VSC));
}

// ─────────────────────────────────────────────
//  按键事件结构
// ─────────────────────────────────────────────
struct KeyEvent {
    BYTE  keyCode;
    BOOL  isDown;
    DWORD delayMs;

    KeyEvent() : keyCode(0), isDown(FALSE), delayMs(0) {}
    KeyEvent(BYTE k, BOOL d, DWORD ms) : keyCode(k), isDown(d), delayMs(ms) {}
};

static bool g_extTableBuilt = false;
static bool g_extTable[256] = {};

static void buildExtTable() {
    if (g_extTableBuilt) return;
    const BYTE ext[] = {
        VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
        VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
        VK_INSERT, VK_DELETE,
        VK_RCONTROL, VK_RMENU,
        VK_NUMLOCK, VK_SNAPSHOT
    };
    for (BYTE k : ext) g_extTable[k] = true;
    g_extTableBuilt = true;
}

// ─────────────────────────────────────────────
//  InputSystem
// ─────────────────────────────────────────────
class InputSystem {
private:
    std::queue<KeyEvent>    eventQueue;
    std::mutex              queueMutex;
    std::condition_variable queueCond;

    std::unique_ptr<std::thread> workerThread;
    std::atomic<bool> running{ false };
    std::atomic<bool> processing{ true };
    std::atomic<int>  processedCount{ 0 };
    std::atomic<int>  queueSize{ 0 };

    // ── 当前输入模式（原子，可随时切换）──────
    std::atomic<int> currentMode{ (int)InputMode::Auto };

    int maxQueueSize = 1024;

    std::set<BYTE> pressedKeys;
    std::mutex     pressedKeysMutex;

    HMODULE hWin32u = nullptr;

    // ─────────────────────────────────────────
    InputSystem() {
        buildScanCache();
        buildExtTable();
        loadNtFunctions();
    }

    ~InputSystem() {
        shutdown();
        if (hWin32u) FreeLibrary(hWin32u);
    }

    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    // ── 加载底层函数 ──────────────────────────
    bool loadNtFunctions() {
        hWin32u = LoadLibraryA("win32u.dll");
        if (!hWin32u) return false;

        pNtUserInjectKeyboardInput = reinterpret_cast<NtUserInjectKeyboardInput_t>(
            GetProcAddress(hWin32u, "NtUserInjectKeyboardInput"));

        pNtUserSendInput = reinterpret_cast<NtUserSendInput_t>(
            GetProcAddress(hWin32u, "NtUserSendInput"));

        return (pNtUserInjectKeyboardInput != nullptr || pNtUserSendInput != nullptr);
    }

    // ── 解析 Auto 模式下的实际模式 ────────────
    InputMode resolveMode() const {
        int m = currentMode.load(std::memory_order_relaxed);
        if (m == (int)InputMode::Auto) {
            if (pNtUserInjectKeyboardInput) return InputMode::NtUserInjectKeyboard;
            if (pNtUserSendInput)           return InputMode::NtUserSendInput;
            return InputMode::SendInput;
        }
        // 请求的模式不可用时降级
        if (m == (int)InputMode::NtUserInjectKeyboard && !pNtUserInjectKeyboardInput) {
            if (pNtUserSendInput) return InputMode::NtUserSendInput;
            return InputMode::SendInput;
        }
        if (m == (int)InputMode::NtUserSendInput && !pNtUserSendInput)
            return InputMode::SendInput;

        return static_cast<InputMode>(m);
    }

    // ── 按键状态追踪 ──────────────────────────
    void updateKeyState(BYTE keyCode, BOOL isDown) {
        std::lock_guard<std::mutex> lock(pressedKeysMutex);
        if (isDown) pressedKeys.insert(keyCode);
        else        pressedKeys.erase(keyCode);
    }

    void releaseAllPressedKeys() {
        std::set<BYTE> keys;
        {
            std::lock_guard<std::mutex> lock(pressedKeysMutex);
            keys = pressedKeys;
            pressedKeys.clear();
        }
        for (BYTE k : keys) {
            sendKeyCore(k, FALSE);
            Sleep(5);
        }
    }

    // ── 核心发送 ──────────────────────────────
    void sendKeyCore(BYTE keyCode, BOOL isDown) {
        WORD scan = g_scanCache[keyCode];
        DWORD flags = isDown ? 0 : KEYEVENTF_KEYUP;
        if (g_extTable[keyCode]) flags |= KEYEVENTF_EXTENDEDKEY;

        switch (resolveMode()) {

        case InputMode::NtUserInjectKeyboard: {
            // Level 1：最底层，直接注入原始输入流
            KEYBDINPUT ki = {};
            ki.wVk = keyCode;
            ki.wScan = scan;
            ki.dwFlags = flags;
            pNtUserInjectKeyboardInput(&ki, 1);
            break;
        }

        case InputMode::NtUserSendInput: {
            // Level 2：内核边界
            INPUT inp = {};
            inp.type = INPUT_KEYBOARD;
            inp.ki.wVk = keyCode;
            inp.ki.wScan = scan;
            inp.ki.dwFlags = flags;
            pNtUserSendInput(1, &inp, sizeof(INPUT));
            break;
        }

        case InputMode::SendInput:
        default: {
            // Level 3：标准用户层
            INPUT inp = {};
            inp.type = INPUT_KEYBOARD;
            inp.ki.wVk = keyCode;
            inp.ki.wScan = scan;
            inp.ki.dwFlags = flags;
            SendInput(1, &inp, sizeof(INPUT));
            break;
        }
        }
    }

    // ── 工作线程 ──────────────────────────────
    void workerProc() {
        while (running) {
            KeyEvent evt;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCond.wait(lock, [this] {
                    return (!eventQueue.empty() && processing) || !running;
                    });

                if (!running) break;
                if (eventQueue.empty()) continue;

                evt = eventQueue.front();
                eventQueue.pop();
                queueSize = static_cast<int>(eventQueue.size());
            }

            sendKeyCore(evt.keyCode, evt.isDown);
            updateKeyState(evt.keyCode, evt.isDown);
            processedCount++;

            if (evt.delayMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(evt.delayMs));
        }
    }

public:
    static InputSystem& getInstance() {
        static InputSystem instance;
        return instance;
    }

    // ── 模式控制 ──────────────────────────────

    // 设置模式，返回实际生效的模式（Auto 时返回解析后的真实模式）
    int setInputMode(int mode) {
        // 验证范围
        if (mode < (int)InputMode::Auto || mode >(int)InputMode::SendInput)
            return -1;

        currentMode.store(mode, std::memory_order_relaxed);
        return (int)resolveMode(); // 返回实际生效的模式
    }

    int getInputMode() const {
        return (int)resolveMode();
    }

    // 返回可用模式位掩码：bit0=Auto, bit1=Inject, bit2=NtSendInput, bit3=SendInput
    int getAvailableModes() const {
        int mask = (1 << (int)InputMode::Auto) | (1 << (int)InputMode::SendInput);
        if (pNtUserInjectKeyboardInput) mask |= (1 << (int)InputMode::NtUserInjectKeyboard);
        if (pNtUserSendInput)           mask |= (1 << (int)InputMode::NtUserSendInput);
        return mask;
    }

    // ── 其余方法与原版相同 ────────────────────

    int initialize(int maxSize) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (running) return 0;

        maxQueueSize = (maxSize > 0) ? maxSize : 1024;
        running = true;
        processing = true;
        processedCount = 0;

        while (!eventQueue.empty()) eventQueue.pop();
        queueSize = 0;

        { std::lock_guard<std::mutex> kl(pressedKeysMutex); pressedKeys.clear(); }

        workerThread = std::make_unique<std::thread>(&InputSystem::workerProc, this);
        return 0;
    }

    int pushKeyEvent(BYTE keyCode, BOOL isDown, DWORD delayMs) {
        if (!running) return -1;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (static_cast<int>(eventQueue.size()) >= maxQueueSize) return -2;
            eventQueue.emplace(keyCode, isDown, delayMs);
            queueSize = static_cast<int>(eventQueue.size());
        }
        queueCond.notify_one();
        return 0;
    }

    int sendKeyDirect(BYTE keyCode, BOOL isDown) {
        if (!running) return -1;
        sendKeyCore(keyCode, isDown);
        updateKeyState(keyCode, isDown);
        return 0;
    }

    int sendKeyCombination(const std::vector<BYTE>& keys, DWORD delayMs = 50) {
        if (!running || keys.empty()) return -1;
        for (BYTE k : keys) { sendKeyCore(k, TRUE);  updateKeyState(k, TRUE);  Sleep(delayMs); }
        for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        {
            sendKeyCore(*it, FALSE); updateKeyState(*it, FALSE); Sleep(delayMs);
        }
        return 0;
    }

    int sendText(const char* text) {
        if (!running || !text) return -1;
        while (*text) {
            char c = *text++;
            SHORT vk = VkKeyScanA(c);
            if (vk == -1) continue;

            BYTE keyCode = LOBYTE(vk);
            BYTE shiftState = HIBYTE(vk);

            if (shiftState & 1) { sendKeyCore(VK_SHIFT, TRUE);  updateKeyState(VK_SHIFT, TRUE); }
            sendKeyCore(keyCode, TRUE);
            sendKeyCore(keyCode, FALSE);
            updateKeyState(keyCode, TRUE);
            updateKeyState(keyCode, FALSE);
            if (shiftState & 1) { sendKeyCore(VK_SHIFT, FALSE); updateKeyState(VK_SHIFT, FALSE); }
            Sleep(10);
        }
        return 0;
    }

    int  startProcessing() { processing = true;  queueCond.notify_one(); return 0; }
    int  stopProcessing() { processing = false; return 0; }

    void clearQueue() {
        processing.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            while (!eventQueue.empty()) eventQueue.pop();
            queueSize = 0;
        }
        releaseAllPressedKeys();
        processing.store(true, std::memory_order_relaxed); // 清完总是恢复
    }

    int getStatus(int* outQueueSize, int* outProcessedCount) {
        if (outQueueSize)      *outQueueSize = queueSize;
        if (outProcessedCount) *outProcessedCount = processedCount;
        return 0;
    }

    void shutdown() {
        running = false;
        queueCond.notify_all();
        if (workerThread && workerThread->joinable()) { workerThread->join(); workerThread.reset(); }
        releaseAllPressedKeys();
    }

    void emergencyStop() {
        { std::lock_guard<std::mutex> lock(queueMutex); while (!eventQueue.empty()) eventQueue.pop(); queueSize = 0; }
        releaseAllPressedKeys();
    }

    bool isUsingNtFunctions() { return pNtUserInjectKeyboardInput != nullptr || pNtUserSendInput != nullptr; }
    int  getPressedKeysCount() { std::lock_guard<std::mutex> lock(pressedKeysMutex); return static_cast<int>(pressedKeys.size()); }
};

// ─────────────────────────────────────────────
//  DLL 入口
// ─────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: DisableThreadLibraryCalls(hModule); break;
    case DLL_PROCESS_DETACH: InputSystem::getInstance().shutdown(); break;
    }
    return TRUE;
}

// ─────────────────────────────────────────────
//  导出函数
// ─────────────────────────────────────────────
extern "C" {
    INPUT_API int  __stdcall Initialize(int maxQueueSize) { return InputSystem::getInstance().initialize(maxQueueSize); }
    INPUT_API int  __stdcall PushKeyEvent(BYTE keyCode, BOOL isDown, DWORD delayMs) { return InputSystem::getInstance().pushKeyEvent(keyCode, isDown, delayMs); }
    INPUT_API int  __stdcall SendKeyDirect(BYTE keyCode, BOOL isDown) { return InputSystem::getInstance().sendKeyDirect(keyCode, isDown); }
    INPUT_API int  __stdcall SendKeyCombination(BYTE* keys, int keyCount, DWORD delayMs) {
        if (!keys || keyCount <= 0) return -1;
        return InputSystem::getInstance().sendKeyCombination(std::vector<BYTE>(keys, keys + keyCount), delayMs);
    }
    INPUT_API int  __stdcall SendText(const char* text) { return InputSystem::getInstance().sendText(text); }
    INPUT_API int  __stdcall StartProcessing() { return InputSystem::getInstance().startProcessing(); }
    INPUT_API int  __stdcall StopProcessing() { return InputSystem::getInstance().stopProcessing(); }
    INPUT_API void __stdcall ClearQueue() { InputSystem::getInstance().clearQueue(); }
    INPUT_API int  __stdcall GetInputQueueStatus(int* queueSize, int* processedCount) { return InputSystem::getInstance().getStatus(queueSize, processedCount); }
    INPUT_API void __stdcall Shutdown() { InputSystem::getInstance().shutdown(); }
    INPUT_API void __stdcall EmergencyStop() { InputSystem::getInstance().emergencyStop(); }
    INPUT_API BOOL __stdcall IsUsingNtFunctions() { return InputSystem::getInstance().isUsingNtFunctions() ? TRUE : FALSE; }
    INPUT_API int  __stdcall GetPressedKeysCount() { return InputSystem::getInstance().getPressedKeysCount(); }

    // ── 模式控制 ──────────────────────────────
    INPUT_API int  __stdcall SetInputMode(int mode) { return InputSystem::getInstance().setInputMode(mode); }
    INPUT_API int  __stdcall GetInputMode() { return InputSystem::getInstance().getInputMode(); }
    INPUT_API int  __stdcall GetAvailableModes() { return InputSystem::getInstance().getAvailableModes(); }
}