#include "bank/commit/CommitExecutor.hpp"

#include "security/PayloadHash.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace {
const char* kindName(TransferKind kind) {
    switch (kind) {
        case TransferKind::LocalMove:
            return "local-move";
        case TransferKind::Upload:
            return "upload";
        case TransferKind::CloudMove:
            return "cloud-move";
        case TransferKind::Download:
            return "download";
        case TransferKind::LocalDelete:
            return "local-delete";
        case TransferKind::CloudDelete:
            return "cloud-delete";
    }
    return "unknown";
}

std::size_t countKind(const CommitPlan& plan, TransferKind kind) {
    return static_cast<std::size_t>(std::count_if(plan.transfers.begin(), plan.transfers.end(),
        [kind](const PlannedTransfer& transfer) { return transfer.kind == kind; }));
}

std::size_t batchesFor(std::size_t count) {
    return (count + CommitExecutor::BatchSize - 1) / CommitExecutor::BatchSize;
}

template <typename Item, typename Action>
void forEachBatch(const std::vector<Item>& items, Action action) {
    for (std::size_t start = 0; start < items.size(); start += CommitExecutor::BatchSize) {
        const std::size_t end = std::min(items.size(), start + CommitExecutor::BatchSize);
        action(std::vector<Item>(items.begin() + static_cast<std::ptrdiff_t>(start),
                                 items.begin() + static_cast<std::ptrdiff_t>(end)));
    }
}

bool cloudCheckReason(IssueReason reason) {
    return reason == IssueReason::CloudClaimFailed || reason == IssueReason::CloudSlotTrading
        || reason == IssueReason::CloudSlotDifferent;
}
}

template <typename Predicate>
std::vector<std::size_t> CommitExecutor::transfersWhere(Predicate predicate) const {
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < plan_.transfers.size(); ++index) {
        if (predicate(index, plan_.transfers[index])) {
            indices.push_back(index);
        }
    }
    return indices;
}

CommitExecutor::CommitExecutor(const CommitPlan& plan, CommitBackend& backend, ProgressFn progress)
    : plan_(plan),
      backend_(backend),
      progress_(std::move(progress)),
      current_(plan.baselineLayout),
      uploads_(plan.transfers.size(), UploadState::NotAttempted),
      cloudAt_(plan.transfers.size()),
      overwrittenBy_(plan.transfers.size()),
      cancelledDownloads_(plan.transfers.size(), false) {
    for (std::size_t index = 0; index < plan.transfers.size(); ++index) {
        const PlannedTransfer& transfer = plan.transfers[index];
        cloudAt_[index] = transfer.origin;
        if (transfer.kind == TransferKind::Upload && transfer.displaced) {
            overwrittenBy_[*transfer.displaced] = index;
        }
    }
}

CommitResult CommitExecutor::run() {
    for (const HeldBackPokemon& held : plan_.heldBack) {
        result_.issues.push_back({describePokemon(held.summary), held.location, held.reason, {}});
        backend_.log("commit: held back " + describeSlot(held.location) + " species "
                     + std::to_string(held.summary.species) + ": " + describeReason(held.reason));
    }
    for (const PlannedTransfer& transfer : plan_.transfers) {
        backend_.log(std::string("commit plan: ") + kindName(transfer.kind) + " " + describeSlot(transfer.origin)
                     + " -> " + describeSlot(transfer.destination)
                     + (transfer.staging ? " parked at " + describeSlot(*transfer.staging) : std::string{})
                     + " species " + std::to_string(transfer.summary.species)
                     + " payload=" + payloadTag(transfer.payload.data));
    }
    totalSteps_ = std::max<std::size_t>(1, plan_.cloudMoves.size() + 2
        + batchesFor(countKind(plan_, TransferKind::Upload))
        + batchesFor(countKind(plan_, TransferKind::Download) + countKind(plan_, TransferKind::CloudDelete)));

    if (plan_.empty()) {
        result_.success = result_.issues.empty();
        return result_;
    }
    if (!runCloudMoves()) {
        reportCloudPositions();
        return result_;
    }
    claimCloudSources();
    if (!runFirstSave()) {
        reportCloudPositions();
        return result_;
    }
    runUploads();
    runFinalSave();
    reportLocalPlacements();
    runCloudDeletes();
    countLocalChanges();
    progress_(CommitPhase::RemovingCloud, 100);
    if (saveFailed_) {
        result_.stop = CommitStop::SaveFailed;
        result_.stopDetail = saveError_;
    } else {
        result_.success = true;
    }
    return result_;
}

bool CommitExecutor::active(std::size_t transfer) const {
    return droppedGroups_.count(plan_.transfers[transfer].group) == 0;
}

// A cloud Pokemon that the cloud moves left away from both its origin and its destination.
bool CommitExecutor::movedInCloud(std::size_t transfer) const {
    const PlannedTransfer& planned = plan_.transfers[transfer];
    return planned.origin.isCloud() && cloudAt_[transfer] != planned.origin
        && !(planned.kind == TransferKind::CloudMove && cloudAt_[transfer] == planned.destination);
}

const PokemonPayload& CommitExecutor::payloadOf(std::size_t transfer) const {
    const auto received = received_.find(transfer);
    return received != received_.end() ? received->second : plan_.transfers[transfer].payload;
}

// The party is a list, and so is a box of a Gen 1/2 save: a write packs it.
bool CommitExecutor::isList(const SlotRef& slot) const {
    return slot.kind == SlotKind::Party || (slot.kind == SlotKind::Local && plan_.listBoxes);
}

std::vector<SlotRef> CommitExecutor::listSlots(const SlotRef& slot) const {
    std::vector<SlotRef> slots;
    const std::size_t size = slot.kind == SlotKind::Party ? PartySlotCount : BoxSlotCount;
    for (std::size_t index = 0; index < size; ++index) {
        slots.push_back({slot.kind, slot.box, static_cast<std::uint8_t>(index)});
    }
    return slots;
}

bool CommitExecutor::runCloudMoves() {
    for (const CloudMoveStep& step : plan_.cloudMoves) {
        advance(CommitPhase::MovingCloud);
        RemoteResult moved = backend_.moveCloud(step.from, step.to);
        if (moved.outcome == RemoteOutcome::Unknown && moveLanded(step)) {
            backend_.log("commit: cloud move " + describeSlot(step.from) + " -> " + describeSlot(step.to)
                         + " confirmed by listing");
            moved.outcome = RemoteOutcome::Done;
        }
        if (moved.outcome == RemoteOutcome::Done) {
            ++result_.moves;
            cloudAt_[step.mover] = step.to;
            if (step.displaced) {
                cloudAt_[*step.displaced] = step.from;
            }
            continue;
        }
        backend_.log("commit: cloud move " + describeSlot(step.from) + " -> " + describeSlot(step.to)
                     + " failed: " + moved.message);
        addIssue(step.mover, step.from, IssueReason::CloudMoveFailed, moved.message);
        if (step.displaced) {
            addIssue(*step.displaced, step.to, IssueReason::CloudMoveFailed, moved.message);
        }
        result_.stop = CommitStop::CloudMoveFailed;
        result_.stopDetail = moved.message;
        return false;
    }
    return true;
}

bool CommitExecutor::moveLanded(const CloudMoveStep& step) {
    const auto listing = backend_.readCloudBox(step.to.box);
    return listing && (*listing)[step.to.slot].payload.data == payloadOf(step.mover).data;
}

CommitExecutor::LayoutOutcome CommitExecutor::applyLayout(const LocalLayout& target) {
    bool changed = false;
    for (const auto& [slot, occupant] : target) {
        const auto existing = current_.find(slot);
        const std::optional<std::size_t> before = existing == current_.end() ? std::nullopt : existing->second;
        if (before == occupant) {
            continue;
        }
        changed = true;
        const bool ok = occupant
            ? backend_.writeLocal(slot, payloadOf(*occupant), plan_.transfers[*occupant].origin.isCloud())
            : backend_.clearLocal(slot);
        if (!ok) {
            backend_.discardLocal();
            return {false, occupant ? occupant : before, occupant.has_value()};
        }
    }
    if (!changed) {
        return {true, std::nullopt, false};
    }
    if (!backend_.persistLocal(saveError_)) {
        backend_.log("commit: save write failed: " + saveError_);
        if (!backend_.discardLocal()) {
            backend_.log("commit: the disk state is unknown after the failed write");
            saveUnknown_ = true;
            return {false, std::nullopt, false};
        }
        // A change inside a list can look the same before and after: then it did not land.
        if (!saveHolds(target) || saveHolds(current_)) {
            return {false, std::nullopt, false};
        }
        backend_.log("commit: the save reached the disk anyway");
    }
    for (const auto& [slot, occupant] : target) {
        current_[slot] = occupant;
    }
    return {true, std::nullopt, false};
}

// Whether the save (as reloaded after a failed write) holds `target`. A list is compared as
// the multiset of the commit's Pokemon in it, because the write packs it.
bool CommitExecutor::saveHolds(const LocalLayout& target) {
    using Data = std::vector<std::uint8_t>;
    std::set<Data> tracked;
    for (std::size_t index = 0; index < plan_.transfers.size(); ++index) {
        tracked.insert(payloadOf(index).data);
    }
    std::map<SlotRef, std::multiset<Data>> lists;
    for (const auto& [slot, occupant] : target) {
        const Data expected = occupant ? payloadOf(*occupant).data : Data{};
        if (isList(slot)) {
            std::multiset<Data>& members = lists[listSlots(slot).front()];
            if (occupant) {
                members.insert(expected);
            }
        } else if (backend_.readLocal(slot).data != expected) {
            return false;
        }
    }
    for (const auto& [head, expected] : lists) {
        std::multiset<Data> found;
        for (const SlotRef& slot : listSlots(head)) {
            Data data = backend_.readLocal(slot).data;
            if (tracked.count(data)) {
                found.insert(std::move(data));
            }
        }
        if (found != expected) {
            return false;
        }
    }
    return true;
}

bool CommitExecutor::stillInSave(std::size_t upload) {
    const SlotRef& origin = plan_.transfers[upload].origin;
    const std::vector<SlotRef> slots = isList(origin) ? listSlots(origin) : std::vector<SlotRef>{origin};
    return std::any_of(slots.begin(), slots.end(), [&](const SlotRef& slot) {
        return backend_.readLocal(slot).data == payloadOf(upload).data;
    });
}

void CommitExecutor::claimCloudSources() {
    const std::vector<std::size_t> sources = transfersWhere([&](std::size_t index, const PlannedTransfer& transfer) {
        const bool leavesCloud = transfer.kind == TransferKind::Download || transfer.kind == TransferKind::CloudDelete;
        return leavesCloud && active(index) && transfer.cloudSlotAfterMoves;
    });
    forEachBatch(sources, [&](const std::vector<std::size_t>& batch) { claimBatch(batch); });
}

void CommitExecutor::claimBatch(const std::vector<std::size_t>& batch) {
    std::vector<ClaimItem> items;
    for (const std::size_t index : batch) {
        items.push_back({*plan_.transfers[index].cloudSlotAfterMoves, plan_.transfers[index].payload});
    }
    const ClaimBatchResult claimed = backend_.claimCloud(items);
    for (std::size_t offset = 0; offset < batch.size(); ++offset) {
        const std::size_t index = batch[offset];
        if (!active(index)) {
            continue;
        }
        if (claimed.outcome != RemoteOutcome::Done || offset >= claimed.claims.size()) {
            dropGroup(index, IssueReason::CloudClaimFailed);
            continue;
        }
        if (offset < claimed.received.size() && claimed.received[offset] && claimed.claims[offset] == CloudClaim::Claimed) {
            received_[index] = *claimed.received[offset];
            backend_.log("commit: event Pokemon created for this trainer at "
                         + describeSlot(*plan_.transfers[index].cloudSlotAfterMoves));
        }
        if (claimed.claims[offset] == CloudClaim::Locked) {
            dropGroup(index, IssueReason::CloudSlotTrading);
        } else if (claimed.claims[offset] == CloudClaim::Changed) {
            dropGroup(index, IssueReason::CloudSlotDifferent);
        }
    }
    if (claimed.outcome != RemoteOutcome::Done) {
        backend_.log("commit: cloud claim failed: " + claimed.message);
    }
}

// Cancels the rest of a group and reports each member where it really is. Uploads that
// were attempted and downloads already in the save are reported later.
void CommitExecutor::dropGroup(std::size_t trigger, IssueReason reason) {
    const std::size_t group = plan_.transfers[trigger].group;
    droppedGroups_.insert(group);
    for (std::size_t index = 0; index < plan_.transfers.size(); ++index) {
        const PlannedTransfer& transfer = plan_.transfers[index];
        if (transfer.group != group) {
            continue;
        }
        if ((transfer.kind == TransferKind::Upload && uploads_[index] != UploadState::NotAttempted)
            || (transfer.kind == TransferKind::Download && localSlotOf(index))) {
            continue;
        }
        const bool moved = movedInCloud(index);
        if (transfer.kind == TransferKind::CloudDelete && cloudCopyOf(index) != CloudCopy::Intact) {
            // An upload overwrote it: it is gone as the user asked, or maybe gone.
            if (cloudCopyOf(index) == CloudCopy::Unknown) {
                addIssue(index, cloudAt_[index], IssueReason::RemovalUnconfirmed);
            }
        } else if (index == trigger && (cloudCheckReason(reason) || !moved)) {
            addIssue(index, cloudAt_[index], reason);
        } else if (moved) {
            addIssue(index, cloudAt_[index], IssueReason::MovedInCloud);
        } else if (!(transfer.kind == TransferKind::CloudMove && cloudAt_[index] == transfer.destination)) {
            addIssue(index, transfer.origin, IssueReason::LinkedToHeldBack);
        }
    }
}

// The first save writes only the downloads that an upload overwrites in the cloud, at their
// destination or parking slot. It empties no slot, so a packed list shifts nothing.
bool CommitExecutor::runFirstSave() {
    advance(CommitPhase::SavingLocal);
    // The parking slots of a list box follow its last Pokemon. When a group is dropped, the
    // downloads left take the first of them, so that no gap makes the write shift them.
    std::map<std::uint16_t, std::vector<SlotRef>> parking;
    for (const PlannedTransfer& transfer : plan_.transfers) {
        if (transfer.staging && plan_.listBoxes) {
            parking[transfer.staging->box].push_back(*transfer.staging);
        }
    }
    for (auto& [box, slots] : parking) {
        std::sort(slots.begin(), slots.end());
    }
    while (true) {
        LocalLayout target = current_;
        std::map<std::uint16_t, std::size_t> taken;
        for (std::size_t index = 0; index < plan_.transfers.size(); ++index) {
            const PlannedTransfer& transfer = plan_.transfers[index];
            if (transfer.kind != TransferKind::Download || !overwrittenBy_[index] || !active(index)) {
                continue;
            }
            if (!transfer.staging) {
                target[transfer.destination] = index;
            } else if (plan_.listBoxes) {
                target[parking[transfer.staging->box][taken[transfer.staging->box]++]] = index;
            } else {
                target[*transfer.staging] = index;
            }
        }
        const LayoutOutcome outcome = applyLayout(target);
        if (outcome.ok) {
            return true;
        }
        if (!outcome.failedTransfer) {
            result_.stop = CommitStop::SaveFailed;
            result_.stopDetail = saveError_;
            for (const auto& [slot, occupant] : target) {
                if (saveUnknown_ && occupant && current_[slot] != occupant) {
                    addIssue(*occupant, cloudAt_[*occupant], IssueReason::SaveUnconfirmed);
                }
            }
            return false;
        }
        dropGroup(*outcome.failedTransfer, outcome.failedWrite ? IssueReason::WriteFailed : IssueReason::RemoveFailed);
    }
}

void CommitExecutor::runUploads() {
    const std::vector<std::size_t> pending = transfersWhere([&](std::size_t index, const PlannedTransfer& transfer) {
        return transfer.kind == TransferKind::Upload && active(index);
    });
    forEachBatch(pending, [&](std::vector<std::size_t> batch) {
        advance(CommitPhase::Uploading);
        uploadBatch(std::move(batch));
    });
    reconcileUnknownUploads();
}

void CommitExecutor::uploadBatch(std::vector<std::size_t> batch) {
    bool retry = false;
    while (!batch.empty()) {
        std::vector<UploadItem> items;
        items.reserve(batch.size());
        for (const std::size_t index : batch) {
            const PlannedTransfer& transfer = plan_.transfers[index];
            items.push_back({transfer.destination, transfer.summary, transfer.payload, transfer.displaced.has_value()});
        }
        const UploadBatchResult sent = backend_.uploadBatch(items);
        if (sent.outcome == RemoteOutcome::Done && sent.stored == batch.size()) {
            for (const std::size_t index : batch) {
                uploads_[index] = UploadState::Stored;
                ++result_.uploads;
            }
            return;
        }
        if (sent.outcome != RemoteOutcome::Refused) {
            backend_.log("commit: upload outcome unknown (" + sent.message + ", stored "
                         + std::to_string(sent.stored) + " of " + std::to_string(batch.size()) + "), checking the cloud");
            for (const std::size_t index : batch) {
                uploads_[index] = UploadState::Unknown;
            }
            return;
        }
        // A retry can be refused because the server stored part of an earlier attempt:
        // the cloud listing decides.
        const UploadState rejectedState = retry ? UploadState::Unknown : UploadState::Refused;
        std::vector<std::size_t> remaining;
        for (const std::size_t index : batch) {
            const PlannedTransfer& transfer = plan_.transfers[index];
            const auto rejection = std::find_if(sent.rejected.begin(), sent.rejected.end(),
                [&](const CloudRejection& candidate) { return candidate.slot == transfer.destination; });
            if (rejection == sent.rejected.end()) {
                remaining.push_back(index);
                continue;
            }
            uploads_[index] = rejectedState;
            if (!retry) {
                addIssue(index, transfer.origin, IssueReason::CloudRefused, rejection->reason);
            }
        }
        if (remaining.size() == batch.size()) {
            for (const std::size_t index : batch) {
                uploads_[index] = rejectedState;
                if (!retry) {
                    addIssue(index, plan_.transfers[index].origin, IssueReason::CloudRefused, sent.message);
                }
            }
            return;
        }
        batch = std::move(remaining);
        retry = true;
    }
}

void CommitExecutor::reconcileUnknownUploads() {
    std::map<std::uint16_t, std::vector<std::size_t>> byBox;
    for (std::size_t index = 0; index < plan_.transfers.size(); ++index) {
        if (uploads_[index] == UploadState::Unknown) {
            byBox[plan_.transfers[index].destination.box].push_back(index);
        }
    }
    for (const auto& [box, transfers] : byBox) {
        const auto listing = backend_.readCloudBox(box);
        for (const std::size_t index : transfers) {
            const PlannedTransfer& transfer = plan_.transfers[index];
            if (!listing) {
                addIssue(index, transfer.origin, IssueReason::UploadUnconfirmed, describeSlot(transfer.destination));
                continue;
            }
            const ListedSlot& found = (*listing)[transfer.destination.slot];
            const bool displacedStillThere = transfer.displaced && found.occupied
                && found.payload.data == plan_.transfers[*transfer.displaced].payload.data;
            if (found.occupied && !found.payload.known()) {
                addIssue(index, transfer.origin, IssueReason::UploadUnconfirmed, describeSlot(transfer.destination));
            } else if (found.occupied && found.payload.data == transfer.payload.data) {
                uploads_[index] = UploadState::Stored;
                ++result_.uploads;
                backend_.log("commit: upload to " + describeSlot(transfer.destination) + " confirmed by listing");
            } else if (!found.occupied && transfer.displaced) {
                // Neither Pokemon is in the slot: the one it replaced may be gone from the cloud,
                // so the save keeps both (the upload state stays Unknown).
                addIssue(index, transfer.origin, IssueReason::UploadNotStored);
            } else if (!found.occupied || displacedStillThere) {
                uploads_[index] = UploadState::Refused;
                addIssue(index, transfer.origin, IssueReason::UploadNotStored);
            } else {
                addIssue(index, transfer.origin, IssueReason::CloudSlotDifferent);
            }
        }
    }
}

CommitExecutor::CloudCopy CommitExecutor::cloudCopyOf(std::size_t transfer) const {
    if (!overwrittenBy_[transfer]) {
        return CloudCopy::Intact;
    }
    switch (uploads_[*overwrittenBy_[transfer]]) {
        case UploadState::Stored:
        case UploadState::Undone:
            return CloudCopy::Overwritten;
        case UploadState::Unknown:
            return CloudCopy::Unknown;
        default:
            return CloudCopy::Intact;
    }
}

std::optional<SlotRef> CommitExecutor::localSlotOf(std::size_t transfer) const {
    for (const auto& [slot, occupant] : current_) {
        if (occupant == transfer) {
            return slot;
        }
    }
    return std::nullopt;
}

// The local layout after the commit, given what the cloud accepted. An upload the cloud did
// not store keeps its slot, so the Pokemon meant for that slot keeps its own slot too, or is
// cancelled when it is a download whose cloud copy is intact. This repeats down the chain.
LocalLayout CommitExecutor::settledLayout() {
    const std::size_t count = plan_.transfers.size();
    std::vector<bool> stays(count, false);
    cancelledDownloads_.assign(count, false);
    for (std::size_t index = 0; index < count; ++index) {
        const PlannedTransfer& transfer = plan_.transfers[index];
        if (!active(index)) {
            const bool cancel = transfer.kind == TransferKind::Download && localSlotOf(index)
                && cloudCopyOf(index) == CloudCopy::Intact;
            (cancel ? cancelledDownloads_ : stays)[index] = true;
        } else if (transfer.kind == TransferKind::Upload && uploads_[index] != UploadState::Stored) {
            stays[index] = true;
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t index = 0; index < count; ++index) {
            const PlannedTransfer& transfer = plan_.transfers[index];
            if (!active(index) || !transfer.landsLocally() || stays[index] || cancelledDownloads_[index]) {
                continue;
            }
            const auto occupant = current_.find(transfer.destination);
            if (occupant == current_.end() || !occupant->second || *occupant->second == index
                || !stays[*occupant->second]) {
                continue;
            }
            const bool cancel = transfer.kind == TransferKind::Download && cloudCopyOf(index) == CloudCopy::Intact;
            (cancel ? cancelledDownloads_ : stays)[index] = true;
            changed = true;
        }
    }
    LocalLayout target = current_;
    for (std::size_t index = 0; index < count; ++index) {
        if (!stays[index]) {
            if (const std::optional<SlotRef> slot = localSlotOf(index)) {
                target[*slot] = std::nullopt;
            }
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        const PlannedTransfer& transfer = plan_.transfers[index];
        if (active(index) && transfer.landsLocally() && !stays[index] && !cancelledDownloads_[index]) {
            target[transfer.destination] = index;
        }
    }
    return target;
}

bool CommitExecutor::partyHasNonEgg(const LocalLayout& target) const {
    for (std::size_t slot = 0; slot < PartySlotCount; ++slot) {
        const auto tracked = target.find(SlotRef::party(slot));
        const bool hatched = tracked != target.end()
            ? tracked->second && !plan_.transfers[*tracked->second].summary.egg
            : plan_.partyBaseline.summaries[slot].occupied() && !plan_.partyBaseline.summaries[slot].egg;
        if (hatched) {
            return true;
        }
    }
    return false;
}

// The final layout. When dropped groups would leave the party with no Pokemon that is not an
// egg, the groups that take a Pokemon out of the party are dropped too.
LocalLayout CommitExecutor::finalTarget() {
    while (true) {
        LocalLayout target = settledLayout();
        if (partyHasNonEgg(target)) {
            return target;
        }
        const std::vector<std::size_t> leaving = transfersWhere([&](std::size_t index, const PlannedTransfer& transfer) {
            return active(index) && transfer.origin.kind == SlotKind::Party
                && (transfer.kind != TransferKind::LocalMove || transfer.destination.kind != SlotKind::Party);
        });
        if (leaving.empty()) {
            return target;
        }
        for (const std::size_t index : leaving) {
            if (active(index)) {
                dropGroup(index, IssueReason::PartyKept);
            }
        }
    }
}

void CommitExecutor::runFinalSave() {
    advance(CommitPhase::Finalizing);
    while (true) {
        const LayoutOutcome outcome = applyLayout(finalTarget());
        if (outcome.ok) {
            break;
        }
        if (!outcome.failedTransfer) {
            backend_.log("commit: final save failed (" + saveError_ + ")");
            saveFailed_ = true;
            // A known disk is as before; an unknown one may have dropped a cancelled download
            // from the save, so its cloud copy must stay.
            if (!saveUnknown_) {
                std::fill(cancelledDownloads_.begin(), cancelledDownloads_.end(), false);
            }
            break;
        }
        dropGroup(*outcome.failedTransfer, outcome.failedWrite ? IssueReason::WriteFailed : IssueReason::RemoveFailed);
    }
    // Undo the uploads whose Pokemon is still in the save: dropped ones (the final layout kept
    // them), and all of them when the save failed, unless the failed write still cleared their
    // slot. When the disk is unknown, an upload may be the only copy left: keep it.
    for (std::size_t index = 0; index < plan_.transfers.size(); ++index) {
        if (saveUnknown_ && active(index) && uploads_[index] == UploadState::Stored) {
            addIssue(index, plan_.transfers[index].origin, IssueReason::SaveUnconfirmed);
        }
    }
    undoUploads(transfersWhere([&](std::size_t index, const PlannedTransfer& transfer) {
        return transfer.kind == TransferKind::Upload && uploads_[index] == UploadState::Stored
            && (!active(index) || (saveFailed_ && !saveUnknown_ && stillInSave(index)));
    }));
}

void CommitExecutor::undoUploads(const std::vector<std::size_t>& uploads) {
    forEachBatch(uploads, [&](const std::vector<std::size_t>& batch) {
        CloudTargets targets;
        std::vector<SlotRef> slots;
        for (const std::size_t index : batch) {
            targets.emplace_back(index, plan_.transfers[index].destination);
            slots.push_back(plan_.transfers[index].destination);
        }
        const std::vector<CloudRemoval> removals = removalsAfter(backend_.deleteCloud(slots), targets);
        for (std::size_t offset = 0; offset < batch.size(); ++offset) {
            const std::size_t index = batch[offset];
            const PlannedTransfer& transfer = plan_.transfers[index];
            switch (removals[offset]) {
                case CloudRemoval::Gone:
                    uploads_[index] = UploadState::Undone;
                    --result_.uploads;
                    addIssue(index, transfer.origin, IssueReason::UploadUndone);
                    break;
                case CloudRemoval::Present:
                    addIssue(index, transfer.origin, IssueReason::UploadNotUndone, describeSlot(transfer.destination));
                    break;
                case CloudRemoval::Unknown:
                    addIssue(index, transfer.origin, IssueReason::RemovalUnconfirmed, describeSlot(transfer.destination));
                    break;
            }
        }
    });
}

// What a cloud delete did to each target. A delete that is not confirmed is checked against
// the box listing.
std::vector<CommitExecutor::CloudRemoval> CommitExecutor::removalsAfter(const RemoteResult& removed,
                                                                       const CloudTargets& targets) {
    if (removed.outcome == RemoteOutcome::Done) {
        return std::vector<CloudRemoval>(targets.size(), CloudRemoval::Gone);
    }
    backend_.log("commit: cloud delete of " + std::to_string(targets.size()) + " slot(s) not confirmed ("
                 + removed.message + "), checking the cloud");
    std::map<std::uint16_t, std::optional<CloudListing>> listings;
    std::vector<CloudRemoval> removals;
    for (const auto& [index, slot] : targets) {
        if (!listings.count(slot.box)) {
            listings[slot.box] = backend_.readCloudBox(slot.box);
        }
        const std::optional<CloudListing>& listing = listings[slot.box];
        const ListedSlot* found = listing ? &(*listing)[slot.slot] : nullptr;
        if (!found || (found->occupied && !found->payload.known())) {
            removals.push_back(CloudRemoval::Unknown);
        } else {
            const bool there = found->occupied && (found->payload.data == plan_.transfers[index].payload.data
                                                   || found->payload.data == payloadOf(index).data);
            removals.push_back(there ? CloudRemoval::Present : CloudRemoval::Gone);
        }
    }
    return removals;
}

void CommitExecutor::reportLocalPlacements() {
    for (std::size_t index = 0; index < plan_.transfers.size(); ++index) {
        const PlannedTransfer& transfer = plan_.transfers[index];
        const bool download = transfer.kind == TransferKind::Download;
        const std::optional<SlotRef> placed = localSlotOf(index);
        const bool reported = active(index) || (download && (placed || cancelledDownloads_[index]));
        if (!reported || (!transfer.landsLocally() && transfer.kind != TransferKind::LocalDelete)) {
            continue;
        }
        const bool moved = movedInCloud(index);
        // After a failed write the save is as before, or unknown: then each change is unconfirmed.
        const auto notSaved = [&](IssueReason reason) { return saveUnknown_ ? IssueReason::SaveUnconfirmed : reason; };
        if (cancelledDownloads_[index]) {
            addIssue(index, cloudAt_[index], saveUnknown_ ? IssueReason::SaveUnconfirmed
                                             : moved   ? IssueReason::MovedInCloud
                                                       : IssueReason::SwapCancelled);
        } else if (transfer.kind == TransferKind::LocalDelete) {
            if (placed) {
                addIssue(index, *placed, notSaved(IssueReason::RemoveFailed));
            }
        } else if (placed == transfer.destination) {
            continue;
        } else if (placed && download) { // a download that is not at its destination is parked
            addIssue(index, savedSlot(index, *placed), IssueReason::PlacedElsewhere);
        } else if (placed) {
            addIssue(index, savedSlot(index, *placed),
                     saveFailed_ ? notSaved(IssueReason::WriteFailed) : IssueReason::ReturnedToOrigin);
        } else if (saveUnknown_) {
            addIssue(index, cloudAt_[index], IssueReason::SaveUnconfirmed);
        } else {
            addIssue(index, cloudAt_[index], moved ? IssueReason::MovedInCloud : IssueReason::WriteFailed);
        }
    }
}

// The slot a Pokemon has in the save: a write packs a list, so look it up there.
SlotRef CommitExecutor::savedSlot(std::size_t transfer, const SlotRef& placed) {
    if (!isList(placed)) {
        return placed;
    }
    for (const SlotRef& slot : listSlots(placed)) {
        if (backend_.readLocal(slot).data == payloadOf(transfer).data) {
            return slot;
        }
    }
    return placed;
}

// After a stop: the cloud moves already made stay, so each moved Pokemon is reported where it is.
void CommitExecutor::reportCloudPositions() {
    for (std::size_t index = 0; index < plan_.transfers.size(); ++index) {
        if (active(index) && movedInCloud(index) && !reported_.count(index)) {
            addIssue(index, cloudAt_[index], IssueReason::MovedInCloud);
        }
    }
}

void CommitExecutor::runCloudDeletes() {
    CloudTargets targets;
    for (std::size_t index = 0; index < plan_.transfers.size(); ++index) {
        const PlannedTransfer& transfer = plan_.transfers[index];
        const bool download = transfer.kind == TransferKind::Download;
        if (download ? (cancelledDownloads_[index] || !localSlotOf(index))
                     : (transfer.kind != TransferKind::CloudDelete || !active(index))) {
            continue;
        }
        switch (cloudCopyOf(index)) {
            case CloudCopy::Overwritten:
                ++(download ? result_.downloads : result_.deletes);
                break;
            case CloudCopy::Unknown:
                if (download) {
                    ++result_.downloads;
                }
                addIssue(index, cloudAt_[index],
                         download ? IssueReason::OverwriteUnconfirmed : IssueReason::RemovalUnconfirmed);
                break;
            case CloudCopy::Intact:
                targets.emplace_back(index, cloudAt_[index]);
                break;
        }
    }
    forEachBatch(targets, [&](const CloudTargets& batch) { deleteFromCloud(batch); });
}

void CommitExecutor::deleteFromCloud(const CloudTargets& targets) {
    advance(CommitPhase::RemovingCloud);
    std::vector<SlotRef> slots;
    slots.reserve(targets.size());
    for (const auto& target : targets) {
        slots.push_back(target.second);
    }
    const RemoteResult removed = backend_.deleteCloud(slots);
    const std::vector<CloudRemoval> removals = removalsAfter(removed, targets);
    for (std::size_t offset = 0; offset < targets.size(); ++offset) {
        const auto& [index, slot] = targets[offset];
        const bool download = plan_.transfers[index].kind == TransferKind::Download;
        switch (removals[offset]) {
            case CloudRemoval::Gone:
                ++(download ? result_.downloads : result_.deletes);
                break;
            case CloudRemoval::Present:
                addIssue(index, slot, download ? IssueReason::DownloadNotRemoved : IssueReason::CloudRemoveFailed,
                         removed.message);
                break;
            case CloudRemoval::Unknown:
                addIssue(index, slot, IssueReason::RemovalUnconfirmed, removed.message);
                break;
        }
    }
}

void CommitExecutor::countLocalChanges() {
    for (std::size_t index = 0; index < plan_.transfers.size(); ++index) {
        const PlannedTransfer& transfer = plan_.transfers[index];
        if (!active(index)) {
            continue;
        }
        if (transfer.kind == TransferKind::LocalMove && localSlotOf(index) == transfer.destination) {
            ++result_.moves;
        } else if (transfer.kind == TransferKind::LocalDelete && !localSlotOf(index)) {
            ++result_.deletes;
        }
    }
}

void CommitExecutor::addIssue(std::size_t transfer, const SlotRef& location, IssueReason reason, std::string detail) {
    const PokemonSummary& summary = plan_.transfers[transfer].summary;
    backend_.log("commit issue: " + describeSlot(location) + " species " + std::to_string(summary.species)
                 + ": " + describeReason(reason) + (detail.empty() ? std::string{} : " (" + detail + ")"));
    result_.issues.push_back({describePokemon(summary), location, reason, std::move(detail)});
    reported_.insert(transfer);
}

void CommitExecutor::advance(CommitPhase phase) {
    completedSteps_ = std::min(completedSteps_ + 1, totalSteps_);
    progress_(phase, static_cast<int>(completedSteps_ * 100 / totalSteps_));
}
