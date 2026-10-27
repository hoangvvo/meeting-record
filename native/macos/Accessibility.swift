import ApplicationServices
import AppKit
import Foundation

/// Optional titles and URLs from accessibility. Bound IPC calls and tree traversal.
enum Accessibility {
    /// Seconds before an AX request to another process gives up.
    private static let messagingTimeout: Float = 0.25
    /// Bound the whole tree walk as well as each individual AX request.
    private static let traversalTimeout: TimeInterval = 0.5

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
                                          _ attribute: String, limit: Int) -> [AXUIElement] {
        guard limit > 0 else { return [] }
        var values: CFArray?
        guard AXUIElementCopyAttributeValues(element, attribute as CFString,
                                            0, limit, &values) == .success else { return [] }
        return values as? [AXUIElement] ?? []
    }

    private static func elementAttribute(_ element: AXUIElement, _ attribute: String) -> AXUIElement? {
        guard let value = copyAttribute(element, attribute),
              CFGetTypeID(value) == AXUIElementGetTypeID() else { return nil }
        return unsafeBitCast(value, to: AXUIElement.self)
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

        if let window = elementAttribute(app, kAXFocusedWindowAttribute as String) {
            AXUIElementSetMessagingTimeout(window, messagingTimeout)
            if let title = stringAttribute(window, kAXTitleAttribute as String) {
                return title
            }
        }

        for window in elementsAttribute(app, kAXWindowsAttribute as String, limit: 4) {
            AXUIElementSetMessagingTimeout(window, messagingTimeout)
            if let title = stringAttribute(window, kAXTitleAttribute as String) {
                return title
            }
        }
        return nil
    }

    static func browserWindows(pid: pid_t) -> [BrowserWindowContent] {
        windowContents(pid: pid, nativePlatform: nil)
    }

    static func nativeWindows(pid: pid_t, platform: MeetingPlatform) -> [BrowserWindowContent] {
        windowContents(pid: pid, nativePlatform: platform)
    }

    private static func windowContents(pid: pid_t, nativePlatform: MeetingPlatform?) -> [BrowserWindowContent] {
        guard isTrusted() else { return [] }
        let app = application(pid)
        enableEnhancedInterface(app)
        let deadline = ProcessInfo.processInfo.systemUptime + traversalTimeout
        // Native Zoom exposes in-meeting actions in its app menu. Inspect it
        // before a large participant tree can consume the traversal budget.
        var menuContent: BrowserWindowContent?
        if nativePlatform == .zoom, let menu = elementAttribute(app, kAXMenuBarAttribute as String) {
            menuContent = BrowserContentReader.read(
                Element(menu), deadline: deadline, now: { ProcessInfo.processInfo.systemUptime },
                nativePlatform: nativePlatform)
        }
        guard ProcessInfo.processInfo.systemUptime < deadline else {
            return menuContent.map { [$0] } ?? []
        }
        var windows = elementsAttribute(app, kAXWindowsAttribute as String, limit: 4)
        if ProcessInfo.processInfo.systemUptime < deadline,
           let window = elementAttribute(app, kAXFocusedWindowAttribute as String) {
            windows.removeAll { CFEqual($0, window) }
            windows.insert(window, at: 0)
        }
        var contents = windows.prefix(4).compactMap { window -> BrowserWindowContent? in
            guard ProcessInfo.processInfo.systemUptime < deadline else { return nil }
            return BrowserContentReader.read(Element(window), deadline: deadline,
                                             now: { ProcessInfo.processInfo.systemUptime },
                                             nativePlatform: nativePlatform)
        }
        if let menu = menuContent { contents.append(menu) }
        return contents
    }

    private struct Element: BrowserAccessibilityElement {
        let element: AXUIElement

        init(_ element: AXUIElement) {
            self.element = element
            AXUIElementSetMessagingTimeout(element, messagingTimeout)
        }

        var role: String { stringAttribute(element, kAXRoleAttribute as String) ?? "" }
        var title: String? {
            stringAttribute(element, kAXTitleAttribute as String)
                ?? stringAttribute(element, kAXDescriptionAttribute as String)
        }
        var url: String? {
            let raw = copyAttribute(element, "AXURL")
            if let url = raw as? URL { return url.absoluteString }
            return raw as? String
        }
        var value: String? { stringAttribute(element, kAXValueAttribute as String) }
        var identifier: String? {
            stringAttribute(element, "AXDOMIdentifier") ?? stringAttribute(element, "AXIdentifier")
        }
        var isHidden: Bool { (copyAttribute(element, "AXHidden") as? Bool) == true }
        var subrole: String? { stringAttribute(element, kAXSubroleAttribute as String) }

        func children(limit: Int) -> [Element] {
            elementsAttribute(element, kAXChildrenAttribute as String, limit: limit).map(Element.init)
        }
    }
}
