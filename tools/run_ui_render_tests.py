"""Render all production menus with the managed LVGL on a native host."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import html
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

from run_host_tests import compiler_from_args, ensure_native_compiler, compiler_description, is_msvc, load_msvc_environment

ROOT = Path(__file__).resolve().parents[1]
GOLDENS = ROOT / "tests/ui_render/golden"


def file_hash(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def file_set_hash(paths: list[Path], base: Path) -> dict:
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.relative_to(base).as_posix().encode("utf-8") + b"\0")
        digest.update(bytes.fromhex(file_hash(path)))
    return {"files": len(paths), "sha256": digest.hexdigest()}


def write_record(path: Path, record: dict) -> None:
    path.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def start_run(output: Path, phase: str) -> dict:
    output = output.resolve()
    goldens = GOLDENS.resolve()
    if output == goldens or goldens in output.parents or (output / "references").resolve() == goldens:
        raise ValueError("The output/reference-copy directory must not overwrite source goldens")
    output.mkdir(parents=True, exist_ok=True)
    record = {"schema_version": 1, "status": "RUNNING", "phase": phase,
              "started_at_utc": datetime.now(timezone.utc).isoformat(),
              "environment": {"platform": platform.platform(), "machine": platform.machine(),
                              "python": platform.python_version()},
              "checks": ["native_render", "screenshot_comparison"]}
    write_record(output / "run.json", record)
    (output / "index.html").write_text(
        '<!doctype html><meta charset="utf-8"><title>LVGL run</title>'
        f'<h1>RUNNING: {phase}</h1><p><a href="run.json">Run metadata</a></p>', encoding="utf-8")
    return record


def fail_run(output: Path, record: dict, error: Exception) -> None:
    record.update(status="FAIL", error=str(error), finished_at_utc=datetime.now(timezone.utc).isoformat())
    write_record(output / "run.json", record)
    (output / "index.html").write_text(
        '<!doctype html><meta charset="utf-8"><title>LVGL run failed</title>'
        f'<h1>FAIL: {html.escape(record["phase"])}</h1><pre>{html.escape(str(error))}</pre>'
        '<p><a href="run.json">Run metadata</a> / <a href="render.log">Render log</a></p>'
        '<p>Remaining images may be from an earlier run. This run did not pass.</p>', encoding="utf-8")


def find_tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    if os.name == "nt":
        candidates = sorted(Path("C:/Espressif/tools").glob(f"{name}/*/**/{name}.exe"), reverse=True)
        if candidates:
            return str(candidates[0])
    raise RuntimeError(f"{name} is required; add it to PATH")


def build(build_dir: Path) -> Path:
    build_dir.mkdir(parents=True, exist_ok=True)
    record = {"schema_version": 1, "status": "RUNNING"}
    write_record(build_dir / "build.json", record)
    try:
        binary = build_native(build_dir, record)
        record.update(status="PASS", binary_sha256=file_hash(binary))
        write_record(build_dir / "build.json", record)
        return binary
    except Exception as error:
        record.update(status="FAIL", error=str(error))
        write_record(build_dir / "build.json", record)
        raise


def build_native(build_dir: Path, record: dict) -> Path:
    compiler = compiler_from_args(None)
    if not compiler:
        raise RuntimeError("A native C compiler is required (MSVC, GCC or Clang)")
    description = compiler_description(compiler)
    ensure_native_compiler(compiler, description)
    environment = load_msvc_environment(compiler) if is_msvc(compiler) else os.environ.copy()
    environment["VSLANG"] = "1033"
    cmake = find_tool("cmake")
    ninja = find_tool("ninja")
    source_files = [ROOT / "tools/run_ui_render_tests.py", ROOT / "tools/run_host_tests.py"]
    for directory in ("components", "main", "tests/ui_render", "nRF54L15/include",
                      "tests/host/stubs/object_store_profile"):
        source_files.extend(path for path in (ROOT / directory).rglob("*")
                            if path.suffix in {".c", ".h", ".cpp", ".cmake"} or path.name == "CMakeLists.txt")
    lvgl = ROOT / "managed_components/lvgl__lvgl"
    lvgl_files = [path for directory in (lvgl / "src", lvgl / "include")
                 for path in directory.rglob("*") if path.suffix in {".c", ".h", ".cpp"}]
    compiler_executable = Path(shutil.which(compiler[0]) or compiler[0]).resolve()
    record.update(
        head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        source=file_set_hash(source_files, ROOT),
        configuration={name: file_hash(ROOT / name) if (ROOT / name).is_file() else None for name in (
            "tests/ui_render/lv_conf.h", "tests/ui_render/CMakeLists.txt", "sdkconfig", "dependencies.lock")},
        font_sha256=file_hash(ROOT / "components/ui/fonts/ui_font_montserrat_14.c"),
        lvgl={"version": json.loads((lvgl / "library.json").read_text(encoding="utf-8"))["version"],
              **file_set_hash(lvgl_files, lvgl)},
        compiler={"command": compiler, "executable": str(compiler_executable),
                  "description": description, "sha256": file_hash(compiler_executable)},
    )
    commands = [
        [cmake, "-S", str(ROOT / "tests/ui_render"), "-B", str(build_dir), "-G", "Ninja",
         f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_C_COMPILER={compiler[0]}", "-DCMAKE_BUILD_TYPE=Release"],
        # A fresh native build also avoids stale header dependencies from
        # localized MSVC /showIncludes output on Windows.
        [cmake, "--build", str(build_dir), "--clean-first", "--parallel", "4"],
    ]
    for index, command in enumerate(commands):
        result = subprocess.run(command, env=environment, cwd=ROOT, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=300)
        log = build_dir / ("configure.log" if index == 0 else "build.log")
        log.write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            raise RuntimeError(f"UI renderer build failed; see {log}\n{(result.stdout + result.stderr)[-10000:]}")
    return build_dir / ("ui_render.exe" if os.name == "nt" else "ui_render")


def render(binary: Path, output: Path) -> list[dict]:
    record = start_run(output, "render")
    try:
        record["build"] = json.loads((binary.parent / "build.json").read_text(encoding="utf-8"))
        if record["build"]["status"] != "PASS" or record["build"]["binary_sha256"] != file_hash(binary):
            raise RuntimeError("Native executable has no matching successful build identity; run build() first")
        frames = render_frames(binary, output, record)
        compared = [frame for frame in frames if frame["golden"]]
        if not compared:
            raise RuntimeError("Native renderer produced no reference views")
        record.update(status="PASS" if all(frame["comparison"] == 1 for frame in compared) else "FAIL",
                      phase="compare", frames=len(frames),
                      comparisons={"matched": sum(frame["comparison"] == 1 for frame in compared),
                                   "missing": sum(frame["comparison"] == 2 for frame in compared),
                                   "different": sum(frame["comparison"] == 0 for frame in compared)},
                      finished_at_utc=datetime.now(timezone.utc).isoformat())
        write_record(output / "run.json", record)
        return frames
    except Exception as error:
        fail_run(output, record, error)
        raise


def render_frames(binary: Path, output: Path, record: dict) -> list[dict]:
    references = output / "references"
    references.mkdir(exist_ok=True)
    if references.resolve().parent != output.resolve():
        raise ValueError("Reference copies must stay inside the output directory")
    # Archive LVGL diagnostics before replacing every previous reference copy.
    for previous in references.glob("*.png"):
        if previous.name.endswith("_err.png"):
            diagnostics = output / "previous-errors"
            diagnostics.mkdir(exist_ok=True)
            if diagnostics.resolve().parent != output.resolve():
                raise ValueError("Diagnostics must stay inside the output directory")
            shutil.copy2(previous, diagnostics / previous.name)
        previous.unlink()
    for source in GOLDENS.glob("*.png"):
        shutil.copy2(source, references / source.name)
    record["references"] = {"source": str(GOLDENS), **file_set_hash(list(references.glob("*.png")), references)}
    write_record(output / "run.json", record)
    # A zero-exit process that produced no frames must not reuse an old success.
    (output / "frames.jsonl").unlink(missing_ok=True)
    (output / "render.log").write_text("", encoding="utf-8")
    # LVGL's mismatch filename has a 128-byte buffer. Keep reference names short
    # by running in the artifact directory, even when the workspace path is long.
    try:
        result = subprocess.run([str(binary), "."], cwd=output, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=90)
    except subprocess.TimeoutExpired as error:
        with (output / "render.log").open("w", encoding="utf-8") as log:
            for part in (error.stdout, error.stderr):
                if part:
                    log.write(part.decode("utf-8", errors="replace") if isinstance(part, bytes) else part)
        raise
    (output / "render.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Native UI test failed ({result.returncode}); see {output / 'render.log'}\n"
                           + result.stdout[-2000:] + result.stderr[-2000:])
    frames = [json.loads(line) for line in (output / "frames.jsonl").read_text(encoding="utf-8").splitlines()]
    cards = []
    families = {"home": "Главное меню", "workflow": "Сценарий измерений", "objects": "Объекты",
                "storage": "SD-карта", "wifi": "Wi-Fi", "system": "Система", "hardware": "Оборудование",
                "measurements": "Живые измерения", "boot": "Запуск и восстановление SD", "preview": "3D-просмотр"}
    for frame in frames:
        if not frame["golden"]:
            continue
        name = html.escape(frame["name"])
        status = {0: "отличается от эталона", 1: "эталон совпал", 2: "нет эталона"}[frame["comparison"]]
        family = html.escape(frame["family"])
        language = name[:2]
        cards.append(f'<figure data-family="{family}" data-lang="{language}" data-result="{frame["comparison"]}">'
                     f'<figcaption><strong>{families[frame["family"]]} / {language.upper()}</strong><br>{name}<br>{status}</figcaption>'
                     f'<a href="{name}.png"><img src="{name}.png" width="320" height="240" alt="{name}"></a></figure>')
    options = ''.join(f'<option value="{key}">{value}</option>' for key, value in families.items())
    (output / "index.html").write_text(
        '<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
        '<title>Geophone — все меню LVGL</title>'
        '<style>body{font:14px system-ui;background:#e8edf2;padding:16px;color:#152238}'
        'nav{display:flex;flex-wrap:wrap;gap:16px;position:sticky;top:0;background:#e8edf2;padding:12px 0;z-index:1}'
        'select{padding:6px}figure[hidden]{display:none}'
        'main{display:grid;grid-template-columns:repeat(auto-fit,320px);gap:16px}'
        'figure{margin:0}figcaption{min-height:70px;overflow-wrap:anywhere;font-size:13px;line-height:1.4}img{display:block}</style>'
        '<h1>Все меню Geophone / LVGL 9.6</h1>'
        f'<p>{len(frames)} состояний / {len(cards)} эталонных видов / RGB565 / 320×240. '
        'Настоящий код экранов, синтетические данные оборудования. '
        '<a href="frames.jsonl">Границы и стили виджетов</a> / <a href="run.json">Версия и среда запуска</a>. '
        'Проверки геометрии и перекрытий выполняет pytest.</p>'
        '<nav><label>Меню <select id="family"><option value="all">Все меню</option>' + options + '</select></label>'
        '<label>Язык <select id="language"><option value="all">EN + UK</option><option value="en">English</option>'
        '<option value="uk">Українська</option></select></label>'
        '<label><input type="checkbox" id="changed"> Только отличия от эталона</label><span id="count"></span></nav>'
        '<main>' + ''.join(cards) + '</main><script>'
        'const family=document.getElementById("family"),language=document.getElementById("language"),'
        'changed=document.getElementById("changed"),count=document.getElementById("count"),cards=[...document.querySelectorAll("figure")];'
        'function filter(){let visible=0;for(const card of cards){card.hidden=!(('
        'family.value==="all"||card.dataset.family===family.value)&&(language.value==="all"||card.dataset.lang===language.value)'
        '&&(!changed.checked||card.dataset.result!=="1"));if(!card.hidden)visible++;}'
        'count.textContent=`Показано: ${visible} / ${cards.length}`;}'
        'for(const control of [family,language,changed])control.addEventListener("change",filter);filter();</script></html>',
        encoding="utf-8")
    return frames


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".test-build/ui-render/native")
    parser.add_argument("--output-dir", type=Path, default=ROOT / ".test-build/ui-render/latest")
    parser.add_argument("--update-goldens", action="store_true",
                        help="Explicitly replace reference PNGs with this run; inspect their diff before committing")
    parser.add_argument("--render-only", action="store_true", help="Write candidate images/layout without accepting references")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    record = start_run(output, "build")
    record["build_metadata"] = str(args.build_dir.resolve() / "build.json")
    try:
        binary = build(args.build_dir.resolve())
    except Exception as error:
        fail_run(output, record, error)
        raise
    frames = render(binary, output)
    golden_frames = [frame for frame in frames if frame["golden"]]
    print(f"Real LVGL: {len(frames)} frames, {len(golden_frames)} reference views. Artifacts: {output}")
    identity = json.loads((output / "run.json").read_text(encoding="utf-8"))
    print(f"Comparison: {identity['status']}; source {identity['build']['head'][:12]}; "
          f"identity: {output / 'run.json'}")
    if args.update_goldens:
        GOLDENS.mkdir(parents=True, exist_ok=True)
        for frame in golden_frames:
            shutil.copy2(output / f"{frame['name']}.png", GOLDENS / f"{frame['name']}.png")
        print("Reference PNGs explicitly updated; review them and run the pytest screen checks.")
    elif not args.render_only:
        failures = [frame["name"] for frame in golden_frames if frame["comparison"] != 1]
        if failures:
            raise SystemExit(f"Screenshot mismatch/missing reference: {', '.join(failures[:10])}; see {output}")


if __name__ == "__main__":
    main()
