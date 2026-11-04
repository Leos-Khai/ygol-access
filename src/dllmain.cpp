// Proxy for xinput9_1_0.dll. The game loads this copy from its own folder,
// which gets our code into the process. XInputGetState is the only function
// YuGiOh.exe imports from it, so it is the only one forwarded.
#include <windows.h>
#include <MinHook.h>

#include <cstdio>

#include "duel.h"
#include "menu.h"
#include "scan.h"
#include "speech.h"
#include "text.h"

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);

extern "C" __declspec(dllexport) DWORD WINAPI XInputGetState(DWORD userIndex, void* state) {
    static XInputGetStateFn real = [] {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\xinput9_1_0.dll");
        HMODULE dll = LoadLibraryW(path);
        auto fn = dll ? (XInputGetStateFn)GetProcAddress(dll, "XInputGetState") : nullptr;
        if (!fn) Log("Could not load system xinput9_1_0.dll; controller input disabled");
        return fn;
    }();
    return real ? real(userIndex, state) : ERROR_DEVICE_NOT_CONNECTED;
}

// The game's own window is in front. Not its text entry box (naming a deck): that is a separate window owned
// by the game window, and typing R or H there must reach it.
static bool GameHasFocus() {
    HWND window = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId() && !GetWindow(window, GW_OWNER);
}

constexpr UINT WM_APP_HOTKEY = WM_APP + 1;
static DWORD g_hotkeyThread;
// An unassigned key, sent after an Alt hotkey: Alt released with only a swallowed key in between would open the
// window's system menu.
constexpr DWORD kMaskKey = 0xE8;

// Mod hotkeys, handled only while the game has focus and hidden from the game. None is bound by the game's
// default keyboard layout (Help & Options > Controls).
//   F1  list these hotkeys
//   F5  read all text on screen
//   R   read the player's name and DP (in the deck editor: the deck's card counts)
//   H   read both duelists' life points
//   I   read the focused card's full info
// In a duel (Alt + the key: the opponent's):
//   F   field (monster, spell/trap and field zones)      G   graveyard        V   banished cards
//   U   hand (the opponent's: a count)                    E   Extra Deck       T   turn, phase, deck counts
static bool IsHotkey(DWORD vk) {
    return vk == VK_F1 || vk == VK_F5 || vk == 'R' || vk == 'H' || vk == 'I' || vk == 'F' || vk == 'G' || vk == 'V' || vk == 'U' ||
           vk == 'E' || vk == 'T';
}

static LRESULT CALLBACK KeyboardHook(int code, WPARAM wParam, LPARAM lParam) {
    static bool down[256];  // the hook sees auto-repeat as more key-downs; act on the first only
    auto* key = (KBDLLHOOKSTRUCT*)lParam;
    if (code == HC_ACTION && key->vkCode == kMaskKey) return CallNextHookEx(nullptr, code, wParam, lParam);
    if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) && GameHasFocus()) MenuAnyKeyPressed(!IsHotkey(key->vkCode));
    if (code == HC_ACTION && (key->vkCode == VK_LEFT || key->vkCode == VK_RIGHT) && wParam == WM_KEYDOWN && GameHasFocus()) {
        MenuValueKeyPressed();  // the game still gets the key
    }
    if (code == HC_ACTION && IsHotkey(key->vkCode) && GameHasFocus()) {
        bool isDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        bool alt = (key->flags & LLKHF_ALTDOWN) != 0;
        if (isDown && !down[key->vkCode]) PostThreadMessageW(g_hotkeyThread, WM_APP_HOTKEY, key->vkCode, alt);
        down[key->vkCode] = isDown;
        return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Game bug fix: on quitting, the main thread stops pumping messages and waits for the worker thread, which may be
// re-setting the (unchanging) window title. SetWindowTextW from another thread waits for the window's thread:
// deadlock, the game freezes on exit. The title is set once at creation, so calls from other threads are skipped.
using SetTitleFn = void(__fastcall*)(uint8_t* window);
static SetTitleFn g_setTitle;
static void __fastcall SetTitleHook(uint8_t* window) {
    HWND hwnd = *(HWND*)(window + 0x40);  // the game window object's HWND
    if (hwnd && GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) return;
    g_setTitle(window);
}

static void FixExitHang() {
    void* setTitle = FindInGame("SetWindowTitle",
        "40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40 48 83 79 40 00 48 8B D9 0F 84");
    if (!setTitle || MH_CreateHook(setTitle, (void*)&SetTitleHook, (void**)&g_setTitle) != MH_OK || MH_EnableHook(setTitle) != MH_OK)
        Log("Exit hang fix not installed");
}

static DWORD WINAPI Startup(void*) {
    Log("ygol-access loaded");
    if (MH_STATUS status = MH_Initialize(); status != MH_OK) {
        Log("MinHook init failed: %s", MH_StatusToString(status));
    } else if (FixExitHang(); !TextHooksInstall()) {
        Log("Text hooks not installed");
    } else if (!MenuHooksInstall()) {
        Log("Menu hooks not installed");
    } else if (!DuelHooksInstall()) {
        Log("Duel hooks not installed");
    }
    if (SpeechInit()) {
        Speak("Yu-Gi-Oh accessibility mod loaded. Press F1 for hotkeys.");
        if (int missed = GameMisses()) {
            char warning[200];
            sprintf_s(warning, "Warning: %d parts of the game were not found, probably because the game updated. Some speech "
                      "will be missing until the mod is updated.", missed);
            Speak(warning, false);
        }
    }

    // Hotkeys come from a low-level keyboard hook, which needs a message loop on this thread.
    // The hook only posts a message; the work runs here, outside the hook's time limit.
    g_hotkeyThread = GetCurrentThreadId();
    if (!SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, nullptr, 0)) Log("Keyboard hook failed: %lu", GetLastError());
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message != WM_APP_HOTKEY) continue;
        if (msg.lParam) {
            INPUT mask[2] = {};
            mask[0].type = mask[1].type = INPUT_KEYBOARD;
            mask[0].ki.wVk = mask[1].ki.wVk = (WORD)kMaskKey;
            mask[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, mask, sizeof(INPUT));
        }
        if (msg.wParam == VK_F1) {
            Log("hotkey help");
            Speak("Hotkeys. F1: this help. F5: read the screen. R: your name and DP; in the deck editor, the deck's "
                  "card counts. H: life points. I: the focused card's full info. In a duel: F field, U hand, "
                  "G graveyard, V banished, E Extra Deck, T turn and phase. Hold Alt with F, U, G, V or E for the "
                  "opponent's.");
        }
        if (msg.wParam == VK_F5) SpeakScreen();
        if (msg.wParam == 'R' && !MenuSpeakDeckStats()) SpeakStatus();  // deck editor: its stats; elsewhere: DP
        if (msg.wParam == 'H') SpeakLifePoints();
        if (msg.wParam == 'I') MenuSpeakCardInfo();
        int side = msg.lParam ? 1 : 0;  // Alt: the opponent's
        if (msg.wParam == 'F') DuelSpeakField(side);
        if (msg.wParam == 'G') DuelSpeakGraveyard(side);
        if (msg.wParam == 'V') DuelSpeakBanished(side);
        if (msg.wParam == 'U') DuelSpeakHand(side);
        if (msg.wParam == 'E') DuelSpeakExtraDeck(side);
        if (msg.wParam == 'T') DuelSpeakTurn();
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(dll);
        // Prism uses COM and loads DLLs, which is unsafe under the loader lock, so start it on a thread.
        if (HANDLE thread = CreateThread(nullptr, 0, Startup, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    // No prism_shutdown on detach: the process is exiting, and shutdown under the loader lock can deadlock.
    return TRUE;
}
