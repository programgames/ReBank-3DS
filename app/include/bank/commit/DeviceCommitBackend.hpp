#pragma once

#include "bank/commit/CommitBackend.hpp"
#include "network/api/CloudApi.hpp"
#include "save/adapter/SaveAdapter.hpp"

#include <map>
#include <string>
#include <vector>

class DeviceCommitBackend final : public CommitBackend {
public:
    DeviceCommitBackend(CloudApi& api, SaveAdapter& save, std::string accessToken)
        : api_(api), save_(save), accessToken_(std::move(accessToken)) {}

    RemoteResult moveCloud(const SlotRef& from, const SlotRef& to) override;
    UploadBatchResult uploadBatch(const std::vector<UploadItem>& items) override;
    std::optional<CloudListing> readCloudBox(std::uint16_t boxKey) override;
    RemoteResult deleteCloud(const std::vector<SlotRef>& slots) override;
    ClaimBatchResult claimCloud(const std::vector<ClaimItem>& items) override;
    bool writeLocal(const SlotRef& slot, const PokemonPayload& payload, bool fromCloud) override;
    bool clearLocal(const SlotRef& slot) override;
    PokemonPayload readLocal(const SlotRef& slot) override;
    bool persistLocal(std::string& error) override;
    bool discardLocal() override;
    void log(const std::string& message) override;

private:
    PokemonPayload readSave(const SlotRef& slot) const;

    CloudApi& api_;
    SaveAdapter& save_;
    std::string accessToken_;
    // The save can store a written Pokemon with other bytes (a conversion, the party format):
    // the bytes it stores, mapped back to the payload the commit wrote.
    std::map<std::vector<std::uint8_t>, PokemonPayload> writtenAs_;
};
