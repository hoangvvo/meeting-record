import ApplicationServices
import AppKit
import Foundation

/// Accessibility helpers for reading window titles and browser URLs.
///
/// Best-effort enrichment: detection stays correct when every function here
/// returns nil.
///
/// Three constraints:
///  * AX calls are synchronous IPC into the target app, so a hung app blocks the
///    caller. Every element gets a messaging timeout.
///  * Chromium and Electron build no tree until `AXEnhancedUserInterface` is set.
///  * Trees run to thousands of elements, so traversal is bounded by depth and
///    node count.
enum Accessibility {
    /// Seconds before an AX request to another process gives up.
    private static let messagingTimeout: Float = 0.25

    /// Preflight check.
    static func isTrusted() -> Bool {
        AXIsProcessTrusted()
    }

    /// Opens the Accessibility pane. There is no in-app prompt, and the app must be
    /// relaunched before `AXIsProcessTrusted()` changes.
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

    /// Without this, `AXWebArea` does not exist in Chromium or Electron trees.
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
            // The attribute returns an element; a mismatch yields no title.
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

    /// URL of the browser's active tab.
    ///
    /// Safari and Chrome expose `AXURL` on the window or a descendant. Other
    /// Chromium variants only expose the omnibox text field's value, which is what
    /// the user typed rather than the canonical URL.
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

    /// `AXURL` returns an NSURL, not a string.
    private static func urlAttribute(_ element: AXUIElement) -> String? {
        guard let raw = copyAttribute(element, "AXURL") else { return nil }
        if let url = raw as? URL { return url.absoluteString }
        if let string = raw as? String, !string.isEmpty { return string }
        return nil
    }

    /// Bounded search for an `AXURL`, a web area's title, or the omnibox value.
    private static func findURL(in element: AXUIElement,
                                depth: Int,
                                maxDepth: Int,
                                budget: inout Int) -> String? {
        guard depth <= maxDepth, budget > 0 else { return nil }
        budget -= 1

        if let url = urlAttribute(element) { return url }

        let role = stringAttribute(element, kAXRoleAttribute as String) ?? ""

        // The omnibox is a text field whose value looks like a URL or host.
        if role == kAXTextFieldRole as String,
           let value = stringAttribute(element, kAXValueAttribute as String),
           value.contains(".") || value.hasPrefix("http") {
            return value
        }

        // A web area's DOM is large and exposes no URL the window does not.
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
