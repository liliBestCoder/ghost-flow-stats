#pragma once
// UTF-16 <-> UTF-8 conversion. L1: a thin wrapper over two Win32 calls and
// nothing else -- no business state, no globals.
//
// CP_UTF8 is not negotiable here. CLAUDE.md's "全链路 UTF-8，禁用 ANSI Win32 API"
// rule exists because the A-version APIs convert through the system ACP (GBK on
// a Chinese machine), and the mangled bytes later make nlohmann's dump() throw
// type_error.316 on a thread with no handler above it.
#include <string>

// UTF-16 -> UTF-8 (CP_UTF8). Returns "" on null / empty / conversion error.
std::string WCharToString(const wchar_t* w);

// UTF-8 -> UTF-16 (CP_UTF8). Returns L"" on empty / conversion error.
std::wstring StringToWChar(const std::string& s);

// Recover the specific legacy UTF-8 -> CP936 -> UTF-8 mojibake pattern.
// Requires a private-use character, lossless CP936 round trip and strict UTF-8
// decoding to CJK text without private-use/replacement/control characters.
// Ordinary UTF-8 text (including normal Chinese aliases) is returned unchanged.
std::string RecoverLegacyUtf8Text(const std::string& text);
