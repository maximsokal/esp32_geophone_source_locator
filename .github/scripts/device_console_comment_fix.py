from pathlib import Path

path = Path('components/web_server/web_server.c')
text = path.read_text(encoding='utf-8')
old = '     * under /assets/*: root, client-log, assets, status, control, seismic page,\n'
new = '     * under the wildcard assets route: root, client-log, assets, status, control, seismic page,\n'
if new not in text:
    if old not in text:
        raise SystemExit('Expected wildcard route comment not found')
    path.write_text(text.replace(old, new, 1), encoding='utf-8')
    print('Fixed strict-C wildcard route comment.')
else:
    print('Wildcard route comment already fixed.')
