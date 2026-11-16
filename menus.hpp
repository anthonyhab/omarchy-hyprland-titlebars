#pragma once

// Window menus for the titlebar, read over D-Bus.
//
// A client registers a window's menu with the registrar this plugin owns on
// the session bus, keyed by the window's xdg-toplevel tag:
//
//   service   org.omarchy.Titlebars
//   object    /org/omarchy/Titlebars
//   interface org.omarchy.Titlebars.AppMenu1
//     RegisterWindow(s tag, o menu)   the caller serves com.canonical.dbusmenu at `menu`
//     UnregisterWindow(s tag)
//
// The titlebar shows the menu's top-level items. A click on one sends the
// menu's Event(id, "clicked", (ii) x y, timestamp), where x y is the item's
// bottom-left corner in logical pixels from the window's top-left (y is 0:
// just under the bar). The client shows the dropdown itself: the plugin draws
// no menus.
//
// Everything runs on Hyprland's event loop and nothing waits on the bus: a
// slow or gone client costs the compositor nothing.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace OmarchyFloatingMenus {

struct SItem {
  int32_t id = 0;
  std::string label;
  bool enabled = true;
};

// Connects to the session bus and owns the registrar's name. `changed` runs
// with a window tag whenever that window's menu items change or go.
void start(std::function<void(const std::string &tag)> changed);
void stop();

// The top-level items for the window with this xdg-toplevel tag; empty when
// it has no menu.
const std::vector<SItem> &items(const std::string &tag);

void click(const std::string &tag, int32_t id, int32_t x, int32_t y);

// "_File" -> "File", "__" -> "_" (dbusmenu marks mnemonics with underscores).
std::string plainLabel(const std::string &label);

} // namespace OmarchyFloatingMenus
