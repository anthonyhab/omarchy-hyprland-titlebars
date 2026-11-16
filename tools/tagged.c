// A plain window with an xdg-toplevel tag, for tools/menus.py:
//   tagged APP_ID TAG
// It runs until killed.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-tag-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct xdg_toplevel_tag_manager_v1 *tags;
static struct wl_surface *surface;
static int width = 600, height = 400, configured;

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version) {
  if (!strcmp(iface, wl_compositor_interface.name))
    compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, wl_shm_interface.name))
    shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  else if (!strcmp(iface, xdg_wm_base_interface.name))
    wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
  else if (!strcmp(iface, xdg_toplevel_tag_manager_v1_interface.name))
    tags = wl_registry_bind(registry, name, &xdg_toplevel_tag_manager_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
static const struct xdg_wm_base_listener wm_listener = {ping};

static void draw(void) {
  const int stride = width * 4, size = stride * height;
  int fd = memfd_create("tagged", 0);
  if (fd < 0 || ftruncate(fd, size) < 0)
    exit(1);
  uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  for (int i = 0; i < width * height; i++)
    pixels[i] = 0xff3b4261;
  munmap(pixels, size);
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  wl_surface_attach(surface, buffer, 0, 0);
  wl_surface_damage_buffer(surface, 0, 0, width, height);
  wl_surface_commit(surface);
}

static void surface_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
  xdg_surface_ack_configure(xdg, serial);
  configured = 1;
  draw();
}
static const struct xdg_surface_listener surface_listener = {surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
  if (w > 0 && h > 0) {
    width = w;
    height = h;
  }
}
static void toplevel_close(void *data, struct xdg_toplevel *t) { exit(0); }
static const struct xdg_toplevel_listener toplevel_listener = {toplevel_configure, toplevel_close};

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: tagged APP_ID TAG\n");
    return 2;
  }
  struct wl_display *display = wl_display_connect(NULL);
  if (!display)
    return 1;
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  if (!compositor || !shm || !wm || !tags) {
    fprintf(stderr, "tagged: missing a global (xdg-toplevel-tag?)\n");
    return 1;
  }
  xdg_wm_base_add_listener(wm, &wm_listener, NULL);
  surface = wl_compositor_create_surface(compositor);
  struct xdg_surface *xdg = xdg_wm_base_get_xdg_surface(wm, surface);
  xdg_surface_add_listener(xdg, &surface_listener, NULL);
  struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg);
  xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
  xdg_toplevel_set_app_id(toplevel, argv[1]);
  xdg_toplevel_set_title(toplevel, argv[1]);
  xdg_toplevel_tag_manager_v1_set_toplevel_tag(tags, toplevel, argv[2]);
  wl_surface_commit(surface);
  while (wl_display_dispatch(display) != -1) {
  }
  return 0;
}
