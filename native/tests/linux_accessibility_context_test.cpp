#include "meeting-record-detect.h"
#include <atomic>
#include <atspi/atspi.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

// A GTK/WebKit host owns its libatspi connection on the UI thread. The SDK must
// use a separate transport without moving or dispatching the host's cache.
int main() {
  dbus_threads_init_default();
  assert(atspi_init() == 0);
  const auto ui_thread = std::this_thread::get_id();
  std::atomic<unsigned> messages{0}, ui_dispatches{0};
  struct State {
    std::thread::id ui;
    std::atomic<unsigned> *messages, *ui_dispatches;
  } state{ui_thread, &messages, &ui_dispatches};
  auto loop = g_main_loop_new(nullptr, FALSE);
  std::thread worker([&] {
    assert(mrec_accessibility_permission_status() == MREC_PERM_GRANTED);
    auto bus = atspi_get_a11y_bus();
    const auto filter = +[](DBusConnection *, DBusMessage *, void *data) {
      auto &s = *static_cast<State *>(data);
      ++*s.messages;
      if (std::this_thread::get_id() == s.ui)
        ++*s.ui_dispatches;
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    };
    assert(dbus_connection_add_filter(bus, filter, &state, nullptr));
    for (int i = 0; i < 10; ++i) {
      auto message = dbus_message_new_method_call(
          DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetId");
      assert(dbus_connection_send(bus, message, nullptr));
      dbus_message_unref(message);
      // Host traffic must keep dispatching on its UI thread during SDK scans.
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      mrec_meeting found[8];
      size_t count = 0;
      assert(mrec_scan(found, 8, &count) == MREC_OK);
    }
    dbus_connection_remove_filter(bus, filter, &state);
    g_idle_add(
        +[](void *data) {
          g_main_loop_quit(static_cast<GMainLoop *>(data));
          return G_SOURCE_REMOVE;
        },
        loop);
  });
  g_main_loop_run(loop);
  worker.join();
  g_main_loop_unref(loop);
  assert(messages > 0);
  std::cout << "AT-SPI messages=" << messages
            << " host UI dispatches=" << ui_dispatches << std::endl;
  assert(ui_dispatches == messages);
}
