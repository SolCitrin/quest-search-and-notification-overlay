#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace QSO::Quests
{
    struct Objective
    {
        std::uint16_t index = 0;
        std::uint32_t state = 0;  // RE::QUEST_OBJECTIVE_STATE of this instance
        std::string   text;       // with <Alias=...> tags replaced by the game
        std::uint32_t mapTarget = 0;  // form ID the vanilla journal's "Show on map" would centre on; 0 = none
        // Where the objective's target is (0.9.0), from the target reference's location: the most specific named
        // location, the city/town/settlement above it, and the hold. Empty when unknown.
        std::uint32_t targetRef = 0;   // the real target (not routed to a door), 0 = none
        std::uint32_t location = 0;    // BGSLocation form ID of the target's location, 0 = none
        std::string   place;
        std::string   town;
        std::string   hold;
        // Nearby (0.10.0): the "area" the target is in (town/city, else the dungeon or site directly under the hold)
        // and its hold, as BGSLocation form IDs; 0 = unknown.
        std::uint32_t area = 0;
        std::uint32_t holdID = 0;
        std::string   areaName;
        // Distance (0.10.4): metres from the player to the target, both taken outdoors (an indoor spot counts as the
        // door that leads out of it, through up to a few doors). -1 = unknown or not in the player's world.
        float         distance = -1.0f;
    };

    // 0.15.0: a note (BOOK) the quest points at through a created or forced reference alias (Missives' missive, a
    // bounty letter...). Read-only: the text comes from the book record; the item itself is never touched.
    struct Note
    {
        std::uint32_t book = 0;  // TESObjectBOOK form ID
        std::string   name;      // the book's name, tags replaced ("Missive: Gather Iron Ore")
        std::string   text;      // tags replaced, book markup stripped, capped at about 2,000 characters
    };

    // One row of the list = one quest instance (radiant quests can have several instances).
    struct Entry
    {
        std::uint32_t          formID = 0;
        std::uint32_t          instanceID = 0;
        std::string            name;  // with tags replaced
        std::string            editorID;
        std::string            plugin;
        std::uint8_t           type = 0;  // RE::QUEST_DATA::Type
        bool                   active = false;  // tracked (markers shown)
        bool                   completed = false;
        bool                   failed = false;
        std::uint16_t          stage = 0;
        std::string            description;  // the journal's text for this quest right now, tags replaced
        int                    newestAge = 0;  // position of its newest objective; higher = newer
        std::vector<Objective> objectives;     // newest first, like the journal
        // 0.15.0: notes of a running, not completed quest that the player holds or has read (spoiler guard).
        // Filled only by Read(false) (the overlay), never by the quiet nearby reads.
        std::vector<Note>      notes;
    };

    struct Snapshot
    {
        bool               playerListValid = false;
        std::string        problem;  // why the player list was rejected, if it was
        std::size_t        instanceCount = 0;
        std::size_t        hiddenCount = 0;  // entries skipped because the journal does not show them
        std::size_t        mapTargets = 0;   // open objectives that have a map target
        std::size_t        located = 0;      // open objectives whose target has a location
        // Where the player is, by the same rule as Objective::area / holdID.
        std::uint32_t      hereArea = 0;
        std::uint32_t      hereHold = 0;
        std::string        hereAreaName;
        std::string        hereHoldName;
        bool               herePlaced = false;  // the player's outdoor position is known (distances are valid)
        std::vector<Entry> entries;  // newest first
    };

    // Main thread only. Builds the list from the player's own objective list (what the journal and
    // Compass Navigation Overhaul read), checked against the quests' objective records first.
    // a_quiet: no log line (used by the nearby check on every location change).
    Snapshot Read(bool a_quiet = false);

    // The "area" rule shared by objectives and the player (0.10.0). Area = the first location up the chain that is a
    // city, town or settlement; otherwise the top-most location below the hold (a dungeon, fort, farm, camp...).
    // A location that IS a hold, or wilderness with no location, has no area.
    struct Area
    {
        std::uint32_t area = 0;
        std::uint32_t hold = 0;
        std::string   areaName;
        std::string   holdName;
    };
    // a_where (optional): the reference standing there (the player, or an objective's target). 0.10.2: the game's
    // location data counts the land around a walled city (bridge, stables, farms) as the city itself; the walled
    // cities have their own worldspace behind the gate, so an exterior spot in a top-level worldspace whose area is a
    // City counts only as the hold. Interiors (Dragonsreach) and the city's own worldspace still count as the city.
    Area AreaOf(RE::BGSLocation* a_location, const RE::TESObjectREFR* a_where = nullptr);

    // 0.10.4: where a reference is "outdoors": its own position if it is in an exterior cell, otherwise the outside
    // door of its interior (found through the interior's load doors, up to a few rooms deep). The world is the
    // top-level worldspace: a walled city's own worldspace shares its parent's coordinates (Whiterun -> Tamriel).
    struct OutdoorSpot
    {
        std::uint32_t world = 0;  // top-level worldspace form ID; 0 = unknown
        float         x = 0, y = 0, z = 0;
    };
    OutdoorSpot OutdoorSpotOf(RE::TESObjectREFR* a_ref);

    // Writes the snapshot plus a cross-check against the quest records to the log.
    // a_rows false = summary lines only (rows are written on the first dump of a session).
    void DumpToLog(const Snapshot& a_snapshot, bool a_rows = true);

    // The snapshot as JSON for the overlay page. Strings are made valid UTF-8 (bytes that are not UTF-8 are read
    // as Windows-1252, the encoding of English Skyrim string files).
    std::string ToJson(const Snapshot& a_snapshot);

    const char* TypeName(std::uint8_t a_type);

    // 0.11.0: one JSON string literal (quotes included), made valid UTF-8 the same way as ToJson.
    std::string JsonString(std::string_view a_text);

    // Main thread only. The map target of one shown objective, recomputed now (0 = it has none any more).
    std::uint32_t FindMapTarget(std::uint32_t a_questFormID, std::uint32_t a_instanceID, std::uint16_t a_objectiveIndex);

    // Main thread only. Opens the world map centred on a_target through the journal's own handler. False = not sent.
    bool ShowOnMap(std::uint32_t a_target);

    // Forget cached journal descriptions (call when a save is loaded or a new game starts).
    void ClearCache();
}
