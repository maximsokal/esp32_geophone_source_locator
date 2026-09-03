from pathlib import Path

path = Path("webui/src/App.tsx")
content = path.read_text(encoding="utf-8")
old = '<ResultViewportErrorBoundary key={scanSnapshot.pointCloudVersion}>'
new = '<ResultViewportErrorBoundary>'
if content.count(old) != 1:
    raise RuntimeError(f"expected exactly one App error-boundary key target, got {content.count(old)}")
path.write_text(content.replace(old, new, 1), encoding="utf-8")
print("XR code-splitting typecheck fix applied")
