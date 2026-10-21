#include "../windows/CaptureRoots.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <vector>

namespace {

using meeting_record::InTrees;
using meeting_record::SelectCaptureRoots;
using meeting_record::UncapturedRoots;

constexpr int kOther = 0;
constexpr int kTeams = 1;

struct Process {
  uint32_t parent;
  int app;
};

using Processes = std::map<uint32_t, Process>;
using Pids = std::vector<uint32_t>;

const auto kTeamsProcess = [](uint32_t, const Process &process) { return process.app == kTeams; };

/* New Teams: the app, its WebView helper, a nested copy, and a call host the system starts. */
Processes TeamsCall() {
  return {
      {1, {0, kOther}},     {10, {1, kTeams}},  {11, {10, kOther}}, {40, {10, kTeams}},
      {900, {1, kOther}},   {20, {900, kTeams}}, {21, {20, kOther}}, {30, {1, kOther}},
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
  const Pids captured{10, 20};
  assert(UncapturedRoots(processes, SelectCaptureRoots(processes, {}, kTeamsProcess), captured)
             .empty());
  processes[50] = {900, kTeams};
  processes[51] = {50, kOther};
  assert(UncapturedRoots(processes, SelectCaptureRoots(processes, {}, kTeamsProcess), captured) ==
         Pids{50});
}

void ExitedRootsKeepTheirChildren() {
  auto processes = TeamsCall();
  processes.erase(20);
  processes[22] = {20, kTeams};
  // The orphan still belongs to the captured tree it was started in.
  assert(UncapturedRoots(processes, SelectCaptureRoots(processes, {}, kTeamsProcess),
                         Pids{10, 20})
             .empty());
}

void ParentCyclesTerminate() {
  Processes processes{{60, {61, kTeams}}, {61, {60, kTeams}}, {62, {62, kTeams}}};
  // Each process in the cycle sits inside the other's tree; a self-parented one is a root.
  assert(SelectCaptureRoots(processes, {}, kTeamsProcess) == Pids{62});
  assert(!InTrees(processes, 60, Pids{70}));
  assert(UncapturedRoots(processes, Pids{62}, Pids{62}).empty());
}

} // namespace

int main() {
  SiblingsOutsideTheTreeBecomeRoots();
  HelpersStartedLaterAreUncaptured();
  ExitedRootsKeepTheirChildren();
  ParentCyclesTerminate();
  std::cout << "capture root tests passed\n";
}
