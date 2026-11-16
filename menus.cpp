#define WLR_USE_UNSTABLE

#include "menus.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <sdbus-c++/sdbus-c++.h>
#include <wayland-server-core.h>

#include <chrono>
#include <map>
#include <memory>
#include <poll.h>

namespace OmarchyFloatingMenus {

namespace {

constexpr const char *NAME = "org.omarchy.Titlebars";
constexpr const char *PATH = "/org/omarchy/Titlebars";
constexpr const char *REGISTRAR = "org.omarchy.Titlebars.AppMenu1";
constexpr const char *DBUSMENU = "com.canonical.dbusmenu";
// A client registering more windows than this is broken, not busy.
constexpr size_t MAX_WINDOWS = 1024;
constexpr size_t MAX_ITEMS = 64;
constexpr size_t MAX_LABEL = 128;

// GetLayout's (ia{sv}av): an item, its properties, its children as variants.
using Layout = sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>, std::vector<sdbus::Variant>>;

struct SMenu {
  std::string service; // the registering connection's unique name
  std::string path;
  std::unique_ptr<sdbus::IProxy> proxy;
  sdbus::Slot signals;
  std::vector<SItem> items;
  uint64_t serial = 0; // which registration a reply belongs to
  bool fetching = false;
  bool again = false; // changed while a fetch was out
};

struct SState {
  std::unique_ptr<sdbus::IConnection> bus;
  std::unique_ptr<sdbus::IObject> registrar;
  std::unique_ptr<sdbus::IProxy> daemon;
  sdbus::Slot owners;
  wl_event_source *busSource = nullptr;
  wl_event_source *wakeSource = nullptr;
  wl_event_source *timer = nullptr;
  std::map<std::string, SMenu> menus; // by window tag
  std::function<void(const std::string &)> changed;
  uint64_t serials = 0;
};

std::unique_ptr<SState> g_state;
const std::vector<SItem> NO_ITEMS;

void pump();
void fetch(const std::string &tag);

int onReadable(int, uint32_t, void *) {
  pump();
  return 0;
}

int onTimer(void *) {
  pump();
  return 0;
}

void fail(const std::string &why) {
  LOG(Log::ERR, "[hyprbars] menus off: {}", why);
  auto changed = g_state ? g_state->changed : nullptr;
  std::vector<std::string> tags;
  if (g_state)
    for (auto &[tag, _] : g_state->menus)
      tags.push_back(tag);
  stop();
  if (changed)
    for (auto &tag : tags)
      changed(tag);
}

// Runs what the bus has for us, then asks the event loop to wake us for what
// it will have next. Called after anything that queues a message as well.
void pump() {
  if (!g_state || !g_state->bus)
    return;
  try {
    while (g_state && g_state->bus && g_state->bus->processPendingEvent()) {
    }
    if (!g_state || !g_state->bus)
      return;
    const auto poll = g_state->bus->getEventLoopPollData();
    uint32_t mask = 0;
    if (poll.events & POLLIN)
      mask |= WL_EVENT_READABLE;
    if (poll.events & POLLOUT)
      mask |= WL_EVENT_WRITABLE;
    wl_event_source_fd_update(g_state->busSource, mask);
    const int timeout = poll.getPollTimeout();
    // 0 disarms a wl timer, so "now" is the next millisecond.
    wl_event_source_timer_update(g_state->timer, timeout < 0 ? 0 : std::max(timeout, 1));
  } catch (const std::exception &e) {
    fail(e.what());
  }
}

std::string clip(std::string s) {
  if (s.size() > MAX_LABEL)
    s.resize(MAX_LABEL);
  return s;
}

template <typename T> std::optional<T> prop(const std::map<std::string, sdbus::Variant> &props, const char *name) {
  const auto it = props.find(name);
  if (it == props.end() || !it->second.containsValueOfType<T>())
    return std::nullopt;
  return it->second.get<T>();
}

std::vector<SItem> topItems(const Layout &root) {
  std::vector<SItem> items;
  for (const auto &child : std::get<2>(root)) {
    if (items.size() >= MAX_ITEMS)
      break;
    if (!child.containsValueOfType<Layout>())
      continue;
    const auto item = child.get<Layout>();
    const auto &props = std::get<1>(item);
    if (prop<std::string>(props, "type").value_or("standard") == "separator" || !prop<bool>(props, "visible").value_or(true))
      continue;
    auto label = plainLabel(prop<std::string>(props, "label").value_or(""));
    if (label.empty())
      continue;
    items.push_back({std::get<0>(item), clip(std::move(label)), prop<bool>(props, "enabled").value_or(true)});
  }
  return items;
}

void fetch(const std::string &tag) {
  auto it = g_state->menus.find(tag);
  if (it == g_state->menus.end())
    return;
  auto &menu = it->second;
  if (menu.fetching) {
    menu.again = true;
    return;
  }
  menu.fetching = true;
  menu.again = false;
  const auto serial = menu.serial;
  try {
    menu.proxy->callMethodAsync("GetLayout")
        .onInterface(DBUSMENU)
        .withArguments(int32_t{0}, int32_t{1}, std::vector<std::string>{"label", "visible", "enabled", "type"})
        .uponReplyInvoke([tag, serial](std::optional<sdbus::Error> error, uint32_t, Layout root) {
          if (!g_state)
            return;
          auto it = g_state->menus.find(tag);
          if (it == g_state->menus.end() || it->second.serial != serial)
            return;
          auto &menu = it->second;
          menu.fetching = false;
          if (error)
            LOG(Log::WARN, "[hyprbars] menus: menu of {} unread: {}", tag, error->getMessage());
          else {
            auto items = topItems(root);
            const bool same = items.size() == menu.items.size() &&
                              std::equal(items.begin(), items.end(), menu.items.begin(),
                                         [](const SItem &a, const SItem &b) { return a.id == b.id && a.label == b.label && a.enabled == b.enabled; });
            if (!same) {
              menu.items = std::move(items);
              g_state->changed(tag);
            }
          }
          if (menu.again)
            fetch(tag);
        });
  } catch (const std::exception &e) {
    menu.fetching = false;
    LOG(Log::WARN, "[hyprbars] menus: menu of {} unread: {}", tag, e.what());
  }
}

void forget(const std::string &tag) {
  if (g_state->menus.erase(tag))
    g_state->changed(tag);
}

void registerWindow(const std::string &tag, const sdbus::ObjectPath &path, const std::string &sender) {
  if (tag.empty() || tag.size() > 256)
    throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.InvalidArgs"}, "a window tag is 1 to 256 bytes");
  auto it = g_state->menus.find(tag);
  if (it == g_state->menus.end() && g_state->menus.size() >= MAX_WINDOWS)
    throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.LimitsExceeded"}, "too many windows");
  if (it != g_state->menus.end() && it->second.service == sender && it->second.path == path)
    return fetch(tag);

  SMenu menu;
  menu.service = sender;
  menu.path = path;
  menu.serial = ++g_state->serials;
  menu.proxy = sdbus::createProxy(*g_state->bus, sdbus::ServiceName{sender}, sdbus::ObjectPath{path});
  const auto rule = std::format("type='signal',sender='{}',path='{}',interface='{}'", sender, std::string{path}, DBUSMENU);
  menu.signals = g_state->bus->addMatchAsync(
      rule,
      [tag](sdbus::Message message) {
        const std::string member = message.getMemberName() ? message.getMemberName() : "";
        if (g_state && (member == "LayoutUpdated" || member == "ItemsPropertiesUpdated"))
          fetch(tag);
      },
      [](sdbus::Message) {}, sdbus::return_slot);
  const bool had = it != g_state->menus.end() && !it->second.items.empty();
  g_state->menus.insert_or_assign(tag, std::move(menu));
  if (had)
    g_state->changed(tag);
  fetch(tag);
}

void onOwnerChanged(sdbus::Message message) {
  std::string name, before, after;
  message >> name >> before >> after;
  if (!after.empty() || !g_state)
    return;
  std::vector<std::string> gone;
  for (auto &[tag, menu] : g_state->menus)
    if (menu.service == name)
      gone.push_back(tag);
  for (auto &tag : gone)
    forget(tag);
}

} // namespace

void start(std::function<void(const std::string &tag)> changed) {
  if (g_state)
    return;
  g_state = std::make_unique<SState>();
  g_state->changed = std::move(changed);
  try {
    auto &state = *g_state;
    state.bus = sdbus::createSessionBusConnection();

    state.registrar = sdbus::createObject(*state.bus, sdbus::ObjectPath{PATH});
    auto *registrar = state.registrar.get();
    registrar
        ->addVTable(sdbus::registerMethod("RegisterWindow")
                        .withInputParamNames("tag", "menu")
                        .implementedAs([registrar](const std::string &tag, const sdbus::ObjectPath &path) {
                          const char *sender = registrar->getCurrentlyProcessedMessage().getSender();
                          registerWindow(tag, path, sender ? sender : "");
                        }),
                    sdbus::registerMethod("UnregisterWindow").withInputParamNames("tag").implementedAs([registrar](const std::string &tag) {
                      const char *sender = registrar->getCurrentlyProcessedMessage().getSender();
                      auto it = g_state->menus.find(tag);
                      if (it != g_state->menus.end() && sender && it->second.service == sender)
                        forget(tag);
                    }))
        .forInterface(REGISTRAR);

    state.owners = state.bus->addMatchAsync("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
                                            onOwnerChanged, [](sdbus::Message) {}, sdbus::return_slot);

    // Owning the name is asked for, not waited on: 4 is DBUS_NAME_FLAG_DO_NOT_QUEUE.
    state.daemon = sdbus::createProxy(*state.bus, sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
    state.daemon->callMethodAsync("RequestName")
        .onInterface("org.freedesktop.DBus")
        .withArguments(std::string{NAME}, uint32_t{4})
        .uponReplyInvoke([](std::optional<sdbus::Error> error, uint32_t reply) {
          if (error)
            LOG(Log::ERR, "[hyprbars] menus: can't own {}: {}", NAME, error->getMessage());
          else if (reply != 1 && reply != 4)
            LOG(Log::ERR, "[hyprbars] menus: {} belongs to another process", NAME);
        });

    const auto poll = state.bus->getEventLoopPollData();
    auto *loop = g_pCompositor->m_wlEventLoop;
    state.busSource = wl_event_loop_add_fd(loop, poll.fd, WL_EVENT_READABLE, onReadable, nullptr);
    state.wakeSource = wl_event_loop_add_fd(loop, poll.eventFd, WL_EVENT_READABLE, onReadable, nullptr);
    state.timer = wl_event_loop_add_timer(loop, onTimer, nullptr);
  } catch (const std::exception &e) {
    fail(e.what());
    return;
  }
  pump();
}

void stop() {
  if (!g_state)
    return;
  auto state = std::move(g_state);
  for (auto *source : {state->busSource, state->wakeSource, state->timer})
    if (source)
      wl_event_source_remove(source);
  // Proxies and slots before the connection they live on.
  state->menus.clear();
  state->owners = {};
  state->daemon.reset();
  state->registrar.reset();
  state->bus.reset();
}

const std::vector<SItem> &items(const std::string &tag) {
  if (!g_state)
    return NO_ITEMS;
  const auto it = g_state->menus.find(tag);
  return it == g_state->menus.end() ? NO_ITEMS : it->second.items;
}

void click(const std::string &tag, int32_t id, int32_t x, int32_t y) {
  if (!g_state)
    return;
  const auto it = g_state->menus.find(tag);
  if (it == g_state->menus.end()) {
    LOG(Log::INFO, "[hyprbars] menus: click on {} item {}: no menu registered", tag, id);
    return;
  }
  LOG(Log::INFO, "[hyprbars] menus: click on {} item {} at {},{} to {}", tag, id, x, y, it->second.service);
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  try {
    it->second.proxy->callMethodAsync("Event")
        .onInterface(DBUSMENU)
        .withArguments(id, std::string{"clicked"}, sdbus::Variant{sdbus::Struct<int32_t, int32_t>{x, y}}, static_cast<uint32_t>(now))
        .uponReplyInvoke([tag, id](std::optional<sdbus::Error> error) {
          if (error)
            LOG(Log::WARN, "[hyprbars] menus: click on {} item {} refused: {}", tag, id, error->getMessage());
          else
            LOG(Log::INFO, "[hyprbars] menus: click on {} item {} answered", tag, id);
        });
  } catch (const std::exception &e) {
    LOG(Log::WARN, "[hyprbars] menus: click on {} not sent: {}", tag, e.what());
  }
  pump();
}

std::string plainLabel(const std::string &label) {
  std::string out;
  out.reserve(label.size());
  for (size_t i = 0; i < label.size(); ++i) {
    if (label[i] == '_') {
      if (i + 1 < label.size() && label[i + 1] == '_')
        out += label[++i];
      continue;
    }
    out += label[i];
  }
  return out;
}

} // namespace OmarchyFloatingMenus
