#pragma once

// Logs to ygol-access.log next to the game exe.
void Log(const char* fmt, ...);

// Starts Prism and picks the best backend (screen reader first, then TTS).
bool SpeechInit();

// Speaks UTF-8 text through the backend. Safe to call from any thread.
void Speak(const char* utf8, bool interrupt = true);
