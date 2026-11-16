"""Verify titlebar menus in an isolated nested Hyprland on a private session bus.

  python3 tools/menus.py build/titlebars.so [--artifacts DIR]

A fake app (fakemenu.py) serves a dbusmenu and registers it for a tagged
window (tagged.c). The check reads the titles off clicks along the bar: each
enabled top-level item must answer with its own Event at its own place, in
order, a disabled one and separators must take no clicks, a LayoutUpdated must
redraw the titles, an app that leaves must take its menu with it, and an app
must get its menu back after the plugin is unloaded and loaded again.

It re-runs itself under dbus-run-session, so the plugin never owns a name on
the user's bus, and never sends plugin commands outside the nested compositor.
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

source = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('library', type=Path)
parser.add_argument('--artifacts', type=Path, default=Path('/tmp/omarchy-hyprland-titlebars-menus'))
args = parser.parse_args()
args.library = args.library.resolve()

if os.environ.get('OHT_PRIVATE_BUS') != '1':
  env = dict(os.environ, OHT_PRIVATE_BUS='1')
  sys.exit(subprocess.run(['dbus-run-session', '--', sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]], env=env).returncode)

args.artifacts.mkdir(parents=True, exist_ok=True)
TAG = 'oht-menus-window'

with tempfile.TemporaryDirectory(prefix='oht-menus-') as scratch:
  scratch = Path(scratch)
  for xml, stem in [(source / 'wlr-virtual-pointer-unstable-v1.xml', 'pointer-protocol'),
                    (Path('/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml'), 'xdg-shell'),
                    (Path('/usr/share/wayland-protocols/staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml'), 'xdg-toplevel-tag-v1')]:
    header = f'{stem}.h' if stem == 'pointer-protocol' else f'{stem}-client-protocol.h'
    subprocess.run(['wayland-scanner', 'client-header', str(xml), str(scratch / header)], check=True)
    subprocess.run(['wayland-scanner', 'private-code', str(xml), str(scratch / f'{stem}.c')], check=True)
  subprocess.run(['cc', str(source / 'pointer.c'), str(scratch / 'pointer-protocol.c'), '-I', str(scratch), '-lwayland-client', '-o', str(scratch / 'pointer')], check=True)
  subprocess.run(['cc', str(source / 'tagged.c'), str(scratch / 'xdg-shell.c'), str(scratch / 'xdg-toplevel-tag-v1.c'), '-I', str(scratch), '-lwayland-client',
                  '-o', str(scratch / 'tagged')], check=True)

  config = scratch / 'hyprland.lua'
  config.write_text('''
hl.monitor({ output = "", mode = "1280x800@60", position = "0x0", scale = 1 })
hl.config({ general = { gaps_in = 8, gaps_out = 8, border_size = 2 }, animations = { enabled = false }, input = { follow_mouse = 0 }, misc = { disable_hyprland_logo = true, disable_splash_rendering = true } })
hl.window_rule({ name = "test-float", match = { class = ".*" }, float = true, size = { 700, 400 }, center = true })
if hl.plugin and hl.plugin.hyprbars then
  hl.config({ plugin = { hyprbars = {
    enabled = true, bar_height = 30, bar_color = "rgb(24283b)", col = { text = "rgb(c0caf5)" },
    bar_text_align = "left", bar_text_font = "Sans", bar_text_size = 12, bar_part_of_window = true, bar_padding = 10,
  } } })
  hl.plugin.hyprbars.add_button({ bg_color = "rgb(f7768e)", fg_color = "rgb(1a1b26)", size = 18, icon = "×", action = "true" })
end
''')
  parent_instances = json.loads(subprocess.check_output(['hyprctl', 'instances', '-j'], text=True))
  parent_signature = parent_instances[0]['instance'] if parent_instances else None
  parent_display = str(Path(os.environ['XDG_RUNTIME_DIR']) / parent_instances[0]['wl_socket'])
  env = dict(os.environ, XDG_RUNTIME_DIR=str(scratch), AQ_BACKEND='wayland', WAYLAND_DISPLAY=parent_display, HYPRLAND_NO_SD_NOTIFY='1', HYPRLAND_NO_SD_VARS='1')
  env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
  log = (args.artifacts / 'compositor.log').open('w')
  compositor = subprocess.Popen(['Hyprland', '--config', str(config)], env=env, stdout=log, stderr=log)
  menu_log = scratch / 'menu.jsonl'
  menu_log.touch()
  children = []
  trace = []

  def ctl(*argv):
    if argv[0] == 'plugin':
      signature = env.get('HYPRLAND_INSTANCE_SIGNATURE')
      if not signature or signature == parent_signature or env['XDG_RUNTIME_DIR'] != str(scratch):
        raise RuntimeError('Refusing to send a plugin command outside the nested compositor')
    result = subprocess.run(['hyprctl', *argv], env=env, capture_output=True, text=True, timeout=5, check=True)
    if result.stdout.startswith('Err') or "Couldn't" in result.stdout:
      raise RuntimeError(result.stdout)
    return result.stdout

  def entries():
    return [json.loads(line) for line in menu_log.read_text().splitlines() if line.strip()]

  def wait_for(predicate, what, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
      if predicate():
        return
      assert compositor.poll() is None, 'compositor exited'
      time.sleep(.05)
    raise AssertionError(f'timed out waiting for {what}; menu log: {entries()[-5:]}')

  def layouts():
    return sum(1 for e in entries() if e['call'] == 'GetLayout')

  def fake_menu():
    p = subprocess.Popen([sys.executable, str(source / 'fakemenu.py'), TAG, str(menu_log)], env=env)
    children.append(p)
    return p

  def capture(name):
    monitor = json.loads(ctl('monitors', '-j'))[0]['name']
    subprocess.run(['grim', '-o', monitor, str(args.artifacts / f'{name}.png')], env=env, check=True)

  def sweep():
    """Clicks along the bar's left half; the Events they made, in order."""
    window = json.loads(ctl('clients', '-j'))[0]
    y = window['at'][1] - 15
    seen = []
    for x in range(window['at'][0] + 4, window['at'][0] + 460, 10):
      before = len(entries())
      subprocess.run([str(scratch / 'pointer'), '1280', '800'], input=f'move {x} {y}\nwait 40\ndown\nwait 30\nup\nwait 60\n', text=True, env=env, check=True, timeout=10)
      time.sleep(.45)  # no double click
      got = [e for e in entries()[before:] if e['call'] == 'Event']
      now = json.loads(ctl('clients', '-j'))[0]['at']
      trace.append((x - window['at'][0], now, [(e['id'], e['data'][0]) for e in got]))
      seen += got
    return seen

  def runs(events):
    """Each item clicked, once, with where the click said it was."""
    out = []
    for e in events:
      if not out or out[-1]['id'] != e['id']:
        out.append(e)
      else:
        assert out[-1]['data'] == e['data'], f'one item, two places: {out[-1]} {e}'
    return out

  try:
    for _ in range(80):
      try:
        instances = json.loads(ctl('instances', '-j'))
      except (ValueError, subprocess.SubprocessError):
        instances = []
      if instances:
        env['HYPRLAND_INSTANCE_SIGNATURE'] = instances[0]['instance']
        env['WAYLAND_DISPLAY'] = instances[0]['wl_socket']
        break
      time.sleep(.1)
    else:
      raise RuntimeError('Isolated compositor did not start')
    time.sleep(.2)

    ctl('plugin', 'load', str(args.library))
    time.sleep(.3)
    ctl('reload')
    time.sleep(.2)
    window = subprocess.Popen([str(scratch / 'tagged'), 'oht-menus', TAG], env=env)
    children.append(window)
    wait_for(lambda: json.loads(ctl('clients', '-j')), 'the window')
    app = fake_menu()
    wait_for(lambda: layouts() >= 1, 'the bar to read the menu')
    time.sleep(.3)
    capture('menus')

    at = json.loads(ctl('clients', '-j'))[0]['at']
    raw = sweep()
    print('window before', at, 'after', json.loads(ctl('clients', '-j'))[0]['at'], [(e['id'], e['data']) for e in raw], flush=True)
    clicked = runs(raw)
    ids = [e['id'] for e in clicked]
    assert ids == [1, 2, 3, 5], f'clicked items {ids}, want File Edit View Help (Disabled takes none)'
    assert all(e['event'] == 'clicked' and e['data'][1] == 0 for e in clicked), clicked
    xs = [e['data'][0] for e in clicked]
    assert xs == sorted(xs) and len(set(xs)) == len(xs) and xs[0] > 0, xs
    print(f'PASS: titles answer clicks in order at x {xs}', flush=True)

    before = layouts()
    app.send_signal(signal.SIGUSR1)
    wait_for(lambda: layouts() > before, 'a re-read after LayoutUpdated')
    time.sleep(.3)
    capture('renamed')
    ids = [e['id'] for e in runs(sweep())]
    assert ids == [1, 2, 3], f'after a rename, clicked {ids}'
    print('PASS: LayoutUpdated redraws the titles', flush=True)

    app.terminate()
    app.wait(timeout=5)
    time.sleep(.5)
    capture('gone')
    assert compositor.poll() is None
    print('PASS: an app that leaves takes its menu (see gone.png)', flush=True)

    app = fake_menu()
    wait_for(lambda: sum(1 for e in entries() if e['call'] == 'registered') >= 2, 'a second registration')
    for cycle in range(3):
      ctl('plugin', 'unload', str(args.library))
      time.sleep(.3)
      assert compositor.poll() is None
      registered = sum(1 for e in entries() if e['call'] == 'registered')
      ctl('plugin', 'load', str(args.library))
      ctl('reload')
      wait_for(lambda: sum(1 for e in entries() if e['call'] == 'registered') > registered, f'registration after reload {cycle}')
    before = layouts()
    wait_for(lambda: layouts() >= before, 'the menu read after reloads')
    time.sleep(.3)
    capture('reloaded')
    ids = [e['id'] for e in runs(sweep())]
    assert ids == [1, 2, 3, 5], f'after reloads, clicked {ids}'
    print('PASS: unload/load keeps the compositor up and the menu comes back', flush=True)
  except AssertionError:
    for t in trace:
      print('trace', t, flush=True)
    raise
  finally:
    for p in children:
      if p.poll() is None:
        p.terminate()
    for p in children:
      try:
        p.wait(timeout=3)
      except subprocess.TimeoutExpired:
        p.kill()
    compositor.terminate()
    try:
      compositor.wait(timeout=5)
    except subprocess.TimeoutExpired:
      compositor.kill()
      compositor.wait()
    log.close()
print(f'Artifacts: {args.artifacts}')
