#pragma once

#include "bank/commit/CommitPlan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class RemoteOutcome : std::uint8_t {
    Done,
    Refused,
    Unknown
};

struct RemoteResult {
    RemoteOutcome outcome = RemoteOutcome::Unknown;
    std::string message;
};

struct CloudRejection {
    SlotRef slot;
    std::string reason;
};

struct UploadBatchResult {
    RemoteOutcome outcome = RemoteOutcome::Unknown;
    std::string message;
    std::vector<CloudRejection> rejected;
    std::size_t stored = 0; // the number of items the server says it stored, when outcome is Done
};

enum class CloudClaim : std::uint8_t {
    Claimed,
    Changed,
    Locked
};

struct ClaimBatchResult {
    RemoteOutcome outcome = RemoteOutcome::Unknown;
    std::string message;
    std::vector<CloudClaim> claims;
    std::vector<std::optional<PokemonPayload>> received;
};

struct ClaimItem {
    SlotRef slot;
    PokemonPayload payload;
};

// One slot of a cloud box listing. The listing can name an occupied slot without its payload.
struct ListedSlot {
    bool occupied = false;
    PokemonPayload payload;
};

using CloudListing = std::array<ListedSlot, BoxSlotCount>;

struct UploadItem {
    SlotRef destination;
    PokemonSummary summary;
    PokemonPayload payload;
    bool replaceOccupant = false;
};

class CommitBackend {
public:
    virtual ~CommitBackend() = default;
    virtual RemoteResult moveCloud(const SlotRef& from, const SlotRef& to) = 0;
    virtual UploadBatchResult uploadBatch(const std::vector<UploadItem>& items) = 0;
    virtual std::optional<CloudListing> readCloudBox(std::uint16_t boxKey) = 0;
    virtual RemoteResult deleteCloud(const std::vector<SlotRef>& slots) = 0;
    virtual ClaimBatchResult claimCloud(const std::vector<ClaimItem>& items) = 0;
    virtual bool writeLocal(const SlotRef& slot, const PokemonPayload& payload) = 0;
    virtual bool clearLocal(const SlotRef& slot) = 0;
    // The Pokemon in a slot of the save in memory; an empty payload for an empty slot.
    virtual PokemonPayload readLocal(const SlotRef& slot) = 0;
    // Writes the save. The party is packed on every write, and a save with list boxes
    // (Gen 1/2) also packs its boxes.
    virtual bool persistLocal(std::string& error) = 0;
    // Reloads the save in memory from the last state known to be on disk. Returns false when
    // a failed write left the disk unknown: the write may have landed.
    virtual bool discardLocal() = 0;
    virtual void log(const std::string&) {}
};
