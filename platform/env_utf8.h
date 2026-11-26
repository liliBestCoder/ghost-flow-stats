#pragma once
// UTF-8 safe environment variable access, shared by the injected core and its
// unit tests.
//
// Why this exists: GHOST_* config travels from the UI to injected processes
// through the environment block, which the UI writes as UTF-16 (it launches
// targets with CREATE_UNICODE_ENVIRONMENT). Reading those variables back with
// the ANSI API makes Windows transcode UTF-16 -> CP_ACP, so on a CP936 (GBK)
// system a node name like "江西南昌" comes back as GBK bytes. Those bytes then
// fail nlohmann's strict UTF-8 validation in dump() and take the process down.
//
// Design invariants:
//   * ALWAYS the wide API + explicit CP_UTF8 — never CP_ACP, never the A
//     variants, so the exact bytes survive regardless of the system locale.
//   * No fixed-size buffer: the length is queried first, so long values are
//     never truncated (a truncated multi-byte sequence is itself invalid UTF-8).
//   * Get returns false ONLY when the variable is absent; a present-but-empty
//     variable returns true with out = "".
#include <windows.h>
#include <string>

namespace ghost_env {

// Convert a UTF-8 string to UTF-16. Returns an empty string for empty input.
inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
    if (len <= 0) return std::wstring();
    std::wstring w((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], len);
    return w;
}

// Convert a UTF-16 string to UTF-8. Returns an empty string for empty input.
//
// One caller: LogUwp in src/entry/launcher_uwp/uwp_main.cpp, which has wide
// messages and a narrow UTF-8 sink. Here rather than written out there, because
// a second conversion inside a launcher is how that same file once ended up
// carrying its own copy of GrantAppContainerPermissions.
inline std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), NULL, 0, NULL, NULL);
    if (len <= 0) return std::string();
    std::string s((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &s[0], len, NULL, NULL);
    return s;
}

// Read an environment variable as UTF-8, via the wide API so non-ASCII
// (CJK/emoji) values survive. Returns false only when the var is absent;
// a present-but-empty var returns true with out="". No fixed-size buffer, so
// long values are never truncated / read as undefined memory.
inline bool GetEnvUtf8(const wchar_t* name, std::string& out) {
    out.clear();
    DWORD need = GetEnvironmentVariableW(name, NULL, 0); // size incl. NUL, or 0 if absent
    if (need == 0) return false;
    std::wstring w(need, L'\0');
    DWORD got = GetEnvironmentVariableW(name, &w[0], need);
    if (got == 0) return true; // present but empty
    // got >= need means the value grew between the probe and the read: nothing
    // was copied and got is the newly required size. Report absence rather than
    // handing back the untouched (all-NUL) buffer as if it were the value.
    if (got >= need) return false;
    w.resize(got);
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (len > 0) {
        out.resize((size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, NULL, NULL);
    }
    return true;
}

// Write a UTF-8 value to an environment variable via the wide API, so child
// processes inherit it without an ANSI (CP_ACP) round-trip that would mangle
// non-ASCII bytes.
inline void SetEnvUtf8(const wchar_t* name, const std::string& utf8) {
    SetEnvironmentVariableW(name, Utf8ToWide(utf8).c_str());
}

// Same, for a variable name that is itself only known at runtime as UTF-8
// (targets.json "env" pass-through).
inline void SetEnvUtf8(const std::string& name, const std::string& utf8) {
    std::wstring wname = Utf8ToWide(name);
    if (wname.empty()) return;
    SetEnvironmentVariableW(wname.c_str(), Utf8ToWide(utf8).c_str());
}

// Cut a UTF-8 string to at most maxBytes, never mid-sequence. Values that reach
// fixed-size C buffers (the stats datagram, NetLog) must be bounded, and a
// naive byte cut would leave a partial sequence -- itself invalid UTF-8, which
// is the failure this whole header exists to prevent.
inline std::string TruncateUtf8(const std::string& utf8, size_t maxBytes) {
    if (utf8.size() <= maxBytes) return utf8;
    size_t cut = maxBytes;
    // Walk back off any continuation byte (10xxxxxx) to a lead byte.
    while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0) == 0x80) --cut;
    return utf8.substr(0, cut);
}

// Upper bound for the node name. It is user-supplied and free-form, and it is
// formatted into fixed 1024-byte buffers downstream, so it is clamped once at
// the point it enters the config rather than at each sink.
static const size_t kMaxNodeNameBytes = 192;

// The [stats] datagram is '|'-delimited with no escaping, so a value carried in
// it must not contain the delimiter: a node name holding "|dns:" or
// "|blocked:" would otherwise impersonate a later field on the parsing side.
// Both ends run this over the node name so the sanitized forms still compare
// equal when the UI matches a reported node against the configured one.
inline std::string SanitizeStatField(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(out[i]);
        if (c == '|' || c < 0x20) out[i] = '/';
    }
    return out;
}

} // namespace ghost_env
