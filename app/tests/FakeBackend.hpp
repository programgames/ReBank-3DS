#pragma once

#include "bank/commit/CommitBackend.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// A test Pokemon is a small integer id; 0 never names one.
using Mon = int;
// Occupied slots only, across the local boxes, the party and the cloud.
using State = std::map<SlotRef, Mon>;

inline PokemonPayload payloadOf(Mon mon) {
    return {7, {0x50, static_cast<std::uint8_t>(mon & 0xFF), static_cast<std::uint8_t>(mon >> 8)}};
}

inline Mon monOf(const PokemonPayload& payload) {
    return payload.data.size() == 3 ? payload.data[1] | (payload.data[2] << 8) : 0;
}

inline PokemonSummary summaryOf(Mon mon) {
    PokemonSummary summary;
    summary.species = static_cast<std::uint16_t>(mon);
    summary.nickname = "M" + std::to_string(mon);
    summary.format = 7;
    return summary;
}

inline void swapSlots(State& state, SlotRef a, SlotRef b) {
    const auto take = [&](const SlotRef& slot) {
        std::optional<Mon> mon;
        if (const auto found = state.find(slot); found != state.end()) {
            mon = found->second;
            state.erase(found);
        }
        return mon;
    };
    const std::optional<Mon> first = take(a);
    const std::optional<Mon> second = take(b);
    if (first) {
        state[b] = *first;
    }
    if (second) {
        state[a] = *second;
    }
}

// Packs the slots of one box (or of the party) to the front, keeping their order.
inline void compact(State& state, SlotKind kind, std::uint16_t box) {
    std::vector<Mon> members;
    for (auto it = state.begin(); it != state.end();) {
        if (it->first.kind == kind && it->first.box == box) {
            members.push_back(it->second);
            it = state.erase(it);
        } else {
            ++it;
        }
    }
    for (std::size_t slot = 0; slot < members.size(); ++slot) {
        state[{kind, box, static_cast<std::uint8_t>(slot)}] = members[slot];
    }
}

// Packs the party, and also every local box when `boxes` is set (Gen 1/2 boxes are lists).
inline void packLists(State& state, bool boxes) {
    compact(state, SlotKind::Party, 0);
    if (!boxes) {
        return;
    }
    std::set<std::uint16_t> indexes;
    for (const auto& entry : state) {
        if (entry.first.kind == SlotKind::Local) {
            indexes.insert(entry.first.box);
        }
    }
    for (const std::uint16_t index : indexes) {
        compact(state, SlotKind::Local, index);
    }
}

enum class Call : std::uint8_t {
    MoveCloud,
    UploadBatch,
    ReadCloudBox,
    DeleteCloud,
    ClaimCloud,
    WriteLocal,
    ClearLocal,
    PersistLocal,
    Count
};

enum class Mode : std::uint8_t {
    Refused,           // RemoteOutcome::Refused, no effect
    UnknownNotApplied, // RemoteOutcome::Unknown, no effect
    UnknownApplied,    // RemoteOutcome::Unknown, but the effect landed
    Fail,              // false, or std::nullopt for readCloudBox
    RejectFirst,       // uploadBatch: Refused with the first item in `rejected`, nothing stored
    ChangedFirst,      // claimCloud: the first claim is CloudClaim::Changed
    LockedFirst,       // claimCloud: the first claim is CloudClaim::Locked
    // Server and device behaviour described in issue #1.
    DoneMissingLast,      // uploadBatch: Done, but the server stored and counts one item fewer (#1 finding 1)
    RejectFirstStoreRest, // uploadBatch: Refused for the first item, the others stored (#1 finding 1)
    FailButLanded,        // persistLocal: false, but the save reached the disk (#1 finding 2)
    DeleteFirstThenFail,  // deleteCloud: the first slot is deleted, then Refused (#1 finding 7)
    UnknownCleared,       // uploadBatch: Unknown; the destinations end empty, nothing stored
    FailDiskUnknown       // persistLocal: false, the save reached the disk, but the disk could not be read back
};

using CallCounts = std::array<int, static_cast<std::size_t>(Call::Count)>;

// The modes each call of FakeBackend implements.
inline bool supports(Call call, Mode mode) {
    switch (call) {
        case Call::MoveCloud:
            return mode == Mode::Refused || mode == Mode::UnknownNotApplied || mode == Mode::UnknownApplied;
        case Call::UploadBatch:
            return mode == Mode::Refused || mode == Mode::UnknownNotApplied || mode == Mode::UnknownApplied
                || mode == Mode::RejectFirst || mode == Mode::DoneMissingLast || mode == Mode::RejectFirstStoreRest
                || mode == Mode::UnknownCleared;
        case Call::DeleteCloud:
            return mode == Mode::Refused || mode == Mode::UnknownNotApplied || mode == Mode::UnknownApplied
                || mode == Mode::DeleteFirstThenFail;
        case Call::ClaimCloud:
            return mode == Mode::Refused || mode == Mode::UnknownNotApplied || mode == Mode::ChangedFirst
                || mode == Mode::LockedFirst;
        case Call::PersistLocal:
            return mode == Mode::Fail || mode == Mode::FailButLanded || mode == Mode::FailDiskUnknown;
        case Call::ReadCloudBox:
        case Call::WriteLocal:
        case Call::ClearLocal:
            return mode == Mode::Fail;
        case Call::Count:
            break;
    }
    return false;
}

// Makes the `occurrence`-th call (1-based) of `call` behave as `mode`.
struct Fault {
    Call call;
    int occurrence;
    Mode mode;
};

// In-memory save and cloud. `working` is the save in memory, `disk` the save on disk, and
// `known` what the device last read from the disk (discardLocal reloads it).
class FakeBackend final : public CommitBackend {
public:
    State disk;
    State working;
    State known;
    bool diskKnown = true;
    State cloud;
    bool listBoxes = false;          // Gen 1/2: boxes and party are lists, packed on every write
    bool listingWithoutPayload = false; // readCloudBox lists occupied slots without their payload
    std::vector<Fault> faults;
    CallCounts calls{};

    RemoteResult moveCloud(const SlotRef& from, const SlotRef& to) override {
        const std::optional<Mode> mode = hit(Call::MoveCloud);
        if (mode == Mode::Refused) {
            return {RemoteOutcome::Refused, "injected"};
        }
        if (mode == Mode::UnknownNotApplied) {
            return {RemoteOutcome::Unknown, "injected"};
        }
        swapSlots(cloud, from, to);
        return {mode == Mode::UnknownApplied ? RemoteOutcome::Unknown : RemoteOutcome::Done, {}};
    }

    UploadBatchResult uploadBatch(const std::vector<UploadItem>& items) override {
        const std::optional<Mode> mode = hit(Call::UploadBatch);
        if (mode == Mode::Refused) {
            return {RemoteOutcome::Refused, "injected", {}};
        }
        if (mode == Mode::UnknownNotApplied) {
            return {RemoteOutcome::Unknown, "injected", {}};
        }
        if (mode == Mode::UnknownCleared) {
            for (const UploadItem& item : items) {
                cloud.erase(item.destination);
            }
            return {RemoteOutcome::Unknown, "injected", {}};
        }
        if (mode == Mode::RejectFirst || mode == Mode::RejectFirstStoreRest) {
            for (std::size_t index = 1; mode == Mode::RejectFirstStoreRest && index < items.size(); ++index) {
                cloud[items[index].destination] = monOf(items[index].payload);
            }
            return {RemoteOutcome::Refused, "injected", {{items.front().destination, "injected"}}};
        }
        std::vector<CloudRejection> rejected;
        for (const UploadItem& item : items) {
            if (cloud.count(item.destination) && !item.replaceOccupant) {
                rejected.push_back({item.destination, "occupied"});
            }
        }
        if (!rejected.empty()) {
            return {RemoteOutcome::Refused, "occupied", rejected};
        }
        const std::size_t stored = items.size() - (mode == Mode::DoneMissingLast ? 1 : 0);
        for (std::size_t index = 0; index < stored; ++index) {
            cloud[items[index].destination] = monOf(items[index].payload);
        }
        return {mode == Mode::UnknownApplied ? RemoteOutcome::Unknown : RemoteOutcome::Done, {}, {}, stored};
    }

    std::optional<CloudListing> readCloudBox(std::uint16_t boxKey) override {
        if (hit(Call::ReadCloudBox) == Mode::Fail) {
            return std::nullopt;
        }
        CloudListing listing{};
        for (const auto& [slot, mon] : cloud) {
            if (slot.box == boxKey) {
                listing[slot.slot] = {true, listingWithoutPayload ? PokemonPayload{} : payloadOf(mon)};
            }
        }
        return listing;
    }

    RemoteResult deleteCloud(const std::vector<SlotRef>& slots) override {
        const std::optional<Mode> mode = hit(Call::DeleteCloud);
        if (mode == Mode::Refused) {
            return {RemoteOutcome::Refused, "injected"};
        }
        if (mode == Mode::UnknownNotApplied) {
            return {RemoteOutcome::Unknown, "injected"};
        }
        if (mode == Mode::DeleteFirstThenFail) {
            cloud.erase(slots.front());
            return {RemoteOutcome::Refused, "injected"};
        }
        for (const SlotRef& slot : slots) {
            cloud.erase(slot);
        }
        return {mode == Mode::UnknownApplied ? RemoteOutcome::Unknown : RemoteOutcome::Done, {}};
    }

    ClaimBatchResult claimCloud(const std::vector<ClaimItem>& items) override {
        const std::optional<Mode> mode = hit(Call::ClaimCloud);
        if (mode == Mode::Refused) {
            return {RemoteOutcome::Refused, "injected", {}, {}};
        }
        if (mode == Mode::UnknownNotApplied) {
            return {RemoteOutcome::Unknown, "injected", {}, {}};
        }
        ClaimBatchResult result{RemoteOutcome::Done, {}, {}, {}};
        for (const ClaimItem& item : items) {
            const auto occupant = cloud.find(item.slot);
            const bool same = occupant != cloud.end() && occupant->second == monOf(item.payload);
            result.claims.push_back(same ? CloudClaim::Claimed : CloudClaim::Changed);
        }
        if (mode == Mode::ChangedFirst) {
            result.claims.front() = CloudClaim::Changed;
        } else if (mode == Mode::LockedFirst) {
            result.claims.front() = CloudClaim::Locked;
        }
        return result;
    }

    bool writeLocal(const SlotRef& slot, const PokemonPayload& payload, bool) override {
        if (hit(Call::WriteLocal) == Mode::Fail) {
            return false;
        }
        working[slot] = monOf(payload);
        return true;
    }

    bool clearLocal(const SlotRef& slot) override {
        if (hit(Call::ClearLocal) == Mode::Fail) {
            return false;
        }
        working.erase(slot);
        return true;
    }

    PokemonPayload readLocal(const SlotRef& slot) override {
        const auto found = working.find(slot);
        return found == working.end() ? PokemonPayload{} : payloadOf(found->second);
    }

    // Mirrors SaveAdapter::writeSave: every write packs the party, and a Gen 1/2 save
    // also packs its boxes.
    bool persistLocal(std::string& error) override {
        const std::optional<Mode> mode = hit(Call::PersistLocal);
        if (mode == Mode::Fail) {
            error = "injected";
            return false;
        }
        packLists(working, listBoxes);
        disk = working;
        if (mode == Mode::FailDiskUnknown) {
            diskKnown = false;
            error = "injected";
            return false;
        }
        known = disk;
        diskKnown = true;
        if (mode == Mode::FailButLanded) {
            error = "injected";
            return false;
        }
        return true;
    }

    bool discardLocal() override {
        working = known;
        return diskKnown;
    }

private:
    std::optional<Mode> hit(Call call) {
        const int occurrence = ++calls[static_cast<std::size_t>(call)];
        for (const Fault& fault : faults) {
            if (fault.call == call && fault.occurrence == occurrence) {
                if (!supports(call, fault.mode)) {
                    std::fprintf(stderr, "FakeBackend: this call does not implement the injected mode\n");
                    std::abort();
                }
                return fault.mode;
            }
        }
        return std::nullopt;
    }
};
