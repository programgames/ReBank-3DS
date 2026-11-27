#pragma once

#include "bank/commit/CommitBackend.hpp"
#include "bank/commit/CommitPlan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

enum class CommitPhase : int {
    Preparing,
    MovingCloud,
    SavingLocal,
    Uploading,
    Finalizing,
    RemovingCloud
};

// Runs a CommitPlan: cloud moves, claims, a first save that writes only the downloads an
// upload overwrites in the cloud, the uploads, a final save with every other local change,
// then the cloud deletes. Every Pokemon ends in exactly one place, or the result names it.
class CommitExecutor {
public:
    static constexpr std::size_t BatchSize = 30;

    using ProgressFn = std::function<void(CommitPhase, int)>;

    CommitExecutor(const CommitPlan& plan, CommitBackend& backend, ProgressFn progress);

    CommitResult run();

private:
    enum class UploadState : std::uint8_t { NotAttempted, Stored, Refused, Unknown, Undone };
    enum class CloudCopy : std::uint8_t { Intact, Overwritten, Unknown };
    enum class CloudRemoval : std::uint8_t { Gone, Present, Unknown };

    struct LayoutOutcome {
        bool ok = false;
        std::optional<std::size_t> failedTransfer;
        bool failedWrite = false;
    };

    using CloudTargets = std::vector<std::pair<std::size_t, SlotRef>>;

    bool runCloudMoves();
    bool moveLanded(const CloudMoveStep& step);
    void claimCloudSources();
    void claimBatch(const std::vector<std::size_t>& batch);
    LayoutOutcome applyLayout(const LocalLayout& target);
    bool saveHolds(const LocalLayout& target);
    bool stillInSave(std::size_t upload);
    bool runFirstSave();
    void dropGroup(std::size_t transfer, IssueReason reason);
    void runUploads();
    void uploadBatch(std::vector<std::size_t> batch);
    void reconcileUnknownUploads();
    LocalLayout settledLayout();
    LocalLayout finalTarget();
    bool partyHasNonEgg(const LocalLayout& target) const;
    void runFinalSave();
    void undoUploads(const std::vector<std::size_t>& uploads);
    std::vector<CloudRemoval> removalsAfter(const RemoteResult& removed, const CloudTargets& targets);
    void reportLocalPlacements();
    SlotRef savedSlot(std::size_t transfer, const SlotRef& placed);
    void reportCloudPositions();
    void runCloudDeletes();
    void deleteFromCloud(const CloudTargets& targets);
    void countLocalChanges();

    template <typename Predicate>
    std::vector<std::size_t> transfersWhere(Predicate predicate) const;
    bool active(std::size_t transfer) const;
    bool movedInCloud(std::size_t transfer) const;
    const PokemonPayload& payloadOf(std::size_t transfer) const;
    bool isList(const SlotRef& slot) const;
    std::vector<SlotRef> listSlots(const SlotRef& slot) const;
    CloudCopy cloudCopyOf(std::size_t transfer) const;
    std::optional<SlotRef> localSlotOf(std::size_t transfer) const;
    void addIssue(std::size_t transfer, const SlotRef& location, IssueReason reason, std::string detail = {});
    void advance(CommitPhase phase);

    const CommitPlan& plan_;
    CommitBackend& backend_;
    ProgressFn progress_;
    CommitResult result_;
    LocalLayout current_;
    std::vector<UploadState> uploads_;
    std::vector<SlotRef> cloudAt_;                          // where each cloud Pokemon is now
    std::vector<std::optional<std::size_t>> overwrittenBy_; // the upload that replaces a download in the cloud
    std::vector<bool> cancelledDownloads_;                  // final save: the download is cancelled
    std::map<std::size_t, PokemonPayload> received_;
    std::set<std::size_t> droppedGroups_;
    std::set<std::size_t> reported_;                       // transfers with an issue
    bool saveFailed_ = false;
    bool saveUnknown_ = false;                              // a failed write left the disk unknown
    std::string saveError_;
    std::size_t totalSteps_ = 1;
    std::size_t completedSteps_ = 0;
};
