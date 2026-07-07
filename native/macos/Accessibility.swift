import ApplicationServices
import AppKit
import Foundation

/// Accessibility (AX) helpers for reading window titles and browser URLs.
///
/// Everything here is best-effort enrichment. Detection must stay correct when
/// this whole file returns nil, because the user may never grant Accessibility.
///
/// Three hazards:
///
///  * **AX calls are synchronous IPC into the target app.** A busy or hung app
///    blocks *us*. Every element gets an explicit messaging timeout.
///  * **Chromium and Electron do not expose a tree until asked.** Setting
///    `AXEnhancedUserInterface` on the application element makes them build one.
///  * **Trees are deep and wide.** A full walk of a Chrome window is thousands of
///    elements, so traversal is bounded by depth and node count.
enum Accessibility {
    /// Seconds before an AX request to another process gives up.
    private static let messagingTimeout: Float = 0.25

    /// Cheap, honest preflight — unlike audio capture, this one exists.
    static func isTrusted() -> Bool {
        AXIsProcessTrusted()
    }

    /// Open the Accessibility pane. There is no prompt that can grant this
    /// without a visit to System Settings, and the app must be relaunched
    /// afterwards before `AXIsProcessTrusted()` flips.
    static func openSettings() {
        let url = URL(string:
            "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")!
        NSWorkspace.shared.open(url)
    }

    // MARK: - Element access

    private static func application(_ pid: pid_t) -> AXUIElement {
        let element = AXUIElementCreateApplication(pid)
        AXUIElementSetMessagingTimeout(element, messagingTimeout)
        return element
    }

    private static func copyAttribute(_ element: AXUIElement,
                                      _ attribute: String) -> CFTypeRef? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, attribute as CFString,
                                            &value) == .success else { return nil }
        return value
    }

    private static func stringAttribute(_ element: AXUIElement,
                                        _ attribute: String) -> String? {
        guard let value = copyAttribute(element, attribute) as? String,
              !value.isEmpty else { return nil }
        return value
    }

    private static func elementsAttribute(_ element: AXUIElement,
                                          _ attribute: String) -> [AXUIElement] {
        copyAttribute(element, attribute) as? [AXUIElement] ?? []
    }

    /// Chromium/Electron build an accessibility tree only once something asks for
    /// one. Without this, `AXWebArea` simply does not exist in their trees.
    private static func enableEnhancedInterface(_ app: AXUIElement) {
        AXUIElementSetAttributeValue(app, "AXEnhancedUserInterface" as CFString,
                                     kCFBooleanTrue)
    }

    // MARK: - Titles

    /// Title of the app's focused window, falling back to its first window.
    static func focusedWindowTitle(pid: pid_t) -> String? {
        guard isTrusted() else { return nil }
        let app = application(pid)

        if let focused = copyAttribute(app, kAXFocusedWindowAttribute as String) {
            // CFTypeRef -> AXUIElement is safe here: the attribute is documented
            // to return an element, and a mismatch just yields no title.
            let window = unsafeBitCast(focused, to: AXUIElement.self)
            AXUIElementSetMessagingTimeout(window, messagingTimeout)
            if let title = stringAttribute(window, kAXTitleAttribute as String) {
                return title
            }
        }

        for window in elementsAttribute(app, kAXWindowsAttribute as String).prefix(4) {
            AXUIElementSetMessagingTimeout(window, messagingTimeout)
            if let title = stringAttribute(window, kAXTitleAttribute as String) {
                return title
            }
        }
        return nil
    }

    // MARK: - Browser URLs

    /// Best-effort URL of the browser's active tab.
    ///
    /// Two strategies, because no single one covers every browser:
    ///
    ///  * Safari and Chrome expose `AXURL` on the window or a descendant, which is
    ///    exact.
    ///  * Chromium variants otherwise expose the URL as the value of a text field
    ///    in the toolbar (the omnibox), which is what the user typed rather than
    ///    the canonical URL — good enough to match a meeting host.
    static func browserURL(pid: pid_t) -> String? {
        guard isTrusted() else { return nil }
        let app = application(pid)
        enableEnhancedInterface(app)

        let windows = elementsAttribute(app, kAXWindowsAttribute as String)
        for window in windows.prefix(3) {
            AXUIElementSetMessagingTimeout(window, messagingTimeout)

            if let url = urlAttribute(window) { return url }

            var budget = 400
            if let found = findURL(in: window, depth: 0, maxDepth: 8, budget: &budget) {
                return found
            }
        }
        return nil
    }

    /// `AXURL` comes back as an NSURL, not a string.
    private static func urlAttribute(_ element: AXUIElement) -> String? {
        guard let raw = copyAttribute(element, "AXURL") else { return nil }
        if let url = raw as? URL { return url.absoluteString }
        if let string = raw as? String, !string.isEmpty { return string }
        return nil
    }

    /// Bounded search for a URL: an `AXURL` anywhere, or a web area's title, or
    /// the omnibox text field's value.
    private static func findURL(in element: AXUIElement,
                                depth: Int,
                                maxDepth: Int,
                                budget: inout Int) -> String? {
        guard depth <= maxDepth, budget > 0 else { return nil }
        budget -= 1

        if let url = urlAttribute(element) { return url }

        let role = stringAttribute(element, kAXRoleAttribute as String) ?? ""

        // The omnibox: a text field whose value looks like a URL or host.
        if role == kAXTextFieldRole as String,
           let value = stringAttribute(element, kAXValueAttribute as String),
           value.contains(".") || value.hasPrefix("http") {
            return value
        }

        // Descending into a web area's DOM is enormous and never yields a URL that
        // the window itself would not already expose.
        if role == "AXWebArea" {
            return stringAttribute(element, kAXTitleAttribute as String)
        }

        for child in elementsAttribute(element, kAXChildrenAttribute as String) {
            AXUIElementSetMessagingTimeout(child, messagingTimeout)
            if let found = findURL(in: child, depth: depth + 1,
                                   maxDepth: maxDepth, budget: &budget) {
                return found
            }
        }
        return nil
    }
}
