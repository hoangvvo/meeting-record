#include "../windows/AudioAppProcess.h"

#include <cassert>
#include <iostream>

int main() {
  using meeting_record::AudioAppProcess;
  using meeting_record::FindAudioAppRoot;
  const std::map<uint32_t, AudioAppProcess> processes = {
      {1, {1, 0, L"explorer.exe", true}},
      {10, {10, 1, L"chrome.exe", true}},
      {11, {11, 10, L"chrome.exe"}},
      {12, {12, 11, L"chrome.exe"}},
      {20, {20, 1, L"chrome.exe", true}},
      {21, {21, 20, L"chrome.exe"}},
      {30, {30, 1, L"Zoom.exe", true}},
      {31, {31, 30, L"CptHost.exe"}},
      {40, {40, 1, L"Discord.exe"}},
      {41, {41, 40, L"discord.exe"}},
      {50, {50, 999, L"player.exe"}},
      {60, {60, 10, L"player.exe", true}},
  };

  // An audio-only helper must target the browser root, including its siblings.
  assert(FindAudioAppRoot(processes, 12, 1)->pid == 10);
  assert(FindAudioAppRoot(processes, 11, 1)->pid == 10);
  assert(FindAudioAppRoot(processes, 10, 1)->pid == 10);
  // Equal executable names do not merge independently launched app instances.
  assert(FindAudioAppRoot(processes, 21, 1)->pid == 20);
  // A helper can use a different executable from its owning application.
  assert(FindAudioAppRoot(processes, 31, 1)->pid == 30);
  // Tray apps still resolve to their root when no app window is visible.
  assert(FindAudioAppRoot(processes, 41, 1)->pid == 40);
  assert(FindAudioAppRoot(processes, 50, 1)->pid == 50);
  // An independently windowed child remains a separate user-visible app.
  assert(FindAudioAppRoot(processes, 60, 1)->pid == 60);
  assert(FindAudioAppRoot(processes, 1, 1) == nullptr);
  assert(FindAudioAppRoot(processes, 999, 1) == nullptr);
  std::cout << "audio application process tests passed\n";
}
