#include "../windows/CaptureRoots.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <vector>

namespace {

using meeting_record::InRootTrees;
using meeting_record::InTrees;
using meeting_record::Root;
using meeting_record::SelectCaptureRoots;
using meeting_record::UncapturedRoots;

constexpr int kOther = 0;
constexpr int kTeams = 1;

struct Process {
  uint32_t parent;
  int app;
  uint64_t created = 0;
};

using Processes = std::map<uint32_t, Process>;
using Pids = std::vector<uint32_t>;
using Roots = std::vector<Root>;

const auto kTeamsProcess = [](uint32_t, const Process &process) { return process.app == kTeams; };

/* Each process's creation time, as the backend reads it; 0 when it is unknown. */
auto CreatedIn(const Processes &processes) {
  return [&processes](uint32_t pid) {
    const auto process = processes.find(pid);
    return process == processes.end() ? uint64_t{0} : process->second.created;
  };
}

/* New Teams: the app, its WebView helper, a nested copy, and a call host the system starts. */
Processes TeamsCall() {
  return {
      {1, {0, kOther, 1}},     {10, {1, kTeams, 100}}, {11, {10, kOther, 110}},
      {40, {10, kTeams, 120}}, {900, {1, kOther, 5}},  {20, {900, kTeams, 130}},
      {21, {20, kOther, 140}}, {30, {1, kOther, 150}},
  };
}

void SiblingsOutsideTheTreeBecomeRoots() {
  const auto processes = TeamsCall();
  // The nested copy is already inside the app's tree; the call host is not.
  assert(SelectCaptureRoots(processes, Pids{10}, kTeamsProcess) == (Pids{10, 20}));
  assert(InTrees(processes, 21, Pids{20}) && !InTrees(processes, 30, Pids{10, 20}));
}

void HelpersStartedLaterAreUncaptured() {
  auto processes = TeamsCall();
  const Roots captured{{10, 100}, {20, 130}};
  const auto created = CreatedIn(processes);
  assert(UncapturedRoots(processes, SelectCaptureRoots(processes, {}, kTeamsProcess), captured,
                         created)
             .empty());
  processes[50] = {900, kTeams, 200};
  processes[51] = {50, kOther, 210};
  assert(UncapturedRoots(processes, SelectCaptureRoots(processes, {}, kTeamsProcess), captured,
                         created) == Pids{50});
}

void ExitedRootsKeepTheirChildren() {
  auto processes = TeamsCall();
  processes.erase(20);
  processes[22] = {20, kTeams, 135};
  const auto created = CreatedIn(processes);
  // The orphan still belongs to the captured tree it was started in.
  assert(InRootTrees(processes, 22, Roots{{20, 130}}, created));
  assert(UncapturedRoots(processes, SelectCaptureRoots(processes, {}, kTeamsProcess),
                         Roots{{10, 100}, {20, 130}}, created)
             .empty());
}

void ReusedPidsAreNotTheCapturedProcess() {
  auto processes = TeamsCall();
  const auto created = CreatedIn(processes);
  const Roots captured{{20, 130}};
  // The call host exits, and an unrelated process reuses its pid with a child.
  processes.erase(21);
  processes[20] = {1, kOther, 500};
  processes[23] = {20, kOther, 510};
  assert(!InRootTrees(processes, 20, captured, created));
  assert(!InRootTrees(processes, 23, captured, created));
  // A child left by the call host predates the reuse, so it is still captured.
  processes[24] = {20, kOther, 140};
  assert(InRootTrees(processes, 24, captured, created));
  // The same app reusing the pid is a new process to capture.
  processes[20].app = kTeams;
  assert(UncapturedRoots(processes, Pids{20}, captured, created) == Pids{20});
}

void ReusedIntermediateParentsBreakTheChain() {
  Processes processes{{1, {0, kOther, 1}}, {10, {1, kTeams, 100}}, {31, {12, kOther, 300}}};
  const auto created = CreatedIn(processes);
  const Roots captured{{10, 100}};
  // 31's parent 12 exited, and a later process took pid 12: 31 is not its child.
  processes[12] = {99, kOther, 650};
  assert(!InRootTrees(processes, 31, captured, created));
  // A parent created before the child is the real one.
  processes[12] = {10, kOther, 200};
  assert(InRootTrees(processes, 31, captured, created));
}

void UnknownTimesMatchPids() {
  const Processes processes{{10, {1, kTeams, 0}}, {11, {10, kOther, 0}}};
  const auto created = CreatedIn(processes);
  assert(InRootTrees(processes, 11, Roots{{10, 100}}, created));
  assert(InRootTrees(processes, 10, Roots{{10, 0}}, created));
}

void ParentCyclesTerminate() {
  Processes processes{{60, {61, kTeams, 5}}, {61, {60, kTeams, 6}}, {62, {62, kTeams, 7}}};
  // Each process in the cycle sits inside the other's tree; a self-parented one is a root.
  assert(SelectCaptureRoots(processes, {}, kTeamsProcess) == Pids{62});
  assert(!InTrees(processes, 60, Pids{70}));
  assert(!InRootTrees(processes, 60, Roots{{70, 1}}, CreatedIn(processes)));
  assert(UncapturedRoots(processes, Pids{62}, Roots{{62, 7}}, CreatedIn(processes)).empty());
}

} // namespace

int main() {
  SiblingsOutsideTheTreeBecomeRoots();
  HelpersStartedLaterAreUncaptured();
  ExitedRootsKeepTheirChildren();
  ReusedPidsAreNotTheCapturedProcess();
  ReusedIntermediateParentsBreakTheChain();
  UnknownTimesMatchPids();
  ParentCyclesTerminate();
  std::cout << "capture root tests passed\n";
}
