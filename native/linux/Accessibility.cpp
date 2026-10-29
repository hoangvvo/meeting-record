#include "Accessibility.h"
#include "Process.h"
#include "meeting-catalog.h"
#include <atspi/atspi-constants.h>
#include <cstring>
#include <dbus/dbus.h>
#include <functional>
#include <memory>
#include <mutex>

namespace meeting_record::linux_backend {
namespace {
constexpr const char *kAccessible = "org.a11y.atspi.Accessible";
constexpr const char *kDocument = "org.a11y.atspi.Document";
std::mutex access_lock;
std::once_flag dbus_threads;
using Message = std::unique_ptr<DBusMessage, decltype(&dbus_message_unref)>;
struct Ref {
  std::string bus, path;
  explicit operator bool() const {
    return !bus.empty() && !path.empty() && path != "/org/a11y/atspi/null" &&
           dbus_validate_bus_name(bus.c_str(), nullptr) &&
           dbus_validate_path(path.c_str(), nullptr);
  }
};
std::string String(DBusMessageIter &iter, int type = DBUS_TYPE_STRING) {
  if (dbus_message_iter_get_arg_type(&iter) != type)
    return {};
  const char *value = nullptr;
  dbus_message_iter_get_basic(&iter, &value);
  return value ? std::string(value, strnlen(value, 4096)) : std::string{};
}
Ref Reference(DBusMessageIter &iter) {
  if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRUCT)
    return {};
  DBusMessageIter fields;
  dbus_message_iter_recurse(&iter, &fields);
  auto bus = String(fields);
  if (!dbus_message_iter_next(&fields))
    return {};
  return {bus, String(fields, DBUS_TYPE_OBJECT_PATH)};
}
std::vector<Ref> References(DBusMessage *message, size_t limit) {
  std::vector<Ref> result;
  DBusMessageIter iter, array;
  if (!message || !dbus_message_iter_init(message, &iter) ||
      dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
    return result;
  dbus_message_iter_recurse(&iter, &array);
  while (result.size() < limit &&
         dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
    auto ref = Reference(array);
    if (ref)
      result.push_back(std::move(ref));
    if (!dbus_message_iter_next(&array))
      break;
  }
  return result;
}
void Basic(DBusMessageIter &iter, int type, const void *value) {
  dbus_message_iter_append_basic(&iter, type, value);
}
void Text(DBusMessageIter &iter, const char *value) {
  Basic(iter, DBUS_TYPE_STRING, &value);
}
void Int(DBusMessageIter &iter, int32_t value) {
  Basic(iter, DBUS_TYPE_INT32, &value);
}

// Own the transport, not libatspi's process-wide cache and GLib sources. GTK
// hosts may already use those on their UI thread. Moving their main context is
// insufficient: libatspi attaches newly discovered application buses to the
// default context while scans are running on another thread.
class Bus {
public:
  ~Bus() { Close(); }
  bool Open() {
    std::call_once(dbus_threads, [] { dbus_threads_init_default(); });
    std::string address;
    if (const char *value = std::getenv("AT_SPI_BUS_ADDRESS"); value && *value)
      address = value;
    else {
      // Do not autolaunch a session bus for an audio-only/headless process.
      if (!std::getenv("DBUS_SESSION_BUS_ADDRESS"))
        return false;
      DBusError error;
      dbus_error_init(&error);
      connection_ = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
      if (dbus_error_is_set(&error))
        dbus_error_free(&error);
      if (!connection_)
        return false;
      dbus_connection_set_exit_on_disconnect(connection_, FALSE);
      auto reply = Call({"org.a11y.Bus", "/org/a11y/bus"}, "org.a11y.Bus",
                        "GetAddress", {}, 1000);
      DBusMessageIter iter;
      if (reply && dbus_message_iter_init(reply.get(), &iter))
        address = String(iter);
      Close();
    }
    if (address.empty())
      return false;
    DBusError error;
    dbus_error_init(&error);
    connection_ = dbus_connection_open_private(address.c_str(), &error);
    if (dbus_error_is_set(&error))
      dbus_error_free(&error);
    if (!connection_)
      return false;
    dbus_connection_set_exit_on_disconnect(connection_, FALSE);
    dbus_connection_set_max_message_size(connection_, 4 * 1024 * 1024);
    return dbus_bus_register(connection_, nullptr);
  }
  Message Call(const Ref &ref, const char *interface, const char *method,
               const std::function<void(DBusMessageIter &)> &args = {},
               int timeout = 150) {
    if (!connection_ || !ref)
      return {nullptr, dbus_message_unref};
    Message message(dbus_message_new_method_call(
                        ref.bus.c_str(), ref.path.c_str(), interface, method),
                    dbus_message_unref);
    if (!message)
      return {nullptr, dbus_message_unref};
    if (args) {
      DBusMessageIter iter;
      dbus_message_iter_init_append(message.get(), &iter);
      args(iter);
    }
    DBusError error;
    dbus_error_init(&error);
    Message reply(dbus_connection_send_with_reply_and_block(
                      connection_, message.get(), timeout, &error),
                  dbus_message_unref);
    if (dbus_error_is_set(&error))
      dbus_error_free(&error);
    if (reply &&
        dbus_message_get_type(reply.get()) != DBUS_MESSAGE_TYPE_METHOD_RETURN)
      reply.reset();
    return reply;
  }
  uint32_t Pid(const Ref &ref) {
    auto reply = Call({DBUS_SERVICE_DBUS, DBUS_PATH_DBUS}, DBUS_INTERFACE_DBUS,
                      "GetConnectionUnixProcessID",
                      [&](auto &iter) { Text(iter, ref.bus.c_str()); });
    uint32_t pid = 0;
    if (reply)
      dbus_message_get_args(reply.get(), nullptr, DBUS_TYPE_UINT32, &pid,
                            DBUS_TYPE_INVALID);
    return pid;
  }
  std::string Property(const Ref &ref, const char *name) {
    auto reply = Call(ref, DBUS_INTERFACE_PROPERTIES, "Get", [&](auto &iter) {
      Text(iter, kAccessible);
      Text(iter, name);
    });
    DBusMessageIter iter, value;
    if (!reply || !dbus_message_iter_init(reply.get(), &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
      return {};
    dbus_message_iter_recurse(&iter, &value);
    return String(value);
  }
  Ref Parent(const Ref &ref) {
    auto reply = Call(ref, DBUS_INTERFACE_PROPERTIES, "Get", [&](auto &iter) {
      Text(iter, kAccessible);
      Text(iter, "Parent");
    });
    DBusMessageIter iter, value;
    if (!reply || !dbus_message_iter_init(reply.get(), &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
      return {};
    dbus_message_iter_recurse(&iter, &value);
    return Reference(value);
  }
  uint32_t Role(const Ref &ref) {
    auto reply = Call(ref, kAccessible, "GetRole");
    uint32_t role = ATSPI_ROLE_INVALID;
    if (reply)
      dbus_message_get_args(reply.get(), nullptr, DBUS_TYPE_UINT32, &role,
                            DBUS_TYPE_INVALID);
    return role;
  }
  std::vector<Ref> Children(const Ref &ref, size_t limit) {
    auto reply = Call(ref, kAccessible, "GetChildren");
    return References(reply.get(), limit);
  }
  std::string Attribute(const Ref &ref, const char *name) {
    auto reply = Call(ref, kDocument, "GetAttributeValue",
                      [&](auto &iter) { Text(iter, name); });
    DBusMessageIter iter;
    return reply && dbus_message_iter_init(reply.get(), &iter) ? String(iter)
                                                               : std::string{};
  }

private:
  void Close() {
    if (connection_) {
      dbus_connection_close(connection_);
      dbus_connection_unref(connection_);
      connection_ = nullptr;
    }
  }
  DBusConnection *connection_ = nullptr;
};
struct Budget {
  uint64_t deadline = NowNs() + 500000000;
  unsigned nodes = 0, documents = 0;
  bool More(unsigned depth) {
    return depth < 28 && nodes++ < 800 && NowNs() < deadline;
  }
};
int ControlRole(uint32_t role) {
  if (role == ATSPI_ROLE_PUSH_BUTTON || role == ATSPI_ROLE_TOGGLE_BUTTON)
    return MREC_CONTROL_BUTTON;
  if (role == ATSPI_ROLE_MENU_ITEM || role == ATSPI_ROLE_CHECK_MENU_ITEM)
    return MREC_CONTROL_MENU_ITEM;
  if (role == ATSPI_ROLE_STATUS_BAR)
    return MREC_CONTROL_STATUS;
  if (role == ATSPI_ROLE_LABEL || role == ATSPI_ROLE_STATIC ||
      role == ATSPI_ROLE_TEXT)
    return MREC_CONTROL_TEXT;
  return 0;
}
bool DocumentRole(uint32_t role) {
  return role == ATSPI_ROLE_DOCUMENT_WEB || role == ATSPI_ROLE_DOCUMENT_FRAME;
}
void Controls(Bus &bus, const Ref &node, int32_t platform, Budget &budget,
              unsigned depth, uint32_t &signals) {
  if (!budget.More(depth))
    return;
  const auto role = bus.Role(node);
  if (depth && DocumentRole(role))
    return;
  if (const auto mapped = ControlRole(role)) {
    const auto name = bus.Property(node, "Name"),
               id = bus.Property(node, "AccessibleId");
    signals |=
        mrec_catalog_call_control(platform, mapped, id.c_str(), name.c_str());
  }
  for (const auto &child : bus.Children(node, 100)) {
    if (NowNs() >= budget.deadline)
      break;
    Controls(bus, child, platform, budget, depth + 1, signals);
  }
}
void Documents(Bus &bus, const Ref &node, const std::string &title,
               Budget &budget, unsigned depth,
               std::vector<WindowEvidence> &out) {
  if (!budget.More(depth))
    return;
  if (DocumentRole(bus.Role(node))) {
    ++budget.documents;
    WindowEvidence evidence;
    evidence.title = bus.Property(node, "Name");
    if (evidence.title.empty())
      evidence.title = title;
    evidence.url = bus.Attribute(node, "URI");
    if (evidence.url.empty())
      evidence.url = bus.Attribute(node, "DocURL");
    evidence.platform = mrec_catalog_context_platform(evidence.url.c_str(),
                                                      evidence.title.c_str());
    if (evidence.platform) {
      uint32_t signals = 0;
      Controls(bus, node, evidence.platform, budget, 0, signals);
      evidence.call_state = mrec_catalog_call_state(signals);
      out.push_back(std::move(evidence));
    }
    return;
  }
  for (const auto &child : bus.Children(node, 100)) {
    if (NowNs() >= budget.deadline)
      break;
    Documents(bus, child, title, budget, depth + 1, out);
  }
}
bool CollectDocuments(Bus &bus, const Ref &window, const std::string &title,
                      Budget &budget, std::vector<WindowEvidence> &out) {
  auto reply = bus.Call(
      window, "org.a11y.atspi.Collection", "GetMatches", [](auto &iter) {
        DBusMessageIter rule, array;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_STRUCT, nullptr,
                                         &rule);
        dbus_message_iter_open_container(&rule, DBUS_TYPE_ARRAY, "i", &array);
        Int(array, 0);
        Int(array, 0);
        dbus_message_iter_close_container(&rule, &array);
        Int(rule, ATSPI_Collection_MATCH_ALL);
        dbus_message_iter_open_container(&rule, DBUS_TYPE_ARRAY, "{ss}",
                                         &array);
        dbus_message_iter_close_container(&rule, &array);
        Int(rule, ATSPI_Collection_MATCH_ALL);
        uint32_t roles[4]{};
        for (const auto role :
             {ATSPI_ROLE_DOCUMENT_WEB, ATSPI_ROLE_DOCUMENT_FRAME})
          roles[role / 32] |= uint32_t(1) << (role % 32);
        dbus_message_iter_open_container(&rule, DBUS_TYPE_ARRAY, "i", &array);
        for (auto role : roles)
          Int(array, int32_t(role));
        dbus_message_iter_close_container(&rule, &array);
        Int(rule, ATSPI_Collection_MATCH_ANY);
        dbus_message_iter_open_container(&rule, DBUS_TYPE_ARRAY, "s", &array);
        dbus_message_iter_close_container(&rule, &array);
        Int(rule, ATSPI_Collection_MATCH_ALL);
        dbus_bool_t invert = FALSE, traverse = TRUE;
        Basic(rule, DBUS_TYPE_BOOLEAN, &invert);
        dbus_message_iter_close_container(&iter, &rule);
        const dbus_uint32_t order = ATSPI_Collection_SORT_ORDER_CANONICAL;
        Basic(iter, DBUS_TYPE_UINT32, &order);
        Int(iter, 16);
        Basic(iter, DBUS_TYPE_BOOLEAN, &traverse);
      });
  if (!reply)
    return false;
  for (const auto &document : References(reply.get(), 16)) {
    bool top_level = true;
    auto parent = bus.Parent(document);
    for (unsigned depth = 0; parent && depth < 28 && NowNs() < budget.deadline;
         ++depth) {
      const auto role = bus.Role(parent);
      if (DocumentRole(role)) {
        top_level = false;
        break;
      }
      if (role == ATSPI_ROLE_FRAME || role == ATSPI_ROLE_APPLICATION)
        break;
      parent = bus.Parent(parent);
    }
    if (top_level && NowNs() < budget.deadline)
      Documents(bus, document, title, budget, 0, out);
  }
  return true;
}
} // namespace
bool AccessibilityAvailable() {
  std::lock_guard<std::mutex> lock(access_lock);
  Bus bus;
  return bus.Open();
}
std::map<uint32_t, AppEvidence> ReadAccessibility() {
  std::lock_guard<std::mutex> lock(access_lock);
  std::map<uint32_t, AppEvidence> result;
  Bus bus;
  if (!bus.Open())
    return result;
  const uint64_t deadline = NowNs() + 2000000000ull;
  for (const auto &app : bus.Children(
           {"org.a11y.atspi.Registry", "/org/a11y/atspi/accessible/root"},
           256)) {
    if (NowNs() >= deadline)
      break;
    const auto root = AppRoot(bus.Pid(app));
    const auto platform = mrec_catalog_linux_platform(root.name.c_str());
    const bool browser =
        mrec_catalog_linux_browser(root.name.c_str()) != nullptr;
    if (!root.pid || (!platform && !browser))
      continue;
    auto &entry = result[root.pid];
    entry.pid = root.pid;
    for (const auto &window : bus.Children(app, 16)) {
      if (NowNs() >= deadline)
        break;
      const auto title = bus.Property(window, "Name");
      Budget budget;
      budget.deadline = std::min(budget.deadline, deadline);
      if (browser) {
        const auto before = entry.windows.size();
        if (!CollectDocuments(bus, window, title, budget, entry.windows))
          Documents(bus, window, title, budget, 0, entry.windows);
        if (before == entry.windows.size() && budget.documents == 0) {
          const auto match = mrec_catalog_context_platform("", title.c_str());
          if (match)
            entry.windows.push_back({title, "", match, MREC_CALL_UNKNOWN});
        }
      } else {
        uint32_t signals = 0;
        Controls(bus, window, platform, budget, 0, signals);
        entry.windows.push_back(
            {title, "", platform, mrec_catalog_call_state(signals)});
      }
    }
  }
  return result;
}
} // namespace meeting_record::linux_backend
