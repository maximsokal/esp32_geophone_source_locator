# WebUI ↔ LVGL Style Unification — Progress

Базовая ревизия: `9e1df383`. Все изменения не закоммичены. Evidence: 2026-11-07.

## Статус срезов

| Срез | Статус | Суть |
|---|---|---|
| S02 тема и дрейф | DONE | `theme.css` — единственный источник 11 ролей; `theme.ts` — реестр. Drift-тест читает настоящий `components/ui/ui_theme.h`, сверяет с CSS и TS, ловит подмену цвета |
| S03 поверхности | DONE | html/body/#root/shell — плоский BACKGROUND без градиентов; карточка PANEL, граница PANEL_ALT 1px, радиус 7px; `.card-heading > button` держит 44px |
| S05 кнопки/панели | DONE | Взято с экрана ESP32 (`ui_shell.c`): кнопка = PANEL_ALT + рамка ACCENT, secondary = PANEL + PANEL_ALT, danger = ERROR + текст HEADER, плашки и алерты = цветной текст GOOD/WARNING/MUTED на PANEL_ALT (алерт ERROR: рамка ERROR, текст TEXT ради контраста 4.5). Контракт обновлён в `tests/ui_contract.json` |
| S06 свежесть статуса | DONE | `statusStale` = есть последний `status` и ошибка `linkError`. Sensors: баннер (EN+UK), READY/MEASURING/CONNECTED нейтральны, NOT_CONNECTED остаётся красным, пилюля pod не зелёная. Unknown status — нейтральный + текст. Модель свежести выбрана по умолчанию (без возраста снимка) |
| S07 Live-каналы | DONE | Вариант A (паритет TFT): API channel n → график n-1. Метки G1..G4 окрашены статусом звена и имеют sr-only текст; дубли/0/вне диапазона → нейтрально; данные `/api/seismic` и SVG не менялись; новых запросов нет. Модуль `features/sensors/nodeLinkStatus.ts` общий для Sensors и Live |
| S08 Live/Saved | PARTIAL | Фоны графиков и подписи на ролях; saved не окрашивается статусом звена (не подключён) |
| S09 сканер DOM/HUD | DONE | `styles.css`, `scan.css`, `StructuralGuideOverlay.css` целиком на 11 ролях; полупрозрачные панели — `rgba` тех же ролей. Исключения: красная кнопка завершения `#991b1b` (проверяется тестом), чёрные тени |
| S10/S11 3D палитра/материалы | DONE | Только цвета палитры: weak=ERROR, improving=WARNING, stable=GOOD, unknown=MUTED, free=LINK_MEASURING, рёбра/пины=ACCENT, bounds=TEXT, источник=ERROR, неопределённость=LINK_MEASURING, датчики GOOD/LINK/WARNING/ACCENT; шейдер live-coverage той же палитрой. Пороги 0.42/0.72, геометрия и позиции не менялись. Тест `tests/xr/threeThemePalette.test.ts`. Цвета методов локализации (`localizationOverlay.ts`) не менялись: им нужно больше различимых цветов, чем в палитре. Видимый WebGL-canvas с readback отдельной fixture не добавлялся (есть `graphics` 12/12 и `webGlPixelReadback`) |
| S12 печать | DONE (regression) | Тест `reportPrintTheme.test.ts`: документ без тёмных цветов темы, body без background, текст `#111`; QR `#fff`/`#000` в `ReportView` |
| S13 WebXR | DONE | Opaque XR фон = BACKGROUND (`!important` сохранён); passthrough alpha=0 проверен существующими тестами; реальная камера — HIL, NOT RUN |
| S14 a11y/responsive | REGRESSION | layout-набор 85/85 (7 размеров, EN/UK, 200%, axe) |
| S15 ESP/asset | DONE | Единственный initial CSS, app.js gzip 67651 B (до работ 67401 B, +250 B), 1 vendor request, 28 ESP-пар |
| S16 canonical visual | DONE | Через WSL Ubuntu + `bwrap` с распакованным pinned-образом (без Docker, см. `docs/ui-regression.md`): `probe` совпал с manifest, 2×capture 96/96, stability STABLE, candidates `222cd7ed…27fe` (96 кадров, фон/кнопки) и `4760bf73…5f6b` (ещё 9 кадров сканера после HUD). Один флейк `compare` (угловые пиксели карточки, `uk-workflow-result`) не повторился при перезапуске. Просмотрено 10 экранов (home, sensors, sensors-disconnected, sensor-check, recording, UK result, UK pair-failed, scanner, objects-long 320, home-error). 96 эталонов утверждены поимённо, PNG и `manifest.json` скопированы в `webui/tests/e2e/snapshots`; `compare` 76/76 critical passed |

## Evidence (все exit 0)

typecheck, typecheck:tests, typecheck:e2e; `test:console` 1025/1025; `test:components` 88/88; `test:domain` 18/18; `test:xr` 1095 (4 skipped); `test:ui:theme-surfaces` 6/6; `test:ui:layout` 85/85; `test:ui:graphics` 12/12; `npm run build` + `verify:bundle`; `i18n_generate.py --check`; `run_local_verify.py --scope ast`; `git diff --check`.

Старые ожидания, изменённые по утверждённому дизайну (`scanner-style.spec.ts`): фон shell после выхода из сканера и фон opaque XR теперь `rgb(8,17,31)`.

## Не запускалось

Firmware build, HIL, мобильный WebXR. 20 extended-кадров visual не сравнивались. Дополнительно прошли: `test:ui:network` 77/77, `test:ui:tft` (984 кадра LVGL, Playwright 9/9), `test:ui:visual:tools` 8/8, `gen_nrf_log_events.py --check`.

## Риски и решения по умолчанию

- Свежесть статуса: без возраста снимка; решение выбрано исполнителем, не утверждено отдельно.
- Вариант A паритета каналов принят как цель документа; физическая маршрутизация не менялась.
- Оставшиеся цвета (кнопки, алерты, HUD, научные) — отдельным срезом после утверждения дизайна.
- RED на старом HEAD отдельным прогоном не воспроизводился; старые значения перечислены в `theme-surfaces.spec.ts`.
