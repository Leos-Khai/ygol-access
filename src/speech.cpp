#include "speech.h"

#include <windows.h>
#include <prism.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

static PrismContext* g_ctx;
static PrismBackend* g_backend;
static std::mutex g_mutex; // Prism backends are not thread-safe.

void Log(const char* fmt, ...) {
    static std::mutex logMutex;
    static FILE* file = [] {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        strcpy_s(slash + 1, path + MAX_PATH - (slash + 1), "ygol-access.log");
        return _fsopen(path, "w", _SH_DENYWR);
    }();
    if (!file) return;

    static const ULONGLONG start = GetTickCount64();
    std::lock_guard lock(logMutex);
    fprintf(file, "%7.3f ", (GetTickCount64() - start) / 1000.0);
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fputc('\n', file);
    fflush(file);
}

bool SpeechInit() {
    std::lock_guard lock(g_mutex);
    PrismConfig cfg = prism_config_init();
    g_ctx = prism_init(&cfg);
    if (!g_ctx) {
        Log("prism_init failed");
        return false;
    }
    g_backend = prism_registry_acquire_best(g_ctx);
    if (!g_backend) {
        Log("No Prism backend available");
        return false;
    }
    Log("Prism backend: %s", prism_backend_name(g_backend));
    return true;
}

void Speak(const char* utf8, bool interrupt) {
    std::lock_guard lock(g_mutex);
    if (!g_backend) return;
    PrismError err = prism_backend_output(g_backend, utf8, interrupt);
    if (err != PRISM_OK) Log("prism_backend_output error %d: %s", (int)err, utf8);
}
