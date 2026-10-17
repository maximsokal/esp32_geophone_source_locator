#!/usr/bin/env python3
"""Generate correctly sized AprilTags and bind them to the five sensor nodes."""

from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import sys
import threading
import webbrowser

try:
    from . import model
except ImportError:  # Direct execution places this directory on sys.path.
    import model  # type: ignore[no-redef]


PT_PER_MM = 72.0 / 25.4
PAGE_W_MM = 210.0
PAGE_H_MM = 297.0
MAX_BODY_BYTES = 16 * 1024
DEFAULT_ROOT = Path(__file__).resolve().parents[2]


def _number(value: float) -> str:
    return f"{value:.6f}".rstrip("0").rstrip(".") or "0"


def _pdf_text(text: str) -> str:
    if not text.isascii():
        raise ValueError("PDF labels must contain ASCII text")
    return text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def _draw_text(commands: list[str], x_mm: float, top_mm: float, text: str, size_pt: float = 9.0) -> None:
    x_pt = x_mm * PT_PER_MM
    y_pt = (PAGE_H_MM - top_mm) * PT_PER_MM
    commands.append(f"BT /F1 {_number(size_pt)} Tf {_number(x_pt)} {_number(y_pt)} Td ({_pdf_text(text)}) Tj ET")


def _draw_line(commands: list[str], x1_mm: float, top1_mm: float, x2_mm: float, top2_mm: float, width_pt: float = 0.35) -> None:
    x1, y1 = x1_mm * PT_PER_MM, (PAGE_H_MM - top1_mm) * PT_PER_MM
    x2, y2 = x2_mm * PT_PER_MM, (PAGE_H_MM - top2_mm) * PT_PER_MM
    commands.append(f"{_number(width_pt)} w {_number(x1)} {_number(y1)} m {_number(x2)} {_number(y2)} l S")


def _module_rect(commands: list[str], x_mm: float, top_mm: float, size_mm: float) -> None:
    x = x_mm * PT_PER_MM
    y = (PAGE_H_MM - top_mm - size_mm) * PT_PER_MM
    size = size_mm * PT_PER_MM
    commands.append(f"{_number(x)} {_number(y)} {_number(size)} {_number(size)} re f")


def _tag_layout(bindings: dict, patterns: dict) -> list[list[tuple[dict, dict, float, float, float, float]]]:
    by_id = {entry["tag_id"]: entry for entry in patterns["tags"]}
    items = [
        (binding, by_id[binding["tag_id"]])
        for binding in bindings["tags"]
        for _ in range(bindings["sheet"]["copies_per_tag"])
    ]
    pages: list[list[tuple[dict, dict, float, float, float, float]]] = [[]]
    x, top, row_height = 5.0, 8.0, 0.0
    for binding, pattern in items:
        size = float(binding["size_mm"])
        quiet = size / 8.0
        footprint = size + 2 * (quiet + 3.0)
        item_height = footprint + 9.0
        if x + footprint > PAGE_W_MM - 5.0:
            x = 5.0
            top += row_height + 7.0
            row_height = 0.0
        if top + item_height > PAGE_H_MM - 33.0:
            pages.append([])
            x, top, row_height = 5.0, 8.0, 0.0
        pages[-1].append((binding, pattern, x, top, footprint, item_height))
        x += footprint + 6.0
        row_height = max(row_height, item_height)
    return pages


def render_pdf(root: Path) -> bytes:
    bindings = model.load_bindings(root)
    patterns = model.pattern_document(root)
    pages = _tag_layout(bindings, patterns)
    page_contents: list[bytes] = []
    for page_items in pages:
        commands = ["q", "0 0 0 rg", "0 0 0 RG"]
        for binding, pattern, frame_x, frame_top, _footprint, _item_height in page_items:
            size = float(binding["size_mm"])
            quiet = size / 8.0
            tag_x = frame_x + 3.0 + quiet
            tag_top = frame_top + 3.0 + quiet
            module = size / 8.0
            # Keep one uncompressed re/f operation per black module for exact geometry checks.
            for row, values in enumerate(pattern["modules"]):
                for column, black in enumerate(values):
                    if black:
                        _module_rect(commands, tag_x + column * module, tag_top + row * module, module)

            # Crop marks sit outside the one-module white quiet zone.
            edge_left, edge_right = tag_x - quiet - 1.0, tag_x + size + quiet + 1.0
            edge_top, edge_bottom = tag_top - quiet - 1.0, tag_top + size + quiet + 1.0
            length = 2.0
            for left in (True, False):
                cx = edge_left if left else edge_right
                for top_edge in (True, False):
                    cy = edge_top if top_edge else edge_bottom
                    x_direction = -1.0 if left else 1.0
                    y_direction = -1.0 if top_edge else 1.0
                    _draw_line(commands, cx, cy + y_direction * length, cx, cy, 0.35)
                    _draw_line(commands, cx + x_direction * length, cy, cx, cy, 0.35)
            caption = f"{binding['node']} | tag36h11 id {binding['tag_id']} | {size:.1f} mm"
            _draw_text(commands, frame_x + 2.0, frame_top + size + 2 * quiet + 8.0, caption, 8.0)
        _draw_line(commands, 5.0, 277.0, 55.0, 277.0, 0.7)
        _draw_line(commands, 5.0, 275.5, 5.0, 278.5, 0.7)
        _draw_line(commands, 55.0, 275.5, 55.0, 278.5, 0.7)
        _draw_text(commands, 5.0, 272.0, "Calibration bar: 50 mm. Measure after printing.", 8.0)
        _draw_text(commands, 5.0, 284.0, "White quiet zone >= tag edge / 8.", 8.0)
        _draw_text(commands, 5.0, 289.0, "Print at 100% scale / no fit-to-page. A4 portrait.", 8.0)
        commands.append("Q")
        page_contents.append(("\n".join(commands) + "\n").encode("ascii"))

    objects: list[bytes] = [b"", b"", b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
    page_refs = []
    for index, content in enumerate(page_contents):
        page_number = 4 + index * 2
        content_number = page_number + 1
        page_refs.append(f"{page_number} 0 R")
        objects.append(
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {_number(PAGE_W_MM * PT_PER_MM)} {_number(PAGE_H_MM * PT_PER_MM)}] "
            f"/Resources << /Font << /F1 3 0 R >> >> /Contents {content_number} 0 R >>".encode("ascii")
        )
        objects.append(b"<< /Length " + str(len(content)).encode("ascii") + b" >>\nstream\n" + content + b"endstream")
    objects[0] = b"<< /Type /Catalog /Pages 2 0 R >>"
    objects[1] = f"<< /Type /Pages /Kids [{' '.join(page_refs)}] /Count {len(page_refs)} >>".encode("ascii")
    output = bytearray(b"%PDF-1.4\n%APRILTAG-SHEET\n")
    offsets = [0]
    for number, obj in enumerate(objects, start=1):
        offsets.append(len(output))
        output.extend(f"{number} 0 obj\n".encode("ascii"))
        output.extend(obj)
        output.extend(b"\nendobj\n")
    xref = len(output)
    output.extend(f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode("ascii"))
    for offset in offsets[1:]:
        output.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    output.extend(
        f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R /ID [<00000000000000000000000000000000><00000000000000000000000000000000>] >>\nstartxref\n{xref}\n%%EOF\n".encode("ascii")
    )
    return bytes(output)


def _send_json(handler: BaseHTTPRequestHandler, status: int, value: object) -> None:
    body = (json.dumps(value, ensure_ascii=True, separators=(",", ":")) + "\n").encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Cache-Control", "no-store")
    handler.end_headers()
    handler.wfile.write(body)


def _state(root: Path) -> dict:
    errors = []
    bindings = None
    try:
        bindings = model.load_bindings(root)
    except model.BindingError as exc:
        errors.append(str(exc))
    try:
        patterns = model.read_json(root / model.PATTERNS_PATH)
    except model.BindingError as exc:
        patterns = {"family": model.FAMILY, "tags": []}
        errors.append(str(exc))
    return {"bindings": bindings, "patterns": patterns, "valid": not errors, "errors": errors}


def make_handler(root: Path):
    root = root.resolve()

    class SheetHandler(BaseHTTPRequestHandler):
        server_version = "AprilTagSheet/1"

        def do_GET(self) -> None:
            path = self.path.split("?", 1)[0]
            if path == "/api/state":
                _send_json(self, 200, _state(root))
            elif path == "/sheet.pdf":
                try:
                    body = render_pdf(root)
                except (OSError, model.BindingError, ValueError) as exc:
                    _send_json(self, 500, {"errors": [str(exc)]})
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/pdf")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Content-Disposition", "inline; filename=apriltag_sheet.pdf")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
            elif path == "/" or path == "/index.html":
                try:
                    body = (root / model.APRILTAG_DIR / "index.html").read_bytes()
                except OSError as exc:
                    _send_json(self, 404, {"errors": [str(exc)]})
                    return
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
            else:
                _send_json(self, 404, {"errors": ["not found"]})

        def do_POST(self) -> None:
            if self.path.split("?", 1)[0] != "/api/bind":
                _send_json(self, 404, {"errors": ["not found"]})
                return
            host = self.headers.get("Host", "")
            if re.fullmatch(r"(?:127\.0\.0\.1|localhost)(?::([0-9]{1,5}))?", host, re.IGNORECASE) is None:
                _send_json(self, 403, {"errors": ["Host must be 127.0.0.1 or localhost"]})
                return
            port_match = re.search(r":([0-9]+)$", host)
            if port_match and not 1 <= int(port_match.group(1)) <= 65535:
                _send_json(self, 403, {"errors": ["Host port is invalid"]})
                return
            if self.headers.get("X-Apriltag-Sheet") != "1":
                _send_json(self, 403, {"errors": ["X-Apriltag-Sheet: 1 is required"]})
                return
            if self.headers.get_content_type() != "application/json":
                _send_json(self, 415, {"errors": ["Content-Type must be application/json"]})
                return
            try:
                length = int(self.headers.get("Content-Length", ""))
            except ValueError:
                _send_json(self, 411, {"errors": ["Content-Length is required"]})
                return
            if length < 0:
                _send_json(self, 400, {"errors": ["Content-Length is invalid"]})
                return
            if length > MAX_BODY_BYTES:
                _send_json(self, 413, {"errors": ["request body exceeds 16 KiB"]})
                return
            body = self.rfile.read(length)
            if len(body) != length:
                _send_json(self, 400, {"errors": ["request body is incomplete"]})
                return
            try:
                value = json.loads(body.decode("utf-8"), object_pairs_hook=model._object_pairs)
                bindings = model.validate_bindings(value)
                model.write_bindings_and_types(root, bindings)
            except (UnicodeDecodeError, json.JSONDecodeError, model.BindingError, OSError) as exc:
                _send_json(self, 400, {"errors": [str(exc)]})
                return
            _send_json(self, 200, _state(root))

        def log_message(self, format: str, *args: object) -> None:
            sys.stderr.write("%s - %s\n" % (self.address_string(), format % args))

    return SheetHandler


def make_server(root: Path, port: int = 0) -> ThreadingHTTPServer:
    if not 0 <= port <= 65535:
        raise ValueError("port must be in 0..65535")
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(root))
    server.daemon_threads = True
    return server


def serve(root: Path, port: int, no_browser: bool) -> None:
    server = make_server(root, port)
    url = f"http://127.0.0.1:{server.server_address[1]}/"
    print(f"AprilTag sheet: {url}", flush=True)
    if not no_browser:
        webbrowser.open(url, new=2)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    serve_command = subcommands.add_parser("serve", help="serve the offline browser binding page")
    serve_command.add_argument("--port", type=int, default=0, help="loopback port; 0 chooses an ephemeral port")
    serve_command.add_argument("--no-browser", action="store_true")
    serve_command.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    pdf_command = subcommands.add_parser("pdf", help="write a deterministic A4 print sheet")
    pdf_command.add_argument("--out", type=Path)
    pdf_command.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    generate_command = subcommands.add_parser("generate", help="regenerate committed TypeScript and pattern data")
    generate_command.add_argument("--check", action="store_true", help="fail if generated files are stale")
    generate_command.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    check_command = subcommands.add_parser("check", help="validate bindings and generated files")
    check_command.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = args.root.resolve()
    try:
        if args.command == "serve":
            serve(root, args.port, args.no_browser)
        elif args.command == "pdf":
            output = args.out if args.out is not None else root / ".test-build" / "apriltag_sheet.pdf"
            if not output.is_absolute():
                output = Path.cwd() / output
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(render_pdf(root))
            print(f"Wrote {output} ({output.stat().st_size} bytes)")
        elif args.command == "generate" and args.check:
            model.check_generated(root)
            print("AprilTag generated files are current.")
        elif args.command == "generate":
            model.generate(root)
            print("Generated AprilTag TypeScript and pattern data.")
        else:
            model.check_generated(root)
            print("AprilTag bindings and generated files are valid.")
    except (OSError, ValueError) as exc:
        print(f"AprilTag sheet error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
