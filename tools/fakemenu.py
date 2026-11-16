"""A com.canonical.dbusmenu server for tools/menus.py.

  fakemenu.py TAG LOG

Serves a menu bar at /MenuBar, registers it for the window tagged TAG with
the titlebar plugin's registrar (again whenever the registrar's name comes
back), and appends one JSON line per call it gets to LOG. On SIGUSR1 it
renames its menus (LayoutUpdated); on SIGTERM it exits.
"""

import json
import signal
import sys

import gi

gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib  # noqa: E402

TAG, LOG = sys.argv[1], sys.argv[2]
XML = '''<node><interface name="com.canonical.dbusmenu">
  <method name="GetLayout"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
    <arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/></method>
  <method name="Event"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/><arg type="u" direction="in"/></method>
  <method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
  <signal name="LayoutUpdated"><arg type="u"/><arg type="i"/></signal>
  <property name="Version" type="u" access="read"/>
</interface></node>'''

state = {'revision': 1, 'labels': ['_File', 'Edit', 'View', 'Disabled', 'Help']}


def log(**entry):
  with open(LOG, 'a') as f:
    f.write(json.dumps(entry) + '\n')


def layout():
  children = []
  for i, label in enumerate(state['labels']):
    props = {'label': GLib.Variant('s', label), 'children-display': GLib.Variant('s', 'submenu')}
    if label == 'Disabled':
      props['enabled'] = GLib.Variant('b', False)
    children.append(GLib.Variant('(ia{sv}av)', (i + 1, props, [])))
  # A separator and a hidden item, which the bar leaves out.
  children.append(GLib.Variant('(ia{sv}av)', (90, {'type': GLib.Variant('s', 'separator')}, [])))
  children.append(GLib.Variant('(ia{sv}av)', (91, {'label': GLib.Variant('s', 'Hidden'), 'visible': GLib.Variant('b', False)}, [])))
  return GLib.Variant('(u(ia{sv}av))', (state['revision'], (0, {}, children)))


def on_call(conn, sender, path, iface, method, params, invocation):
  if method == 'GetLayout':
    log(call='GetLayout', args=params.unpack()[:2])
    invocation.return_value(layout())
  elif method == 'Event':
    item, event, data, _ = params.unpack()
    log(call='Event', id=item, event=event, data=list(data) if isinstance(data, tuple) else data)
    invocation.return_value(None)
  elif method == 'AboutToShow':
    invocation.return_value(GLib.Variant('(b)', (False,)))


def on_get(conn, sender, path, iface, name):
  return GLib.Variant('u', 3)


bus = Gio.bus_get_sync(Gio.BusType.SESSION)
bus.register_object('/MenuBar', Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0], on_call, on_get, None)


def register(*_):
  def done(conn, result):
    try:
      conn.call_finish(result)
      log(call='registered')
    except GLib.Error as e:
      log(call='register-failed', error=e.message)
  bus.call('org.omarchy.Titlebars', '/org/omarchy/Titlebars', 'org.omarchy.Titlebars.AppMenu1', 'RegisterWindow',
           GLib.Variant('(so)', (TAG, '/MenuBar')), None, Gio.DBusCallFlags.NONE, 2000, None, done)


Gio.bus_watch_name_on_connection(bus, 'org.omarchy.Titlebars', Gio.BusNameWatcherFlags.NONE, register, None)


def rename():
  state['labels'] = ['Shell', 'Edit', 'Window']
  state['revision'] += 1
  bus.emit_signal(None, '/MenuBar', 'com.canonical.dbusmenu', 'LayoutUpdated', GLib.Variant('(ui)', (state['revision'], 0)))
  log(call='renamed')
  return GLib.SOURCE_REMOVE


loop = GLib.MainLoop()
GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR1, rename)
GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, loop.quit)
log(call='started', name=bus.get_unique_name())
loop.run()
