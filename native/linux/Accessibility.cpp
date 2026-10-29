#include "Accessibility.h"
#include "Process.h"
#include "meeting-catalog.h"
#include <atspi/atspi.h>
#include <mutex>

namespace meeting_record::linux_backend {
namespace {
// libatspi owns process-wide caches; serialize scans and permission probes.
std::mutex access_lock;
bool initialized = false;
GMainContext *access_context = nullptr;
bool Init() {
  // atspi_set_main_context dereferences the bus even when no accessibility
  // service is installed. Audio-only/headless sessions must remain usable.
  if (!atspi_get_a11y_bus())
    return false;
  if (!initialized) {
    // Never dispatch the host GUI's default context on a scan worker. AT-SPI
    // events belong to a private context, serialized with all cache access.
    if (!access_context) {
      access_context = g_main_context_new();
      atspi_set_main_context(access_context);
    }
    const auto status = atspi_init();
    if (status != 0 && status != 1)
      return false;
    atspi_set_timeout(150, 150);
    initialized = true;
  }
  return atspi_get_a11y_bus() != nullptr;
}
std::string Take(gchar *value) {
  std::string result = value ? value : "";
  g_free(value);
  return result;
}
struct Budget {
  uint64_t deadline = NowNs() + 500000000;
  unsigned nodes = 0;
  unsigned documents = 0;
  bool More(unsigned depth) {
    return depth < 28 && nodes++ < 800 && NowNs() < deadline;
  }
};
int ControlRole(AtspiRole role) {
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
void Controls(AtspiAccessible *node, int32_t platform, Budget &budget,
              unsigned depth, uint32_t &signals) {
  if (!node || !budget.More(depth))
    return;
  const auto role = atspi_accessible_get_role(node, nullptr);
  // Never combine controls from nested/embedded documents with their host page.
  if (depth &&
      (role == ATSPI_ROLE_DOCUMENT_WEB || role == ATSPI_ROLE_DOCUMENT_FRAME))
    return;
  const auto mapped = ControlRole(role);
  if (mapped) {
    const auto name = Take(atspi_accessible_get_name(node, nullptr));
    const auto id = Take(atspi_accessible_get_accessible_id(node, nullptr));
    signals |=
        mrec_catalog_call_control(platform, mapped, id.c_str(), name.c_str());
  }
  const auto children =
      std::min(atspi_accessible_get_child_count(node, nullptr), 100);
  for (int i = 0; i < children && NowNs() < budget.deadline; ++i) {
    auto child = atspi_accessible_get_child_at_index(node, i, nullptr);
    if (!child)
      continue;
    Controls(child, platform, budget, depth + 1, signals);
    g_object_unref(child);
  }
}
void Documents(AtspiAccessible *node, const std::string &title, Budget &budget,
               unsigned depth, std::vector<WindowEvidence> &out) {
  if (!node || !budget.More(depth))
    return;
  const auto role = atspi_accessible_get_role(node, nullptr);
  if (role == ATSPI_ROLE_DOCUMENT_WEB || role == ATSPI_ROLE_DOCUMENT_FRAME) {
    budget.documents++;
    WindowEvidence evidence;
    evidence.title = Take(atspi_accessible_get_name(node, nullptr));
    if (evidence.title.empty())
      evidence.title = title;
    if (auto document = atspi_accessible_get_document_iface(node)) {
      // Chromium exposes URI via AtkDocument (verified in Chromium's
      // implementation). Older AT-SPI headers take gchar* here; newer headers
      // take const gchar*.
      char uri_attribute[] = "URI", firefox_attribute[] = "DocURL";
      evidence.url = Take(atspi_document_get_document_attribute_value(
          document, uri_attribute, nullptr));
      if (evidence.url.empty())
        evidence.url = Take(atspi_document_get_document_attribute_value(
            document, firefox_attribute, nullptr));
      g_object_unref(document);
    }
    evidence.platform = mrec_catalog_context_platform(evidence.url.c_str(),
                                                      evidence.title.c_str());
    if (evidence.platform != MREC_PLATFORM_UNKNOWN) {
      uint32_t signals = 0;
      Controls(node, evidence.platform, budget, 0, signals);
      evidence.call_state = mrec_catalog_call_state(signals);
      out.push_back(std::move(evidence));
    }
    return;
  }
  const auto children =
      std::min(atspi_accessible_get_child_count(node, nullptr), 100);
  for (int i = 0; i < children && NowNs() < budget.deadline; ++i) {
    auto child = atspi_accessible_get_child_at_index(node, i, nullptr);
    if (!child)
      continue;
    Documents(child, title, budget, depth + 1, out);
    g_object_unref(child);
  }
}

bool CollectDocuments(AtspiAccessible *window, const std::string &title,
                      Budget &budget, std::vector<WindowEvidence> &out) {
  auto collection = atspi_accessible_get_collection_iface(window);
  if (!collection)
    return false;
  auto roles = g_array_new(FALSE, FALSE, sizeof(AtspiRole));
  AtspiRole role = ATSPI_ROLE_DOCUMENT_WEB;
  g_array_append_val(roles, role);
  role = ATSPI_ROLE_DOCUMENT_FRAME;
  g_array_append_val(roles, role);
  auto rule = atspi_match_rule_new(nullptr, ATSPI_Collection_MATCH_ALL, nullptr,
                                   ATSPI_Collection_MATCH_ALL, roles,
                                   ATSPI_Collection_MATCH_ANY, nullptr,
                                   ATSPI_Collection_MATCH_ALL, FALSE);
  g_array_unref(roles);
  GError *error = nullptr;
  auto matches = atspi_collection_get_matches(
      collection, rule, ATSPI_Collection_SORT_ORDER_CANONICAL, 16, TRUE,
      &error);
  if (std::getenv("MREC_DEBUG_ACCESSIBILITY"))
    std::fprintf(stderr, "collection matches=%d error=%s remaining_ms=%lld\n",
                 matches ? int(matches->len) : -1,
                 error ? error->message : "none",
                 (long long)(budget.deadline - NowNs()) / 1000000);
  g_object_unref(rule);
  g_object_unref(collection);
  const bool supported = matches && !error;
  if (error)
    g_error_free(error);
  if (matches) {
    for (guint i = 0; i < matches->len; ++i) {
      auto document = g_array_index(matches, AtspiAccessible *, i);
      bool top_level = true;
      auto parent = atspi_accessible_get_parent(document, nullptr);
      for (unsigned depth = 0; parent && depth < 28; ++depth) {
        const auto parent_role = atspi_accessible_get_role(parent, nullptr);
        if (parent_role == ATSPI_ROLE_DOCUMENT_WEB ||
            parent_role == ATSPI_ROLE_DOCUMENT_FRAME) {
          top_level = false;
          g_object_unref(parent);
          parent = nullptr;
          break;
        }
        if (parent_role == ATSPI_ROLE_FRAME ||
            parent_role == ATSPI_ROLE_APPLICATION) {
          g_object_unref(parent);
          parent = nullptr;
          break;
        }
        auto next = atspi_accessible_get_parent(parent, nullptr);
        g_object_unref(parent);
        parent = next;
      }
      if (parent)
        g_object_unref(parent);
      if (supported && top_level && NowNs() < budget.deadline)
        Documents(document, title, budget, 0, out);
      g_object_unref(document);
    }
    g_array_free(matches, TRUE);
  }
  return supported;
}
} // namespace
bool AccessibilityAvailable() {
  std::lock_guard<std::mutex> lock(access_lock);
  return Init();
}
std::map<uint32_t, AppEvidence> ReadAccessibility() {
  std::lock_guard<std::mutex> lock(access_lock);
  std::map<uint32_t, AppEvidence> result;
  if (!Init())
    return result;
  const auto dispatch_deadline = NowNs() + 5000000;
  for (unsigned events = 0; events < 100 && NowNs() < dispatch_deadline &&
                            g_main_context_pending(access_context);
       ++events)
    g_main_context_iteration(access_context, FALSE);
  auto desktop = atspi_get_desktop(0);
  if (!desktop)
    return result;
  const uint64_t deadline = NowNs() + 2000000000ull;
  const auto apps =
      std::min(atspi_accessible_get_child_count(desktop, nullptr), 256);
  for (int i = 0; i < apps && NowNs() < deadline; ++i) {
    auto app = atspi_accessible_get_child_at_index(desktop, i, nullptr);
    if (!app)
      continue;
    const auto pid = atspi_accessible_get_process_id(app, nullptr);
    const auto root = AppRoot(pid);
    const auto platform = mrec_catalog_linux_platform(root.name.c_str());
    const bool browser =
        mrec_catalog_linux_browser(root.name.c_str()) != nullptr;
    if (root.pid && (platform || browser)) {
      auto &entry = result[root.pid];
      entry.pid = root.pid;
      const auto windows =
          std::min(atspi_accessible_get_child_count(app, nullptr), 16);
      for (int w = 0; w < windows && NowNs() < deadline; ++w) {
        auto window = atspi_accessible_get_child_at_index(app, w, nullptr);
        if (!window)
          continue;
        const auto title = Take(atspi_accessible_get_name(window, nullptr));
        Budget budget;
        budget.deadline = std::min(budget.deadline, deadline);
        if (browser) {
          const auto before = entry.windows.size();
          if (!CollectDocuments(window, title, budget, entry.windows))
            Documents(window, title, budget, 0, entry.windows);
          if (std::getenv("MREC_DEBUG_ACCESSIBILITY"))
            std::fprintf(stderr, "window nodes=%u documents=%zu title=%s\n",
                         budget.nodes, entry.windows.size(), title.c_str());
          if (before == entry.windows.size() && budget.documents == 0) {
            const auto match = mrec_catalog_context_platform("", title.c_str());
            if (match)
              entry.windows.push_back({title, "", match, MREC_CALL_UNKNOWN});
          }
        } else {
          uint32_t signals = 0;
          Controls(window, platform, budget, 0, signals);
          entry.windows.push_back(
              {title, "", platform, mrec_catalog_call_state(signals)});
        }
        g_object_unref(window);
      }
    }
    g_object_unref(app);
  }
  g_object_unref(desktop);
  return result;
}
} // namespace meeting_record::linux_backend
