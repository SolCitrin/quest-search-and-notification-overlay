#include "Capture.h"
#include "B612.h"
#include "Boxes.h"
#include "History.h"
#include "Settings.h"

#include <mutex>

// Every plain notification line passes here. M2: it is logged, then taken for our boxes or left to vanilla.
// 0.16.0: moved unchanged from HUD Notification Replacer 0.2.0 (src/Capture.cpp) into QSO's capture part. Only the
// namespaces, the "Capture: " log prefix, the switch (Settings::CaptureOn(), live from the Settings page) and the
// hand-over to the shared box stack (Boxes::PushLine) changed. Installed only if [Capture] bEnabled was on at startup.
//
// Where the hook sits (docs/RESEARCH-INTERCEPT.md section 1 and 5): HUDMenu::ProcessMessage handles a HUDData
// kNotification by calling GFxValue::ObjectInterface::Invoke(root, "ShowMessage", [text]) once. That one call is
// the last stop before hudmenu.swf, so every sender passes it (Debug.Notification, RE::DebugNotification,
// SendHUDMessage::ShowHUDMessage callers such as Experience's "+N XP", raw AddMessage posters), and every other
// mod's ProcessMessage hook has already run. We replace that one `call` (write_call<5>) after checking its bytes;
// if the bytes are not what we expect we write nothing there and hook HUDMenu's vtable slot 0x4 instead.
//
// The design follows Notification Tweaks (Nana1873, GPL-3.0); no code is copied from it.

namespace
{
    // ---- Addresses (CommonLibSSE-NG b93280e Offsets.h; offsets per research doc / Notification Tweaks notes) ----
    constexpr REL::RelocationID kProcessMessageId{ 50718, 51612 };  // HUDMenu::ProcessMessage (vtable slot 0x4)
    constexpr REL::RelocationID kInvokeId{ 80233, 82256 };          // GFxValue::ObjectInterface::Invoke
    constexpr std::size_t       kCallOffsetAE = 0x686;              // AE 1.6.1170 and 1.7.104 (read statically 2026-11-25); SE 1.5.97 would be 0x65E
    constexpr std::size_t       kLeaBeforeCall = 0x15;              // lea r9, "ShowMessage" sits 0x15 bytes earlier
    constexpr std::uint8_t      kCallOpcode = 0xE8;                 // call rel32
    constexpr std::uint8_t      kLeaR9[] = { 0x4C, 0x8D, 0x0D };    // lea r9, [rip+disp32]
    constexpr std::size_t       kLeaLength = 7;
    constexpr char              kMethod[] = "ShowMessage";          // compared WITH its terminator (12 bytes)

    // MEMORY_BASIC_INFORMATION values (CommonLib's trimmed Windows headers lack some macros).
    constexpr std::uint32_t kMemCommit = 0x1000;
    constexpr std::uint32_t kPageNoAccess = 0x01;
    constexpr std::uint32_t kPageGuard = 0x100;

    enum class Path
    {
        kNone,
        kCallsite,
        kVtable
    };
    std::atomic<Path> g_path{ Path::kNone };
    std::uint32_t     g_mainThread = 0;
    std::uint64_t     g_seq = 0;                // under g_hookLock
    std::chrono::steady_clock::time_point g_last{};
    std::atomic<bool> g_dumpB612{ true };       // list B612's items with the next line

    // ---- Small helpers ----

    std::string Hex(std::uintptr_t a_at, std::size_t a_count)
    {
        std::string out;
        out.reserve(a_count * 3);
        for (std::size_t i = 0; i < a_count; ++i) {
            out += std::format("{:02X}", *reinterpret_cast<const std::uint8_t*>(a_at + i));
            if (i + 1 < a_count) {
                out += ' ';
            }
        }
        return out;
    }

    std::int32_t Rel32(std::uintptr_t a_at)
    {
        std::int32_t value;
        std::memcpy(&value, reinterpret_cast<const void*>(a_at), sizeof(value));
        return value;
    }

    // True if a_count bytes at a_at are committed, readable memory (so peeking at a hook target cannot fault).
    bool Readable(std::uintptr_t a_at, std::size_t a_count)
    {
        REX::W32::MEMORY_BASIC_INFORMATION info{};
        if (!REX::W32::VirtualQuery(reinterpret_cast<const void*>(a_at), &info, sizeof(info))) {
            return false;
        }
        if (info.state != kMemCommit || (info.protect & kPageNoAccess) || (info.protect & kPageGuard)) {
            return false;
        }
        const auto end = reinterpret_cast<std::uintptr_t>(info.baseAddress) + info.regionSize;
        return a_at + a_count <= end;
    }

    // The DLL / exe a code address belongs to, or "no module (hook trampoline?)".
    std::string OwnerOf(std::uintptr_t a_at)
    {
        REX::W32::MEMORY_BASIC_INFORMATION info{};
        if (!REX::W32::VirtualQuery(reinterpret_cast<const void*>(a_at), &info, sizeof(info)) || !info.allocationBase) {
            return "unknown";
        }
        wchar_t path[260]{};
        const auto len = REX::W32::GetModuleFileNameW(static_cast<REX::W32::HMODULE>(info.allocationBase), path, 260);
        if (!len) {
            return std::format("no module (hook trampoline?) base {:X}", reinterpret_cast<std::uintptr_t>(info.allocationBase));
        }
        std::wstring_view full(path, len);
        const auto slash = full.find_last_of(L"\\/");
        const auto name = slash == std::wstring_view::npos ? full : full.substr(slash + 1);
        std::string out;
        for (const auto c : name) {
            out += c < 0x80 ? static_cast<char>(c) : '?';
        }
        return out;
    }

    // Raw text made log-safe: printable ASCII kept as is, everything else as \xNN, so the log shows the exact bytes
    // (and so whether a line is UTF-8 or Windows-1252). Also reports whether the bytes form valid UTF-8.
    struct Escaped
    {
        std::string text;
        std::size_t bytes = 0;
        bool        highBytes = false;
        bool        validUtf8 = true;
    };

    Escaped Escape(const char* a_text)
    {
        Escaped out;
        if (!a_text) {
            out.text = "<null>";
            return out;
        }
        const auto* p = reinterpret_cast<const std::uint8_t*>(a_text);
        std::size_t n = std::strlen(a_text);
        out.bytes = n;
        constexpr std::size_t kCap = 2048;
        const std::size_t     shown = std::min(n, kCap);
        for (std::size_t i = 0; i < shown; ++i) {
            const auto c = p[i];
            if (c == '\\') {
                out.text += "\\\\";
            } else if (c == '\n') {
                out.text += "\\n";
            } else if (c >= 0x20 && c < 0x7F) {
                out.text += static_cast<char>(c);
            } else {
                out.text += std::format("\\x{:02X}", c);
            }
        }
        if (shown < n) {
            out.text += std::format("...(+{} bytes)", n - shown);
        }
        // UTF-8 validity over the whole string.
        for (std::size_t i = 0; i < n;) {
            const auto c = p[i];
            if (c < 0x80) {
                ++i;
                continue;
            }
            out.highBytes = true;
            std::size_t extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 99;
            if (extra == 99 || c == 0xC0 || c == 0xC1 || i + extra >= n + 1) {
                out.validUtf8 = false;
                break;
            }
            for (std::size_t k = 1; k <= extra; ++k) {
                if (i + k >= n || (p[i + k] & 0xC0) != 0x80) {
                    out.validUtf8 = false;
                    break;
                }
            }
            if (!out.validUtf8) {
                break;
            }
            i += extra + 1;
        }
        return out;
    }

    std::string EscapeWide(const wchar_t* a_text)
    {
        if (!a_text) {
            return "<null>";
        }
        std::string out;
        for (const wchar_t* w = a_text; *w && out.size() < 4096; ++w) {
            const auto c = static_cast<std::uint16_t>(*w);
            if (c == '\\') {
                out += "\\\\";
            } else if (c >= 0x20 && c < 0x7F) {
                out += static_cast<char>(c);
            } else {
                out += std::format("\\u{:04X}", c);
            }
        }
        return out;
    }

    // A short description of a non-string GFx argument, for the log.
    std::string Describe(const RE::GFxValue& a_value)
    {
        if (a_value.IsNumber()) {
            return std::format("number {}", a_value.GetNumber());
        }
        if (a_value.IsBool()) {
            return a_value.GetBool() ? "bool true" : "bool false";
        }
        if (a_value.IsUndefined()) {
            return "undefined";
        }
        if (a_value.IsNull()) {
            return "null";
        }
        return std::format("<type {}>", static_cast<int>(a_value.GetType()));
    }

    // ---- The decision, per line (on the hook's thread) ----
    //
    // M1 showed the hook running on varying NON-main threads, so nothing here touches menus or Prisma: the line is
    // copied and handed to the main thread (SKSE AddTask), where Boxes decides show / hold. What must be decided here,
    // synchronously, is only whether vanilla gets the line, from facts that are safe to read on this thread: the
    // kill switch, the page-ready flag (atomic) and B612's items (in the HUD movie, which this thread is driving).

    enum class Fate
    {
        kBox,             // taken: vanilla does not show it, our box will
        kVanillaOff,      // [Capture] bEnabled=0 (switched off after startup): log only
        kVanillaB612,     // B612 claims it: vanilla + B612 handle it as without HNR
        kVanillaNotReady  // our page is not loaded (yet): vanilla shows it
    };

    const char* FateName(Fate a_fate)
    {
        switch (a_fate) {
        case Fate::kBox:
            return "box";
        case Fate::kVanillaOff:
            return "vanilla-disabled";
        case Fate::kVanillaB612:
            return "vanilla-B612";
        default:
            return "vanilla-not-ready";
        }
    }

    std::mutex g_hookLock;  // the hook may run on different threads; keep its counters consistent

    // Returns true when the line is taken (the caller must then NOT call the original).
    bool Decide(Path a_path, const char* a_text, const char* a_note)
    {
        std::scoped_lock lock(g_hookLock);
        const auto       now = std::chrono::steady_clock::now();
        const auto       gapMs = g_seq ? std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last).count() : -1;
        g_last = now;
        const auto seq = ++g_seq;

        const std::string text = a_text ? a_text : "";
        const bool        logItems = g_dumpB612.exchange(false);

        Fate fate;
        if (!QSO::Settings::CaptureOn()) {
            fate = Fate::kVanillaOff;
            if (logItems) {
                QSO::B612::Claims(text, true);  // still list them: useful in the log
            }
        } else if (QSO::B612::Claims(text, logItems)) {
            fate = Fate::kVanillaB612;
        } else if (!QSO::Boxes::Ready()) {
            fate = Fate::kVanillaNotReady;
        } else {
            fate = Fate::kBox;
        }

        const auto e = Escape(a_text);
        const auto thread = REX::W32::GetCurrentThreadId();
        logger::info("Capture: #{} path={} gap={}ms thread={} bytes={} enc={}{} text=\"{}\" -> {}", seq,
            a_path == Path::kCallsite ? "callsite" : "vtable", gapMs,
            thread == g_mainThread ? "main" : std::format("OTHER({})", thread), e.bytes,
            !e.highBytes ? "ascii" : e.validUtf8 ? "utf8" : "NOT-utf8(cp1252?)", a_note ? a_note : "", e.text, FateName(fate));

        QSO::History::AddLine(seq, text, FateName(fate));  // 0.17.0: every line the hook sees, shown or not
        if (fate != Fate::kBox) {
            return false;
        }
        SKSE::GetTaskInterface()->AddTask([seq, text] { QSO::Boxes::PushLine(seq, text); });
        return true;
    }

    // ---- Path 1: the ShowMessage call site ----

    struct ShowMessageCall
    {
        static bool thunk(RE::GFxValue::ObjectInterface* a_self, void* a_data, RE::GFxValue* a_result, const char* a_name,
            const RE::GFxValue* a_args, RE::UPInt a_numArgs, bool a_isDObj)
        {
            try {
                if (!a_name || std::strcmp(a_name, kMethod) != 0) {
                    logger::warn("Capture: call site: method is \"{}\", not ShowMessage (passed on)", a_name ? a_name : "<null>");
                } else if (!a_args || a_numArgs == 0) {
                    logger::warn("Capture: call site: ShowMessage with no arguments (passed on)");
                } else {
                    const auto& v = a_args[0];
                    const auto  note = a_numArgs != 1 ? std::format(" args={}", a_numArgs) : std::string{};
                    if (v.IsString()) {
                        if (Decide(Path::kCallsite, v.GetString(), note.empty() ? nullptr : note.c_str())) {
                            return true;  // taken: the swf never gets it (the caller ignores this result)
                        }
                    } else {
                        // Wide strings never appeared in M1 (51 lines); leave anything unusual to vanilla.
                        logger::info("Capture: call site: ShowMessage argument is not a plain string ({}{}); left to vanilla",
                            v.IsStringW() ? "wide string" : Describe(v), note);
                    }
                }
            } catch (...) {
                // Never let a problem here reach the game; the original call below shows the line.
            }
            return func(a_self, a_data, a_result, a_name, a_args, a_numArgs, a_isDObj);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // ---- Path 2 (fallback): HUDMenu vtable slot 0x4 ----

    struct ProcessMessageHook
    {
        static RE::UI_MESSAGE_RESULTS thunk(RE::HUDMenu* a_this, RE::UIMessage& a_message)
        {
            try {
                if (a_message.type == RE::UI_MESSAGE_TYPE::kUpdate && a_message.data) {
                    if (const auto* data = skyrim_cast<RE::HUDData*>(a_message.data);
                        data && data->type.get() == RE::HUD_MESSAGE_TYPE::kNotification && Decide(Path::kVtable, data->text.c_str(), nullptr)) {
                        return RE::UI_MESSAGE_RESULTS::kHandled;  // taken: vanilla never sees this notification
                    }
                }
            } catch (...) {
            }
            return func(a_this, a_message);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };
    // ---- Call-site checks ----

    struct SiteCheck
    {
        std::uintptr_t site = 0;
        bool           opcode = false;
        bool           callee = false;
        bool           lea = false;
        bool           method = false;
        std::uintptr_t calleeAt = 0;
        bool           Ok() const { return opcode && callee && lea && method; }
    };

    SiteCheck CheckSite(bool a_verbose)
    {
        SiteCheck c;
        const auto base = REL::Module::get().base();
        c.site = kProcessMessageId.address() + kCallOffsetAE;
        const auto invoke = kInvokeId.address();

        if (a_verbose) {
            logger::info("Capture:   HUDMenu::ProcessMessage = SkyrimSE+{:X}; call site = +0x{:X} -> SkyrimSE+{:X}",
                kProcessMessageId.address() - base, kCallOffsetAE, c.site - base);
            logger::info("Capture:   16 bytes at the site: {}", Hex(c.site, 16));
            logger::info("Capture:   7 bytes 0x15 earlier (expect 4C 8D 0D = lea r9): {}", Hex(c.site - kLeaBeforeCall, kLeaLength));
        }

        c.opcode = *reinterpret_cast<const std::uint8_t*>(c.site) == kCallOpcode;
        if (c.opcode) {
            c.calleeAt = c.site + 5 + Rel32(c.site + 1);
            c.callee = c.calleeAt == invoke;
        }
        const auto lea = c.site - kLeaBeforeCall;
        c.lea = std::memcmp(reinterpret_cast<const void*>(lea), kLeaR9, sizeof(kLeaR9)) == 0;
        std::uintptr_t str = 0;
        if (c.lea) {
            str = lea + kLeaLength + Rel32(lea + sizeof(kLeaR9));
            c.method = Readable(str, sizeof(kMethod)) && std::memcmp(reinterpret_cast<const void*>(str), kMethod, sizeof(kMethod)) == 0;
        }

        if (a_verbose) {
            logger::info("Capture:   check 1, E8 call opcode: {}", c.opcode ? "OK" : std::format("FAIL (byte {:02X})", *reinterpret_cast<const std::uint8_t*>(c.site)));
            if (c.opcode) {
                logger::info("Capture:   check 2, callee is Invoke (SkyrimSE+{:X}): {} (callee SkyrimSE+{:X}, owner {})",
                    invoke - base, c.callee ? "OK" : "FAIL", c.calleeAt - base, OwnerOf(c.calleeAt));
                // Another mod's write_call points the call at a trampoline stub, usually `FF 25` = jmp [rip+0].
                if (!c.callee && Readable(c.calleeAt, 14)) {
                    logger::info("Capture:   callee's first 14 bytes: {}", Hex(c.calleeAt, 14));
                    if (*reinterpret_cast<const std::uint8_t*>(c.calleeAt) == 0xFF && *reinterpret_cast<const std::uint8_t*>(c.calleeAt + 1) == 0x25) {
                        const auto slot = c.calleeAt + 6 + Rel32(c.calleeAt + 2);
                        if (Readable(slot, 8)) {
                            std::uintptr_t target;
                            std::memcpy(&target, reinterpret_cast<const void*>(slot), sizeof(target));
                            logger::info("Capture:   ...jumps to {:X}, owner {}", target, OwnerOf(target));
                        }
                    }
                }
            } else {
                logger::info("Capture:   check 2, callee is Invoke: not checked (no E8)");
            }
            logger::info("Capture:   check 3, lea r9 0x15 bytes earlier: {}", c.lea ? "OK" : "FAIL");
            if (c.lea) {
                logger::info("Capture:   check 3b, lea target is \"ShowMessage\\0\": {} (target {:X}{})", c.method ? "OK" : "FAIL", str,
                    Readable(str, 12) ? std::format(", bytes {}", Hex(str, 12)) : ", unreadable");
            } else {
                logger::info("Capture:   check 3b, lea target string: not checked");
            }
        }
        return c;
    }
}

namespace QSO::Capture
{
    void LogEarlyBytes()
    {
        try {
            logger::info("Capture: Plugin load: call-site bytes before kDataLoaded (compare with the kDataLoaded check below):");
            const auto c = CheckSite(false);
            logger::info("Capture:   16 bytes at the site: {} -> {}", Hex(c.site, 16), c.Ok() ? "all checks OK" : "checks FAIL");
        } catch (...) {
            logger::warn("Capture: Plugin load: could not read the call site.");
        }
    }

    void Install()
    {
        g_mainThread = REX::W32::GetCurrentThreadId();
        const auto version = REL::Module::get().version();
        logger::info("Capture: kDataLoaded (main thread {}): checking the ShowMessage call site on {}:", g_mainThread, version.string());

        const auto c = CheckSite(true);
        if (c.Ok()) {
            ShowMessageCall::func = SKSE::GetTrampoline().write_call<5>(c.site, ShowMessageCall::thunk);
            g_path = Path::kCallsite;
            QSO::Boxes::SetCaptureHooked(true);
            logger::info("Capture: ACTIVE PATH: callsite (write_call<5> on the ShowMessage Invoke call). Lines are logged, then taken for boxes or left to vanilla (see each line).");
            return;
        }

        logger::warn("Capture: Call-site checks failed: NOT writing there. FALLBACK: hooking HUDMenu vtable slot 0x4 (kNotification only).");
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_HUDMenu[0] };
        ProcessMessageHook::func = vtbl.write_vfunc(0x4, ProcessMessageHook::thunk);
        const auto previous = ProcessMessageHook::func.address();
        logger::info("Capture:   previous slot 0x4 = {:X}, owner {} ({})", previous, OwnerOf(previous),
            previous == kProcessMessageId.address() ? "vanilla" : "NOT vanilla: another mod hooked it first");
        g_path = Path::kVtable;
        QSO::Boxes::SetCaptureHooked(true);
        logger::warn("Capture: ACTIVE PATH: vtable (fallback). Lines are logged, then taken for boxes or left to vanilla (see each line).");
    }

    void OnGameLoaded()
    {
        // Runs on the main thread (SKSE messaging), as do the hooks.
        g_dumpB612 = true;
        logger::info("Capture: Game loaded: B612's items will be listed with the next line. Active path: {}.",
            g_path == Path::kCallsite ? "callsite" : g_path == Path::kVtable ? "vtable" : "none");
    }
}
