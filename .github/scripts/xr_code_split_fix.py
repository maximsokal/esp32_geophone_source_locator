from pathlib import Path

app_path = Path("webui/src/App.tsx")
app = app_path.read_text(encoding="utf-8")
old = '<ResultViewportErrorBoundary key={scanSnapshot.pointCloudVersion}>'
new = '<ResultViewportErrorBoundary>'
if app.count(old) != 1:
    raise RuntimeError(f"expected exactly one App error-boundary key target, got {app.count(old)}")
app_path.write_text(app.replace(old, new, 1), encoding="utf-8")

prepare_path = Path("webui/scripts/prepare-esp-assets.mjs")
prepare = prepare_path.read_text(encoding="utf-8")
start = prepare.find("const generatedTable = ")
end_marker = "\n\nawait rm(espDir"
end = prepare.find(end_marker, start)
if start < 0 or end < 0:
    raise RuntimeError("could not locate generatedTable block in prepare-esp-assets.mjs")
replacement = """const generatedTable = [
  ...declarations,
  '',
  'static const web_webui_chunk_asset_t s_webui_chunk_assets[] = {',
  ...tableRows,
  '};',
  '#define WEB_WEBUI_CHUNK_ASSET_COUNT (sizeof(s_webui_chunk_assets) / sizeof(s_webui_chunk_assets[0]))',
  '',
].join(String.fromCharCode(10));"""
prepare_path.write_text(prepare[:start] + replacement + prepare[end:], encoding="utf-8")

print("XR code-splitting verification fixes applied")
