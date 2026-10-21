"""Render all production menus with the managed LVGL on a native host."""
from __future__ import annotations

import argparse
import html
import json
import os
from pathlib import Path
import shutil
import subprocess

from run_host_tests import compiler_from_args, ensure_native_compiler, compiler_description, is_msvc, load_msvc_environment

ROOT = Path(__file__).resolve().parents[1]
GOLDENS = ROOT / "tests/ui_render/golden"


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
    compiler = compiler_from_args(None)
    if not compiler:
        raise RuntimeError("A native C compiler is required (MSVC, GCC or Clang)")
    ensure_native_compiler(compiler, compiler_description(compiler))
    environment = load_msvc_environment(compiler) if is_msvc(compiler) else os.environ.copy()
    environment["VSLANG"] = "1033"
    cmake = find_tool("cmake")
    ninja = find_tool("ninja")
    build_dir.mkdir(parents=True, exist_ok=True)
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
    output.mkdir(parents=True, exist_ok=True)
    references = output / "references"
    references.mkdir(exist_ok=True)
    for source in GOLDENS.glob("*.png"):
        shutil.copy2(source, references / source.name)
    # LVGL's mismatch filename has a 128-byte buffer. Keep reference names short
    # by running in the artifact directory, even when the workspace path is long.
    result = subprocess.run([str(binary), "."], cwd=output, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=90)
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
        '<a href="frames.jsonl">Границы и стили виджетов</a>. Проверки геометрии и перекрытий выполняет pytest.</p>'
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
    binary = build(args.build_dir.resolve())
    output = args.output_dir.resolve()
    frames = render(binary, output)
    golden_frames = [frame for frame in frames if frame["golden"]]
    print(f"Real LVGL: {len(frames)} frames, {len(golden_frames)} reference views. Artifacts: {output}")
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
