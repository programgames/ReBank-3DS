// Host-side tests for planCommit -> CommitExecutor::run against an in-memory CommitBackend.
// Every run, successful or not, is checked against three invariants (see checkInvariants).

#include "FakeBackend.hpp"

#include "bank/commit/CommitExecutor.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- harness

using Failures = std::vector<std::string>;

struct TestCase {
    const char* name;
    void (*body)(Failures&);
    const char* expectedFailure; // nullptr: the test must pass
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Register {
    Register(const char* name, void (*body)(Failures&), const char* expectedFailure) {
        registry().push_back({name, body, expectedFailure});
    }
};

#define TEST_CASE(name, expectedFailure)                           \
    void name(Failures& failures);                                \
    const Register register_##name(#name, name, expectedFailure); \
    void name([[maybe_unused]] Failures& failures)

#define TEST(name) TEST_CASE(name, nullptr)
// A known bug: the test must fail with a message that contains `expected`. When it passes,
// the run fails so that the fix turns it into a TEST.
#define EXPECTED_FAILURE(name, expected) TEST_CASE(name, expected)

std::uint32_t runSeed = 20261127;

// ---------------------------------------------------------------- building cases

SlotRef box(std::size_t box, std::size_t slot) { return SlotRef::local(box, slot); }
SlotRef party(std::size_t slot) { return SlotRef::party(slot); }
SlotRef bank(std::size_t slot, std::uint16_t box = 0) { return SlotRef::cloud(box, slot); }

struct Case {
    State before;
    State after;
    std::vector<Mon> trashed;
    std::set<Mon> eggs;
    bool listBoxes = false;
    bool listingWithoutPayload = false;
};

constexpr Mon PartyKeeper = 99;
constexpr std::size_t LocalBoxes = 2;

bool hasParty(const State& state) {
    return std::any_of(state.begin(), state.end(), [](const auto& entry) { return entry.first.kind == SlotKind::Party; });
}

bool hasHatchedParty(const State& state, const std::set<Mon>& eggs) {
    return std::any_of(state.begin(), state.end(), [&](const auto& entry) {
        return entry.first.kind == SlotKind::Party && eggs.count(entry.second) == 0;
    });
}

// Most cases keep one Pokemon in the party, like the UI guard does.
Case scenario(State before, State after, std::vector<Mon> trashed = {}) {
    Case c;
    c.before = std::move(before);
    c.after = std::move(after);
    c.trashed = std::move(trashed);
    c.before[party(0)] = PartyKeeper;
    c.after[party(0)] = PartyKeeper;
    return c;
}

CommitSnapshot snapshotOf(const Case& c) {
    CommitSnapshot snapshot;
    for (std::size_t index = 0; index < LocalBoxes; ++index) {
        snapshot.localBaselines[index] = {};
        snapshot.localDrafts[index] = {};
    }
    const auto summary = [&c](Mon mon) {
        PokemonSummary found = summaryOf(mon);
        found.egg = c.eggs.count(mon) != 0;
        return found;
    };
    const auto fill = [&](const State& state, bool draft) {
        for (const auto& [slot, mon] : state) {
            switch (slot.kind) {
                case SlotKind::Local: {
                    BoxSlots& slots = (draft ? snapshot.localDrafts : snapshot.localBaselines)[slot.box];
                    slots.summaries[slot.slot] = summary(mon);
                    slots.payloads[slot.slot] = payloadOf(mon);
                    break;
                }
                case SlotKind::Party: {
                    PartySlots& slots = draft ? snapshot.partyWorking : snapshot.partyBaseline;
                    slots.summaries[slot.slot] = summary(mon);
                    slots.payloads[slot.slot] = payloadOf(mon);
                    break;
                }
                case SlotKind::Cloud: {
                    CloudBoxDraft& cloud = snapshot.cloudBoxes[slot.box];
                    if (draft) {
                        cloud.summaries[slot.slot] = summary(mon);
                        cloud.pending[slot.slot] = payloadOf(mon);
                    } else {
                        cloud.baseline[slot.slot] = summary(mon);
                        cloud.payloads[slot.slot] = payloadOf(mon);
                    }
                    break;
                }
            }
        }
    };
    fill(c.before, false);
    fill(c.after, true);
    for (auto& [key, cloud] : snapshot.cloudBoxes) {
        for (std::size_t slot = 0; slot < BoxSlotCount; ++slot) {
            if (cloud.pending[slot] == cloud.payloads[slot]) {
                cloud.pending[slot] = {};
            }
        }
    }
    for (const Mon mon : c.trashed) {
        snapshot.confirmedDeletions.push_back(payloadOf(mon));
    }
    return snapshot;
}

struct Outcome {
    CommitPlan plan;
    CommitResult result;
    State finalState; // the save on disk plus the cloud
    CallCounts calls{};
};

Outcome execute(const Case& c, const std::vector<Fault>& faults) {
    FakeBackend fake;
    for (const auto& [slot, mon] : c.before) {
        (slot.isCloud() ? fake.cloud : fake.disk)[slot] = mon;
    }
    fake.working = fake.disk;
    fake.known = fake.disk;
    fake.listBoxes = c.listBoxes;
    fake.listingWithoutPayload = c.listingWithoutPayload;
    fake.faults = faults;
    const LocalSlotProbe probe{LocalBoxes,
                               [&c](std::size_t boxIndex, std::size_t slot) { return c.before.count(box(boxIndex, slot)) != 0; },
                               BoxSlotCount, c.listBoxes};
    Outcome outcome;
    outcome.plan = planCommit(snapshotOf(c), probe);
    outcome.result = CommitExecutor(outcome.plan, fake, [](CommitPhase, int) {}).run();
    outcome.finalState = fake.disk;
    outcome.finalState.insert(fake.cloud.begin(), fake.cloud.end());
    outcome.calls = fake.calls;
    return outcome;
}

// ---------------------------------------------------------------- text

std::string text(const SlotRef& slot) {
    const std::string index = std::to_string(slot.slot);
    switch (slot.kind) {
        case SlotKind::Party: return "party" + index;
        case SlotKind::Cloud: return "bank" + std::to_string(slot.box) + "." + index;
        case SlotKind::Local: break;
    }
    return "box" + std::to_string(slot.box) + "." + index;
}

std::string text(const std::vector<SlotRef>& slots) {
    if (slots.empty()) {
        return "nowhere";
    }
    std::string out;
    for (const SlotRef& slot : slots) {
        out += (out.empty() ? "" : "+") + text(slot);
    }
    return out;
}

std::string text(const State& state) {
    std::string out;
    for (const auto& [slot, mon] : state) {
        out += (out.empty() ? "" : " ") + text(slot) + "=M" + std::to_string(mon);
    }
    return "{" + out + "}";
}

std::string text(const CommitResult& result) {
    std::string out = std::string(result.success ? "success" : "failure") + ", " + std::to_string(result.issues.size())
        + " issue(s)";
    for (const CommitIssue& issue : result.issues) {
        out += "; " + issue.pokemon + ": " + describeReason(issue.reason);
    }
    return out;
}

// ---------------------------------------------------------------- invariants

std::vector<SlotRef> where(const State& state, Mon mon) {
    std::vector<SlotRef> slots;
    for (const auto& [slot, occupant] : state) {
        if (occupant == mon) {
            slots.push_back(slot);
        }
    }
    return slots;
}

// Where a Pokemon is, with a list counted as one place: the party (invariant 2 checks it), and a
// Gen 1/2 box, where a Pokemon that stays shifts the others. Invariant 3 checks the order of a
// clean commit.
std::vector<SlotRef> placesOf(const State& state, Mon mon, bool listBoxes) {
    std::vector<SlotRef> slots = where(state, mon);
    for (SlotRef& slot : slots) {
        if (slot.kind == SlotKind::Party || (slot.kind == SlotKind::Local && listBoxes)) {
            slot.slot = 0;
        }
    }
    return slots;
}

// Slot positions the game does not keep: the party is packed by the final write, and
// a Gen 1/2 box is a list.
State normalized(State state, bool listBoxes) {
    packLists(state, listBoxes);
    return state;
}

Mon monOfName(const std::string& name) {
    return name.size() > 1 && name[0] == 'M' ? std::atoi(name.c_str() + 1) : 0;
}

// What an issue tells the user about where the Pokemon is, checked against the real state.
// Returns the claim when the state contradicts it.
const char* contradictedClaim(IssueReason reason, const std::vector<SlotRef>& at, const std::vector<SlotRef>& before) {
    const bool inSave = std::any_of(at.begin(), at.end(), [](const SlotRef& slot) { return !slot.isCloud(); });
    const bool onlyInCloud = at.size() == 1 && at.front().isCloud();
    const auto cloudPlaces = static_cast<std::size_t>(std::count_if(at.begin(), at.end(),
        [](const SlotRef& slot) { return slot.isCloud(); }));
    switch (reason) {
        case IssueReason::UploadNotUndone:
        case IssueReason::DownloadNotRemoved:
            return at.size() == 2 ? nullptr : "it exists twice";
        case IssueReason::CloudRefused:
        case IssueReason::UploadNotStored:
        case IssueReason::UploadUndone:
        case IssueReason::PlacedElsewhere:
        case IssueReason::ReturnedToOrigin:
            return at.size() == 1 && inSave ? nullptr : "it is in the save only";
        case IssueReason::UploadUnconfirmed:
        case IssueReason::OverwriteUnconfirmed:
            return inSave && at.size() <= 2 ? nullptr : "it is in the save, maybe also in the cloud";
        case IssueReason::CloudSlotTrading:
        case IssueReason::CloudRemoveFailed:
        case IssueReason::CloudClaimFailed:
        case IssueReason::MovedInCloud:
            return onlyInCloud ? nullptr : "it is in the cloud only";
        case IssueReason::RemovalUnconfirmed:
        case IssueReason::SaveUnconfirmed:
            return cloudPlaces <= 1 && at.size() - cloudPlaces <= 1 ? nullptr : "it may still be in the cloud";
        case IssueReason::CloudSlotDifferent:
            return at.size() == 1 ? nullptr : "it was not copied";
        case IssueReason::DataNotLoaded:
        case IssueReason::OriginUnconfirmed:
        case IssueReason::NotInTrash:
        case IssueReason::LinkedToHeldBack:
        case IssueReason::NoParkingSlot:
        case IssueReason::WriteFailed:
        case IssueReason::RemoveFailed:
        case IssueReason::SwapCancelled:
        case IssueReason::PartyKept:
            return at == before ? nullptr : "nothing changed";
        case IssueReason::CloudMoveFailed:
            return nullptr; // no claim about the location
    }
    return nullptr;
}

struct Counts {
    std::size_t uploads = 0;
    std::size_t downloads = 0;
    std::size_t moves = 0;
    std::size_t deletes = 0;
};

// What a commit that fully succeeds must count, from the draft alone (cloud moves from the plan).
Counts draftCounts(const Case& c, const CommitPlan& plan) {
    Counts counts;
    counts.moves = plan.cloudMoves.size();
    counts.deletes = c.trashed.size();
    for (const auto& [from, mon] : c.before) {
        const std::vector<SlotRef> to = where(c.after, mon);
        if (to.empty()) {
            continue;
        }
        if (from.isCloud() != to.front().isCloud()) {
            ++(from.isCloud() ? counts.downloads : counts.uploads);
        } else if (!from.isCloud() && from != to.front()) {
            ++counts.moves;
        }
    }
    return counts;
}

// Upper bounds for the counts, from the final state.
Counts landedCounts(const Case& c, const State& finalState) {
    Counts counts;
    for (const auto& [from, mon] : c.before) {
        const std::vector<SlotRef> at = where(finalState, mon);
        const bool trashed = std::count(c.trashed.begin(), c.trashed.end(), mon) != 0;
        counts.deletes += trashed && at.empty() ? 1 : 0;
        for (const SlotRef& slot : at) {
            counts.uploads += !from.isCloud() && slot.isCloud() ? 1 : 0;
            counts.downloads += from.isCloud() && !slot.isCloud() ? 1 : 0;
        }
    }
    return counts;
}

Failures checkInvariants(const Case& c, const Outcome& outcome) {
    Failures failures;
    const CommitResult& result = outcome.result;
    const State before = normalized(c.before, c.listBoxes);
    const State draft = normalized(c.after, c.listBoxes);
    const State after = normalized(outcome.finalState, c.listBoxes);

    // 1. Every Pokemon is in exactly one place (where it was, or where the draft put it),
    //    or the report names it, and then the report is true.
    std::map<Mon, std::vector<IssueReason>> reported;
    for (const CommitIssue& issue : result.issues) {
        reported[monOfName(issue.pokemon)].push_back(issue.reason);
    }
    for (const auto& entry : c.before) {
        const Mon mon = entry.second;
        const std::string name = "M" + std::to_string(mon);
        const std::vector<SlotRef> at = placesOf(after, mon, c.listBoxes);
        const std::vector<SlotRef> was = placesOf(before, mon, c.listBoxes);
        const std::vector<SlotRef> planned = placesOf(draft, mon, c.listBoxes);
        const auto issues = reported.find(mon);
        if (issues == reported.end()) {
            if (at != was && at != planned) {
                failures.push_back(name + " is not in the report but ends at " + text(at) + " (was " + text(was)
                                   + ", draft " + text(planned) + ")");
            }
            continue;
        }
        const bool trashed = std::count(c.trashed.begin(), c.trashed.end(), mon) != 0;
        if ((at.empty() && !trashed) || at.size() > 2) {
            failures.push_back(name + " is in the report but ends at " + text(at) + " (was " + text(was) + ")");
        }
        for (const IssueReason reason : issues->second) {
            if (const char* claim = contradictedClaim(reason, at, was)) {
                failures.push_back(name + ": the report says " + claim + " (\"" + describeReason(reason)
                                   + "\") but it ends at " + text(at) + " (was " + text(was) + ")");
            }
        }
    }

    // 2. The saved party is never empty, never eggs only, and has no gaps.
    if (hasParty(c.before)) {
        std::vector<std::size_t> slots;
        bool hatched = false;
        for (const auto& [slot, mon] : outcome.finalState) {
            if (slot.kind == SlotKind::Party) {
                slots.push_back(slot.slot);
                hatched = hatched || c.eggs.count(mon) == 0;
            }
        }
        if (slots.empty()) {
            failures.push_back("the saved party is empty");
        } else if (!hatched) {
            failures.push_back("the saved party holds only eggs");
        }
        for (std::size_t index = 0; index < slots.size(); ++index) {
            if (slots[index] != index) {
                failures.push_back("the saved party has a gap before slot " + std::to_string(slots[index]));
                break;
            }
        }
    }

    // 3. The CommitResult matches the final state.
    if (result.success && result.stop != CommitStop::None) {
        failures.push_back("the result reports success and a stop");
    }
    if (result.success && result.issues.empty()) {
        if (after != draft) {
            failures.push_back("clean commit, but the final state " + text(after) + " is not the draft " + text(draft));
        }
        const Counts want = draftCounts(c, outcome.plan);
        if (result.uploads != want.uploads || result.downloads != want.downloads || result.moves != want.moves
            || result.deletes != want.deletes) {
            failures.push_back("clean commit, but the counts differ from the draft");
        }
    }
    const Counts landed = landedCounts(c, outcome.finalState);
    if (result.uploads > landed.uploads || result.downloads > landed.downloads || result.deletes > landed.deletes) {
        failures.push_back("the result counts more uploads, downloads or deletes than the final state shows");
    }

    if (!failures.empty()) {
        failures.push_back("result: " + text(result));
        failures.push_back("before " + text(c.before) + ", draft " + text(c.after) + ", final " + text(outcome.finalState));
    }
    return failures;
}

void append(Failures& to, const Failures& from) { to.insert(to.end(), from.begin(), from.end()); }

void expectInvariants(Failures& failures, const Case& c, const std::vector<Fault>& faults) {
    append(failures, checkInvariants(c, execute(c, faults)));
}

// The draft has no conflict and no fault is injected: the commit must apply it all.
void expectClean(Failures& failures, const Case& c) {
    const Outcome outcome = execute(c, {});
    append(failures, checkInvariants(c, outcome));
    if (!outcome.result.success || !outcome.result.issues.empty()) {
        failures.push_back("expected a clean commit, got " + text(outcome.result));
        failures.push_back("before " + text(c.before) + ", draft " + text(c.after));
    }
}

// ---------------------------------------------------------------- hand-written scenarios

TEST(local_move) { expectClean(failures, scenario({{box(0, 0), 1}}, {{box(1, 5), 1}})); }

TEST(local_swap) {
    expectClean(failures, scenario({{box(0, 0), 1}, {box(0, 1), 2}}, {{box(0, 0), 2}, {box(0, 1), 1}}));
}

TEST(party_and_box_exchange) {
    expectClean(failures, scenario({{party(1), 1}, {box(0, 0), 2}}, {{box(0, 3), 1}, {party(1), 2}}));
}

TEST(upload) { expectClean(failures, scenario({{box(0, 0), 1}}, {{bank(3), 1}})); }

TEST(download) { expectClean(failures, scenario({{bank(0), 1}}, {{box(0, 0), 1}})); }

TEST(swap_between_save_and_cloud) {
    expectClean(failures, scenario({{box(0, 0), 1}, {bank(0), 2}}, {{box(0, 0), 2}, {bank(0), 1}}));
}

TEST(swap_between_party_and_cloud) {
    expectClean(failures, scenario({{party(1), 1}, {bank(0), 2}}, {{party(1), 2}, {bank(0), 1}}));
}

TEST(cloud_move) { expectClean(failures, scenario({{bank(0), 1}}, {{bank(7, 1), 1}})); }

TEST(cloud_swap) {
    expectClean(failures, scenario({{bank(0), 1}, {bank(1), 2}}, {{bank(0), 2}, {bank(1), 1}}));
}

TEST(local_delete) { expectClean(failures, scenario({{box(0, 0), 1}}, {}, {1})); }

TEST(cloud_delete) { expectClean(failures, scenario({{bank(0), 1}}, {}, {1})); }

TEST(more_uploads_than_one_batch) {
    State before;
    State after;
    for (Mon mon = 1; mon <= 40; ++mon) {
        before[box(mon / BoxSlotCount, mon % BoxSlotCount)] = mon;
        after[bank(mon % BoxSlotCount, static_cast<std::uint16_t>(mon / BoxSlotCount))] = mon;
    }
    expectClean(failures, scenario(before, after));
}

// Uploads, a swap through a parking slot, a download, cloud and local deletes,
// a party-to-box move and a cloud move, in one commit.
Case mixedScenario() {
    return scenario({{box(0, 0), 1}, {box(0, 1), 2}, {box(0, 2), 3}, {party(1), 11},
                     {bank(0), 20}, {bank(1), 21}, {bank(2), 22}, {bank(4), 23}},
                    {{bank(3), 1}, {bank(0), 2}, {box(0, 1), 20}, {box(1, 0), 21}, {box(1, 1), 11}, {bank(5), 23}},
                    {22, 3});
}

// Cloud moves that free the slot a download leaves from, then an upload into it.
Case cloudChainScenario() {
    return scenario({{bank(0), 30}, {bank(1), 31}, {bank(2), 32}, {box(0, 0), 33}},
                    {{bank(1), 30}, {bank(2), 31}, {box(0, 1), 32}, {bank(0), 33}});
}

// A Gen 1/2 save, where every write packs the boxes: a move out of a box, and a swap
// between the box and the cloud (#1 finding 3).
Case listBoxScenario() {
    Case c = scenario({{box(0, 0), 1}, {box(0, 1), 2}, {box(0, 2), 3}, {box(0, 3), 4}, {bank(0), 5}},
                      {{bank(0), 1}, {box(0, 0), 5}, {box(1, 0), 2}, {box(0, 2), 3}, {box(0, 3), 4}});
    c.listBoxes = true;
    return c;
}

// Swaps that go through the party: party <-> cloud and party <-> box.
Case partySwapScenario() {
    Case c;
    c.before = {{party(0), 40}, {party(1), 41}, {party(2), 42}, {box(0, 0), 43}, {bank(0), 44}};
    c.after = {{party(0), 44}, {party(1), 43}, {party(2), 42}, {box(0, 0), 41}, {bank(0), 40}};
    return c;
}

TEST(mixed) { expectClean(failures, mixedScenario()); }

TEST(cloud_chain) { expectClean(failures, cloudChainScenario()); }

TEST(party_swaps) { expectClean(failures, partySwapScenario()); }

TEST(list_boxes) { expectClean(failures, listBoxScenario()); }

TEST(no_change) { expectClean(failures, scenario({{box(0, 0), 1}, {bank(0), 2}}, {{box(0, 0), 1}, {bank(0), 2}})); }

// ---------------------------------------------------------------- failure injection

const char* callName(Call call) {
    switch (call) {
        case Call::MoveCloud: return "moveCloud";
        case Call::UploadBatch: return "uploadBatch";
        case Call::ReadCloudBox: return "readCloudBox";
        case Call::DeleteCloud: return "deleteCloud";
        case Call::ClaimCloud: return "claimCloud";
        case Call::WriteLocal: return "writeLocal";
        case Call::ClearLocal: return "clearLocal";
        case Call::PersistLocal: return "persistLocal";
        case Call::Count: break;
    }
    return "?";
}

const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::Refused: return "refused";
        case Mode::UnknownNotApplied: return "unknown-not-applied";
        case Mode::UnknownApplied: return "unknown-applied";
        case Mode::Fail: return "fail";
        case Mode::RejectFirst: return "reject-first";
        case Mode::ChangedFirst: return "changed-first";
        case Mode::LockedFirst: return "locked-first";
        case Mode::DoneMissingLast: return "done-missing-last";
        case Mode::RejectFirstStoreRest: return "reject-first-store-rest";
        case Mode::FailButLanded: return "fail-but-landed";
        case Mode::DeleteFirstThenFail: return "delete-first-then-fail";
        case Mode::UnknownCleared: return "unknown-cleared";
        case Mode::FailDiskUnknown: return "fail-disk-unknown";
    }
    return "?";
}

std::vector<Mode> modesFor(Call call) {
    switch (call) {
        case Call::MoveCloud:
            return {Mode::Refused, Mode::UnknownNotApplied, Mode::UnknownApplied};
        case Call::DeleteCloud:
            return {Mode::Refused, Mode::UnknownNotApplied, Mode::UnknownApplied, Mode::DeleteFirstThenFail};
        case Call::UploadBatch:
            return {Mode::Refused, Mode::UnknownNotApplied, Mode::UnknownApplied, Mode::RejectFirst,
                    Mode::DoneMissingLast, Mode::RejectFirstStoreRest, Mode::UnknownCleared};
        // The fake's claim changes nothing in the cloud (it creates no event Pokemon),
        // so "applied" and "not applied" are the same run.
        case Call::ClaimCloud:
            return {Mode::Refused, Mode::UnknownNotApplied, Mode::ChangedFirst, Mode::LockedFirst};
        case Call::PersistLocal:
            return {Mode::Fail, Mode::FailButLanded, Mode::FailDiskUnknown};
        case Call::ReadCloudBox:
        case Call::WriteLocal:
        case Call::ClearLocal:
        case Call::Count:
            break;
    }
    return {Mode::Fail};
}

// Sweep runs that break an invariant because of a known bug. Each must still fail:
// when one passes, the fix removes it here. Each entry names its issue.
const std::map<std::string, const char*> KnownFailures = {};

struct SweepScenario {
    const char* name;
    Case c;
};

struct SweepBase {
    const char* name;
    std::vector<Fault> faults;
};

// Runs each scenario once per (call, occurrence, mode), with that one call failing, on top of
// each base. The bases reach calls a clean run never makes: the cloud box read after an unknown
// upload, the retry after a partial refusal, and the cloud deletes that undo uploads.
TEST(fault_injection_sweep) {
    const std::vector<SweepScenario> scenarios = {
        {"mixed", mixedScenario()}, {"cloud-chain", cloudChainScenario()}, {"party-swaps", partySwapScenario()},
        {"list-boxes", listBoxScenario()}};
    const std::vector<SweepBase> bases = {{"clean", {}},
                                          {"upload1-unknown", {{Call::UploadBatch, 1, Mode::UnknownApplied}}},
                                          {"upload1-reject-first", {{Call::UploadBatch, 1, Mode::RejectFirst}}},
                                          {"final-save-fails", {{Call::PersistLocal, 2, Mode::Fail}}}};
    std::map<std::string, Failures> broken;
    CallCounts covered{};
    int runs = 0;
    for (const SweepScenario& scenario : scenarios) {
        for (const SweepBase& base : bases) {
            const Outcome reference = execute(scenario.c, base.faults);
            if (const Failures found = checkInvariants(scenario.c, reference); !found.empty()) {
                broken[std::string(scenario.name) + "/" + base.name] = found;
            }
            for (std::size_t call = 0; call < covered.size(); ++call) {
                for (int occurrence = 1; occurrence <= reference.calls[call]; ++occurrence) {
                    const Call which = static_cast<Call>(call);
                    const bool taken = std::any_of(base.faults.begin(), base.faults.end(), [&](const Fault& fault) {
                        return fault.call == which && fault.occurrence == occurrence;
                    });
                    if (taken) {
                        continue;
                    }
                    ++covered[call];
                    for (const Mode mode : modesFor(which)) {
                        std::vector<Fault> faults = base.faults;
                        faults.push_back({which, occurrence, mode});
                        ++runs;
                        const Failures found = checkInvariants(scenario.c, execute(scenario.c, faults));
                        if (!found.empty()) {
                            broken[std::string(scenario.name) + "/" + base.name + "/" + callName(which) + "#"
                                   + std::to_string(occurrence) + "/" + modeName(mode)] = found;
                        }
                    }
                }
            }
        }
    }
    for (std::size_t call = 0; call < covered.size(); ++call) {
        if (covered[call] == 0) {
            failures.push_back(std::string("no sweep scenario calls ") + callName(static_cast<Call>(call)));
        }
    }
    for (const auto& [key, found] : broken) {
        if (KnownFailures.count(key) == 0) {
            failures.push_back(key + ":");
            for (const std::string& line : found) {
                failures.push_back("    " + line);
            }
        }
    }
    for (const auto& [key, finding] : KnownFailures) {
        if (broken.count(key) == 0) {
            failures.push_back(key + " now passes (" + finding + "): remove it from KnownFailures");
        }
    }
    std::printf("      fault injection: %d runs, %zu break an invariant, %zu known\n", runs, broken.size(),
                KnownFailures.size());
}

// ---------------------------------------------------------------- randomized drafts

// A party of 1 to 6, a few Pokemon in two local boxes and two cloud boxes, then 1 to 8 random
// swaps or deletes. Like the UI, the draft never empties the party; here it also keeps one
// member that is not an egg. Half of the saves have list boxes (Gen 1/2), packed from the start.
Case randomCase(std::mt19937& rng) {
    const auto pick = [&rng](std::size_t count) {
        return std::uniform_int_distribution<std::size_t>(0, count - 1)(rng);
    };
    std::vector<SlotRef> slots;
    for (std::size_t slot = 0; slot < PartySlotCount; ++slot) {
        slots.push_back(party(slot));
    }
    for (std::uint16_t index = 0; index < 2; ++index) {
        for (std::size_t slot = 0; slot < BoxSlotCount; ++slot) {
            slots.push_back(box(index, slot));
            slots.push_back(bank(slot, index));
        }
    }
    Case c;
    Mon next = 1;
    const std::size_t partySize = 1 + pick(PartySlotCount);
    for (std::size_t slot = 0; slot < partySize; ++slot) {
        c.before[party(slot)] = next++;
    }
    for (std::size_t count = 4 + pick(16); count > 0; --count) {
        SlotRef slot;
        do {
            slot = slots[PartySlotCount + pick(slots.size() - PartySlotCount)];
        } while (c.before.count(slot));
        c.before[slot] = next++;
    }
    c.listBoxes = pick(2) == 0;
    if (c.listBoxes) {
        packLists(c.before, true);
    }
    for (const auto& [slot, mon] : c.before) {
        if (slot != party(0) && pick(6) == 0) {
            c.eggs.insert(mon);
        }
    }
    c.after = c.before;
    for (std::size_t operations = 1 + pick(8); operations > 0; --operations) {
        State draft = c.after;
        std::vector<Mon> trashed = c.trashed;
        const auto chosen = std::next(draft.begin(), static_cast<std::ptrdiff_t>(pick(draft.size())));
        if (pick(6) == 0) {
            trashed.push_back(chosen->second);
            draft.erase(chosen);
        } else {
            swapSlots(draft, chosen->first, slots[pick(slots.size())]);
        }
        if (hasHatchedParty(draft, c.eggs)) {
            c.after = std::move(draft);
            c.trashed = std::move(trashed);
        }
    }
    return c;
}

// Each draft runs once clean, then once with one random call failing.
TEST(randomized_drafts) {
    constexpr std::uint32_t Drafts = 1000;
    for (std::uint32_t index = 0; index < Drafts; ++index) {
        const std::uint32_t seed = runSeed + index;
        std::mt19937 rng(seed);
        const Case c = randomCase(rng);
        const Outcome clean = execute(c, {});
        Failures found = checkInvariants(c, clean);
        // A list box parks a Pokemon only after its last one, so a draft can lack a parking slot.
        const bool heldBack = c.listBoxes && !clean.plan.heldBack.empty();
        if (!heldBack && (!clean.result.success || !clean.result.issues.empty())) {
            found.push_back("expected a clean commit, got " + text(clean.result));
            found.push_back("before " + text(c.before) + ", draft " + text(c.after));
        }
        std::vector<Fault> faults;
        for (std::size_t call = 0; call < clean.calls.size(); ++call) {
            for (int occurrence = 1; occurrence <= clean.calls[call]; ++occurrence) {
                for (const Mode mode : modesFor(static_cast<Call>(call))) {
                    faults.push_back({static_cast<Call>(call), occurrence, mode});
                }
            }
        }
        if (!faults.empty()) {
            const Fault fault = faults[std::uniform_int_distribution<std::size_t>(0, faults.size() - 1)(rng)];
            const Failures faulty = checkInvariants(c, execute(c, {fault}));
            if (!faulty.empty()) {
                found.push_back(std::string("with ") + callName(fault.call) + "#" + std::to_string(fault.occurrence)
                                + "/" + modeName(fault.mode) + ":");
                append(found, faulty);
            }
        }
        if (!found.empty()) {
            failures.push_back("draft seed " + std::to_string(seed) + " (rerun alone: make test SEED="
                               + std::to_string(seed) + ")");
            append(failures, found);
            return;
        }
    }
}

// ---------------------------------------------------------------- issue #1 regressions
// Each scenario of issue #1 (commit integrity).

// Finding 1: the server answers Done but stored, and counts, one item fewer.
TEST(issue1_f1_upload_reply_count_checked) {
    expectInvariants(failures, scenario({{box(0, 0), 1}, {box(0, 1), 2}}, {{bank(0), 1}, {bank(1), 2}}),
                     {{Call::UploadBatch, 1, Mode::DoneMissingLast}});
}

// Finding 1: the server refused one item but stored the others; the retry is refused as occupied.
TEST(issue1_f1_retry_after_partial_store) {
    expectInvariants(failures,
                     scenario({{box(0, 0), 1}, {box(0, 1), 2}, {box(0, 2), 3}}, {{bank(0), 1}, {bank(1), 2}, {bank(2), 3}}),
                     {{Call::UploadBatch, 1, Mode::RejectFirstStoreRest}});
}

// Finding 2: the final save reports an error but reached the disk; the upload is kept.
TEST(issue1_f2_final_save_error_but_landed) {
    const Case c = scenario({{box(0, 0), 1}}, {{bank(0), 1}});
    const Outcome outcome = execute(c, {{Call::PersistLocal, 1, Mode::FailButLanded}});
    append(failures, checkInvariants(c, outcome));
    if (!outcome.result.success || !outcome.result.issues.empty()) {
        failures.push_back("expected a clean commit, got " + text(outcome.result));
    }
}

// Finding 3: a Gen 1/2 save packs its boxes on every write; a move out and a swap in one commit.
TEST(issue1_f3_gen12_move_out_and_swap) { expectClean(failures, listBoxScenario()); }

// Finding 4: party [A, B], cloud C dropped into the party, A and B moved to a box; C's claim times out.
TEST(issue1_f4_claim_drop_keeps_party) {
    Case c;
    c.before = {{party(0), 1}, {party(1), 2}, {bank(0), 3}};
    c.after = {{party(2), 3}, {box(0, 0), 1}, {box(0, 1), 2}};
    expectInvariants(failures, c, {{Call::ClaimCloud, 1, Mode::UnknownNotApplied}});
}

// Finding 4: the same with an egg left behind.
TEST(issue1_f4_claim_drop_keeps_a_hatched_member) {
    Case c;
    c.before = {{party(0), 1}, {party(1), 2}, {bank(0), 3}};
    c.after = {{party(0), 1}, {party(2), 3}, {box(0, 0), 2}};
    c.eggs = {1};
    expectInvariants(failures, c, {{Call::ClaimCloud, 1, Mode::UnknownNotApplied}});
}

// Finding 5: a party member leaves for the cloud, another for a box; the save fails.
TEST(issue1_f5_failed_save_leaves_no_party_gap) {
    Case c;
    c.before = {{party(0), 1}, {party(1), 2}, {party(2), 3}};
    c.after = {{bank(0), 1}, {box(0, 0), 2}, {party(2), 3}};
    expectInvariants(failures, c, {{Call::PersistLocal, 1, Mode::Fail}});
}

// Finding 6: the box listing has no payload for an occupied slot; the upload stays unconfirmed.
TEST(issue1_f6_listing_without_payload) {
    Case c = scenario({{box(0, 0), 1}}, {{bank(0), 1}});
    c.listingWithoutPayload = true;
    expectInvariants(failures, c, {{Call::UploadBatch, 1, Mode::UnknownApplied}});
}

// Finding 7: the per-slot delete fallback deleted the first slot, then failed.
TEST(issue1_f7_partial_cloud_delete) {
    expectInvariants(failures, scenario({{bank(0), 1}, {bank(1), 2}}, {{box(0, 0), 1}, {box(0, 1), 2}}),
                     {{Call::DeleteCloud, 1, Mode::DeleteFirstThenFail}});
}

// Finding 8 is a UI bug: the hand overwrites a slot, so a Pokemon vanishes from the draft.
// The planner holds that group back and the save stays as it was.
TEST(issue1_f8_vanished_pokemon_is_held_back) {
    const Case c = scenario({{box(0, 0), 1}, {box(0, 1), 2}}, {{box(0, 0), 2}});
    const Outcome outcome = execute(c, {});
    append(failures, checkInvariants(c, outcome));
    if (outcome.finalState != c.before || outcome.result.issues.empty()) {
        failures.push_back("expected nothing to change and an issue, got " + text(outcome.result));
    }
}

// Finding 9: a group dropped at claim time after its cloud moves ran.
TEST(issue1_f9_claim_drop_after_cloud_move) {
    expectInvariants(failures, scenario({{bank(0), 1}, {bank(1), 2}}, {{bank(1), 1}, {box(0, 0), 2}}),
                     {{Call::ClaimCloud, 1, Mode::UnknownNotApplied}});
}

// Finding 10: A 1->2, B 2->3, C 3->box; both moves run, so C is in slot 1. Then the save fails.
TEST(issue1_f10_save_fails_after_cloud_moves) {
    const Case c = scenario({{bank(0), 1}, {bank(1), 2}, {bank(2), 3}}, {{bank(1), 1}, {bank(2), 2}, {box(0, 0), 3}});
    const Outcome outcome = execute(c, {{Call::PersistLocal, 1, Mode::Fail}});
    append(failures, checkInvariants(c, outcome));
    const bool named = std::any_of(outcome.result.issues.begin(), outcome.result.issues.end(), [](const CommitIssue& issue) {
        return issue.pokemon == "M3" && issue.location == bank(0);
    });
    if (!named) {
        failures.push_back("expected M3 reported in bank0.0, got " + text(outcome.result));
    }
}

// Finding 10 through the first save: the cloud chain parks M32, then that save fails.
TEST(issue1_f10_first_save_fails_after_cloud_moves) {
    const Case c = cloudChainScenario();
    const Outcome outcome = execute(c, {{Call::PersistLocal, 1, Mode::Fail}});
    append(failures, checkInvariants(c, outcome));
    const bool named = std::any_of(outcome.result.issues.begin(), outcome.result.issues.end(), [](const CommitIssue& issue) {
        return issue.pokemon == "M32" && issue.location == bank(0) && issue.reason == IssueReason::MovedInCloud;
    });
    if (outcome.result.stop != CommitStop::SaveFailed || !named) {
        failures.push_back("expected a SaveFailed stop and M32 reported in bank0.0, got " + text(outcome.result));
    }
}

// Finding 2: a failed write whose disk cannot be read back may have landed: the upload stays.
TEST(issue1_f2_unknown_disk_keeps_the_upload) {
    const Case c = scenario({{box(0, 0), 1}}, {{bank(0), 1}});
    const Outcome outcome = execute(c, {{Call::PersistLocal, 1, Mode::FailDiskUnknown}});
    append(failures, checkInvariants(c, outcome));
    if (outcome.finalState.count(bank(0)) == 0) {
        failures.push_back("the upload was undone although the save may have cleared its slot");
    }
}

// Finding 6: the upload answers Unknown and the listing shows its slot empty: the Pokemon it
// replaced may be gone from the cloud, so the parked copy stays in the save.
TEST(issue1_f6_emptied_slot_keeps_the_parked_download) {
    expectInvariants(failures, scenario({{box(0, 0), 1}, {bank(0), 2}}, {{box(0, 0), 2}, {bank(0), 1}}),
                     {{Call::UploadBatch, 1, Mode::UnknownCleared}});
}

// A cloud move that answers Unknown but landed is confirmed by the listing; the commit goes on.
TEST(unknown_cloud_move_confirmed_by_listing) {
    const Case c = cloudChainScenario();
    const Outcome outcome = execute(c, {{Call::MoveCloud, 1, Mode::UnknownApplied}});
    append(failures, checkInvariants(c, outcome));
    if (!outcome.result.success || !outcome.result.issues.empty()) {
        failures.push_back("expected a clean commit, got " + text(outcome.result));
    }
}

} // namespace

int main(int argc, char** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "--seed") {
            runSeed = static_cast<std::uint32_t>(std::strtoul(argv[index + 1], nullptr, 10));
        }
    }
    int failed = 0;
    for (const TestCase& test : registry()) {
        Failures failures;
        test.body(failures);
        if (test.expectedFailure) {
            const auto expected = std::find_if(failures.begin(), failures.end(), [&](const std::string& failure) {
                return failure.find(test.expectedFailure) != std::string::npos;
            });
            if (failures.empty()) {
                ++failed;
                std::printf("XPASS %s: the bug looks fixed, turn its EXPECTED_FAILURE into a TEST\n", test.name);
            } else if (expected == failures.end()) {
                ++failed;
                std::printf("FAIL  %s: expected a failure with \"%s\", got:\n", test.name, test.expectedFailure);
                for (const std::string& failure : failures) {
                    std::printf("      %s\n", failure.c_str());
                }
            } else {
                std::printf("xfail %s: %s\n", test.name, expected->c_str());
            }
        } else if (!failures.empty()) {
            ++failed;
            std::printf("FAIL  %s\n", test.name);
            for (const std::string& failure : failures) {
                std::printf("      %s\n", failure.c_str());
            }
        } else {
            std::printf("ok    %s\n", test.name);
        }
    }
    std::printf("%zu tests, %d failed (seed %u)\n", registry().size(), failed, runSeed);
    return failed == 0 ? 0 : 1;
}
