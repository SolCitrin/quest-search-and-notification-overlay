#include "Pins.h"

#include <mutex>
#include <set>

namespace QSO::Pins
{
    namespace
    {
        constexpr std::uint32_t kUniqueID = 'QSOP';
        constexpr std::uint32_t kPinRecord = 'PINS';  // 0.13.0
        constexpr std::uint32_t kHideRecord = 'HIDE';  // 0.14.0; a save without it simply has nothing hidden
        constexpr std::uint32_t kVersion = 1;          // both records: count + quest FormIDs

        std::mutex           lock;
        std::set<RE::FormID> pins;    // quest FormIDs as they are in the current load order
        std::set<RE::FormID> hidden;  // 0.14.0

        const char* NameOf(std::uint32_t a_record) { return a_record == kPinRecord ? "pins" : "hidden quests"; }

        void OnRevert(SKSE::SerializationInterface*)
        {
            std::scoped_lock guard(lock);
            pins.clear();
            hidden.clear();
        }

        void WriteSet(SKSE::SerializationInterface* a_intfc, std::uint32_t a_record, const std::set<RE::FormID>& a_set)
        {
            if (!a_intfc->OpenRecord(a_record, kVersion)) {
                logger::error("Pins: could not open the co-save record; {} {} not saved.", a_set.size(), NameOf(a_record));
                return;
            }
            const auto count = static_cast<std::uint32_t>(a_set.size());
            a_intfc->WriteRecordData(count);
            for (const auto id : a_set) {
                a_intfc->WriteRecordData(id);
            }
            logger::info("Pins: {} {} saved in the co-save.", count, NameOf(a_record));
        }

        void OnSave(SKSE::SerializationInterface* a_intfc)
        {
            std::scoped_lock guard(lock);
            WriteSet(a_intfc, kPinRecord, pins);
            WriteSet(a_intfc, kHideRecord, hidden);
        }

        void ReadSet(SKSE::SerializationInterface* a_intfc, std::uint32_t a_record, std::set<RE::FormID>& a_set)
        {
            std::uint32_t count = 0;
            if (!a_intfc->ReadRecordData(count)) {
                logger::error("Pins: co-save record for {} cut short.", NameOf(a_record));
                return;
            }
            std::uint32_t dropped = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                RE::FormID saved = 0, now = 0;
                if (!a_intfc->ReadRecordData(saved)) {
                    logger::error("Pins: co-save record for {} cut short after {} of {}.", NameOf(a_record), i, count);
                    break;
                }
                // Re-maps the mod index to today's load order; false = that mod is gone.
                if (!a_intfc->ResolveFormID(saved, now) || now == 0) {
                    ++dropped;
                    continue;
                }
                a_set.insert(now);
            }
            logger::info("Pins: {} {} read from the co-save ({} dropped: their mod is no longer loaded).", a_set.size(),
                NameOf(a_record), dropped);
        }

        void OnLoad(SKSE::SerializationInterface* a_intfc)
        {
            std::scoped_lock guard(lock);
            pins.clear();
            hidden.clear();
            std::uint32_t type = 0, version = 0, length = 0;
            while (a_intfc->GetNextRecordInfo(type, version, length)) {
                if (type != kPinRecord && type != kHideRecord) {
                    logger::warn("Pins: unknown co-save record {:08X} skipped.", type);
                    continue;
                }
                if (version != kVersion) {
                    logger::warn("Pins: co-save record version {} (expected {}) skipped.", version, kVersion);
                    continue;
                }
                ReadSet(a_intfc, type, type == kPinRecord ? pins : hidden);
            }
        }

        std::string ToJson(const std::set<RE::FormID>& a_set)
        {
            std::string out = "[";
            for (const auto id : a_set) {
                out += std::format(R"({}"{:08X}")", out.size() > 1 ? "," : "", id);
            }
            return out + "]";
        }
    }

    void Install()
    {
        const auto intfc = SKSE::GetSerializationInterface();
        if (!intfc) {
            logger::error("Pins: no SKSE serialization interface; pins and hidden quests last until the game closes.");
            return;
        }
        intfc->SetUniqueID(kUniqueID);
        intfc->SetRevertCallback(OnRevert);
        intfc->SetSaveCallback(OnSave);
        intfc->SetLoadCallback(OnLoad);
        logger::info("Pins: co-save callbacks registered (id QSOP: pins + hidden quests).");
    }

    bool Command(const std::string& a_text)
    {
        unsigned formID = 0;
        int      on = 0;
        const bool hide = a_text.starts_with("hide:");
        if (std::sscanf(a_text.c_str(), hide ? "hide:%8x:%d" : "pin:%8x:%d", &formID, &on) != 2 || (on != 0 && on != 1) ||
            formID == 0) {
            logger::warn("Pins: bad command '{}'.", a_text);
            return false;
        }
        std::scoped_lock guard(lock);
        auto&            set = hide ? hidden : pins;
        if (on) {
            set.insert(formID);
        } else {
            set.erase(formID);
        }
        logger::info("Pins: {:08X} {} ({} pinned, {} hidden).", formID,
            hide ? (on ? "hidden" : "unhidden") : (on ? "pinned" : "unpinned"), pins.size(), hidden.size());
        return true;
    }

    std::string Json()
    {
        std::scoped_lock guard(lock);
        return ToJson(pins);
    }

    std::string HiddenJson()
    {
        std::scoped_lock guard(lock);
        return ToJson(hidden);
    }

    bool IsHidden(RE::FormID a_quest)
    {
        std::scoped_lock guard(lock);
        return hidden.contains(a_quest);
    }
}
