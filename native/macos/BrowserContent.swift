import Foundation

struct BrowserWindowContent {
    var title = ""
    var url = ""
    var tabTitles: [String] = []
    var callPlatform: MeetingPlatform?
    var callSignals: UInt32 = 0
}

protocol BrowserAccessibilityElement {
    var role: String { get }
    var title: String? { get }
    var url: String? { get }
    var value: String? { get }
    var identifier: String? { get }
    var subrole: String? { get }
    var isHidden: Bool { get }
    func children(limit: Int) -> [Self]
}

extension BrowserAccessibilityElement {
    var identifier: String? { nil }
    var subrole: String? { nil }
    var isHidden: Bool { false }
}

enum BrowserContentReader {
    static func read<Element: BrowserAccessibilityElement>(
        _ root: Element, deadline: TimeInterval,
        now: () -> TimeInterval, nativePlatform: MeetingPlatform? = nil
    ) -> BrowserWindowContent {
        var content = BrowserWindowContent()
        if let platform = nativePlatform {
            guard now() < deadline else { return content }
            content.title = root.title ?? ""
            content.callPlatform = platform
            var budget = 1200
            content.callSignals = CallControlReader.read(
                root, platform: platform, deadline: deadline, now: now, remaining: &budget)
            return content
        }
        var remaining = 400
        var documents: [Element] = []
        func visit(_ element: Element, depth: Int) {
            guard remaining > 0, depth <= 12, now() < deadline else { return }
            remaining -= 1
            guard !element.isHidden, now() < deadline else { return }
            let role = element.role
            guard now() < deadline else { return }
            if depth == 0 { content.title = element.title ?? "" }
            guard now() < deadline else { return }
            if role == "AXRadioButton" || role == "AXTab" {
                if let title = element.title { content.tabTitles.append(title) }
                return
            }
            if content.url.isEmpty, let url = element.url { content.url = url }
            guard now() < deadline else { return }
            if role == "AXTextField", content.url.isEmpty,
               let value = element.value, value.contains(".") || value.hasPrefix("http") {
                content.url = value
            }
            // Defer document inspection until its active service is known.
            if role == "AXWebArea" || role == "AXDocument" {
                documents.append(element)
                return
            }
            guard now() < deadline else { return }
            for child in element.children(limit: remaining) {
                visit(child, depth: depth + 1)
                if remaining == 0 || now() >= deadline { break }
            }
        }
        visit(root, depth: 0)
        guard now() < deadline else { return content }
        let context = MeetingCatalog.contextPlatform(url: content.url, title: content.title)
        guard let platform = context else { return content }
        content.callPlatform = platform
        // Browser PIP controls can live outside a web area.
        let roots = documents.isEmpty ? [root] : documents
        var callBudget = 1200
        for document in roots {
            guard now() < deadline, callBudget > 0 else { break }
            if let url = document.url,
               MeetingCatalog.contextPlatform(url: url, title: "") != platform { continue }
            content.callSignals |= CallControlReader.read(
                document, platform: platform, deadline: deadline, now: now, remaining: &callBudget,
                scopeDocuments: true)
        }
        return content
    }
}

/// Inspect only a known service's controls, without reading editable values.
enum CallControlReader {
    static func read<Element: BrowserAccessibilityElement>(
        _ root: Element, platform: MeetingPlatform, deadline: TimeInterval,
        now: () -> TimeInterval, remaining: inout Int, scopeDocuments: Bool = false
    ) -> UInt32 {
        var signals: UInt32 = 0
        func visit(_ element: Element, depth: Int, statusContext: Bool, budget: inout Int) {
            guard budget > 0, depth <= 24, now() < deadline else { return }
            budget -= 1
            guard !element.isHidden, now() < deadline else { return }
            let role = element.role
            guard now() < deadline else { return }
            if role == "AXTextField" || role == "AXTextArea" { return }
            if scopeDocuments && (role == "AXWebArea" || role == "AXDocument"), let url = element.url,
               MeetingCatalog.contextPlatform(url: url, title: "") != platform { return }
            guard now() < deadline else { return }
            let subrole = role == "AXGroup" ? element.subrole : nil
            let status = statusContext || role == "AXHeading" || role == "AXStatusBar"
                || subrole == "AXApplicationAlert" || subrole == "AXApplicationStatus"
            let action = ["AXButton", "AXMenuButton", "AXPopUpButton", "AXMenuItem"].contains(role)
            let voiceStatus = platform == .discord && ["AXStaticText", "AXLink"].contains(role)
            let statusLabel = status && (role == "AXStaticText" || role == "AXHeading" || role == "AXStatusBar")
            if action || voiceStatus || statusLabel {
                let identifier = element.identifier
                let controlRole = statusLabel ? "AXHeading" : role
                signals |= MeetingCatalog.callControl(platform: platform, role: controlRole,
                                                       identifier: identifier, label: nil)
                guard now() < deadline else { return }
                let label = element.title
                signals |= MeetingCatalog.callControl(platform: platform, role: controlRole,
                                                       identifier: nil, label: label)
            }
            guard now() < deadline else { return }
            // Toolbars commonly follow large rosters/documents in the tree.
            for child in element.children(limit: budget).reversed() {
                visit(child, depth: depth + 1, statusContext: status, budget: &budget)
                if budget == 0 || now() >= deadline { break }
            }
        }
        visit(root, depth: 0, statusContext: false, budget: &remaining)
        return signals
    }
}
