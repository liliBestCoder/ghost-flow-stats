#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "str_conv.h"
#include <algorithm>
#include <limits>

// Moved verbatim from ui/str_utils.cpp; no behaviour change.
std::string WCharToString(const wchar_t* w) {
    if (!w) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return "";
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, NULL, NULL);
    return s;
}

std::wstring StringToWChar(const std::string& s) {
     if (s.empty()) return L"";
     int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
     if (len <= 0) return L"";
     std::wstring ws(len - 1, L'\0');
     MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
     return ws;
}

std::string RecoverLegacyUtf8Text(const std::string& text) {
    if (text.empty() || text.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
        text.find('\0') != std::string::npos) return text;
    const auto original = StringToWChar(text);
    auto privateUse = [](wchar_t c) { return c >= 0xE000 && c <= 0xF8FF; };
    if (!std::any_of(original.begin(), original.end(), privateUse) || WCharToString(original.c_str()) != text)
        return text;

    // Fixed legacy code page, never the machine's current ACP. No best-fit or
    // replacement is allowed: a failed/ambiguous conversion keeps the input.
    BOOL substituted = FALSE;
    const int size = WideCharToMultiByte(936, WC_NO_BEST_FIT_CHARS, original.data(),
        static_cast<int>(original.size()), nullptr, 0, nullptr, &substituted);
    if (size <= 0 || substituted) return text;
    std::string bytes(size, '\0');
    if (WideCharToMultiByte(936, WC_NO_BEST_FIT_CHARS, original.data(), static_cast<int>(original.size()),
        bytes.data(), size, nullptr, &substituted) != size || substituted) return text;

    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), size, nullptr, 0);
    if (count <= 0) return text;
    std::wstring recovered(count, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), size, recovered.data(), count) != count)
        return text;
    bool hasCjk = false;
    for (wchar_t c : recovered) {
        if (privateUse(c) || c == 0xFFFD || c < 0x20 || (c >= 0x7F && c <= 0x9F)) return text;
        hasCjk = hasCjk || (c >= 0x3400 && c <= 0x9FFF);
    }
    if (!hasCjk) return text;
    const int roundTripCount = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, bytes.data(), size, nullptr, 0);
    if (roundTripCount != static_cast<int>(original.size())) return text;
    std::wstring roundTrip(roundTripCount, L'\0');
    if (MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, bytes.data(), size, roundTrip.data(), roundTripCount) != roundTripCount ||
        roundTrip != original) return text;
    return bytes;
}
