#pragma once

#include "bank/BankContext.hpp"
#include "bank/BankSession.hpp"
#include "bank/LoadService.hpp"
#include "bank/StorageController.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

class HandController {
public:
    HandController(BankContext& context, BankSession& session, StorageController& storage)
        : context_(context), session_(session), storage_(storage) {}

    void pickUp();
    void drop();
    void pickUpOrDrop() { session_.hand.active ? drop() : pickUp(); }
    void completeCloudSwap(std::size_t slot, PokemonPayload occupantPayload);
    void returnHand();

private:
    // Refuses what the save's game cannot hold, and asks once before a drop that loses data.
    bool acceptedBySave();
    void placeHand(StorageAddress address, std::size_t slot);
    void swapHand(StorageAddress address, std::size_t slot, SlotContents occupant);
    void startCloudFetch(LoadService::Operation operation, std::size_t slot, const PokemonSummary& summary);

    BankContext& context_;
    BankSession& session_;
    StorageController& storage_;
    // The hand generation whose data loss the user saw: the next drop into the save goes ahead.
    std::optional<std::uint32_t> lossConfirmedFor_;
};
