"""Print the fixture browser's actual AT-SPI tree for Linux E2E diagnostics."""
import pyatspi


def walk(node, depth=0):
    if depth > 18:
        return
    print(" " * depth, node.getRoleName(), repr(node.name), flush=True)
    if node.getRoleName() in ("document web", "document frame"):
        try:
            print("document attributes", node.queryDocument().getAttributes(), flush=True)
        except Exception as error:
            print(error, flush=True)
    for child in list(node)[:100]:
        walk(child, depth + 1)


desktop = pyatspi.Registry.getDesktop(0)
for app in desktop:
    print("APP", app.name, app.get_process_id(), flush=True)
    if "Chrome" in app.name or "Chromium" in app.name:
        walk(app)
