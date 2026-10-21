import Foundation

struct BrowserWindowContent {
    var title = ""
    var url = ""
    var tabTitles: [String] = []
}

protocol BrowserAccessibilityElement {
    var role: String { get }
    var title: String? { get }
    var url: String? { get }
    var value: String? { get }
    func children(limit: Int) -> [Self]
}

enum BrowserContentReader {
    static func read<Element: BrowserAccessibilityElement>(
        _ root: Element, deadline: TimeInterval,
        now: () -> TimeInterval
    ) -> BrowserWindowContent {
        var content = BrowserWindowContent()
        var remaining = 400
        func visit(_ element: Element, depth: Int) {
            guard remaining > 0, depth <= 12, now() < deadline else { return }
            remaining -= 1
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
            // Read the document's URL, never its DOM or page controls.
            if role == "AXWebArea" || role == "AXDocument" { return }
            guard now() < deadline else { return }
            for child in element.children(limit: remaining) {
                visit(child, depth: depth + 1)
                if remaining == 0 || now() >= deadline { break }
            }
        }
        visit(root, depth: 0)
        return content
    }
}
