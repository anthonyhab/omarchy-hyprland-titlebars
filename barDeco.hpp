#pragma once

#define WLR_USE_UNSTABLE

#include <map>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRule.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include "globals.hpp"

#define private public
#include <hyprland/src/managers/input/InputManager.hpp>
#undef private

namespace Event {
struct SCallbackInfo;
}

class COmarchyFloatingHyprBar : public IHyprWindowDecoration {
public:
  COmarchyFloatingHyprBar(PHLWINDOW);
  virtual ~COmarchyFloatingHyprBar();

  virtual SDecorationPositioningInfo getPositioningInfo();

  virtual void onPositioningReply(const SDecorationPositioningReply &reply);

  virtual void draw(Render::CRenderContext &ctx, PHLMONITOR monitor, const float &a, const Render::SWindowRenderPresentation &presentation);

  virtual eDecorationType getDecorationType();

  virtual void updateWindow(PHLWINDOW);

  virtual void damageEntire();

  virtual eDecorationLayer getDecorationLayer();

  virtual uint64_t getDecorationFlags();

  bool m_bButtonsDirty = true;

  virtual std::string getDisplayName();

  PHLWINDOW getOwner();

  void updateRules();
  void onConfigReloaded();
  // Its window's menu items changed (menus.hpp).
  void onMenusChanged();
  // The window's xdg-toplevel tag, which its menu is registered under.
  std::string menuTag();

  WP<COmarchyFloatingHyprBar> m_self;

private:
  SBoxExtents m_seExtents;

  PHLWINDOWREF m_pWindow;

  CBox m_bAssignedBox;

  SP<Render::ITexture> m_pTextTex;
  // Title textures keyed by monitor scale x100; see renderPass().
  std::map<int, SP<Render::ITexture>> m_titleTexByScale;

  bool m_bWindowSizeChanged = false;
  bool m_hidden = false;
  bool m_bTitleColorChanged = false;
  bool m_bLastEnabledState = false;
  bool m_bWindowHasFocus = false;
  std::optional<CHyprColor> m_bForcedBarColor;
  std::optional<CHyprColor> m_bForcedTitleColor;

  Time::steady_tp m_lastMouseDown{};

  PHLANIMVAR<CHyprColor> m_cRealBarColor;

  Vector2D cursorRelativeToBar();

  void renderPass(Render::CRenderContext &ctx, PHLMONITOR, float const &a);
  void renderBarTitle(const Vector2D &bufferSize, const float scale);
  void renderBarButtons(Render::CRenderContext &ctx, CBox *barBox, const float scale, const float a);
  void renderBarButtonsText(Render::CRenderContext &ctx, CBox *barBox, const float scale, const float a);
  void damageOnButtonHover();
  void renderMenus(Render::CRenderContext &ctx, const CBox &barBox, float scale, float a, float startX, float endX);
  // The menu item under COORDS (bar-relative, logical), or -1.
  int menuAt(const Vector2D &coords);

  bool inputIsValid();
  void onMouseButton(Event::SCallbackInfo &info, IPointer::SButtonEvent e);
  void onTouchDown(Event::SCallbackInfo &info, ITouch::SDownEvent e);
  void onTouchUp(Event::SCallbackInfo &info, ITouch::SUpEvent e);
  void onMouseMove(Vector2D coords);
  void onTouchMove(Event::SCallbackInfo &info, ITouch::SMotionEvent e);

  void handleDownEvent(Event::SCallbackInfo &info, std::optional<ITouch::SDownEvent> touchEvent);
  void handleUpEvent(Event::SCallbackInfo &info);
  bool startTopEdgeResize(Event::SCallbackInfo &info, PHLWINDOW window, const Vector2D &coords);
  void handleMovement();
  void keepTitlebarReachable();
  bool doButtonPress(Config::INTEGER barPadding, Config::INTEGER barButtonPadding, Config::INTEGER barHeight, Vector2D COORDS, bool BUTTONSRIGHT);

  CBox assignedBoxGlobal();

  CHyprSignalListener m_pMouseButtonCallback;
  CHyprSignalListener m_pTouchDownCallback;
  CHyprSignalListener m_pTouchUpCallback;

  CHyprSignalListener m_pTouchMoveCallback;
  CHyprSignalListener m_pMouseMoveCallback;

  std::string m_szLastTitle;

  bool m_bDraggingThis = false;
  bool m_bTouchEv = false;
  bool m_bDragPending = false;
  bool m_bCancelledDown = false;
  int m_touchId = 0;

  // store hover state for buttons as a bitfield
  unsigned int m_iButtonHoverState = 0;

  // for dynamic updates
  int m_iLastHeight = 0;

  // The window's top-level menu items as last drawn: bar-relative logical x
  // and width, for hits and hover.
  struct SMenuLabel {
    int32_t id = 0;
    std::string label;
    bool enabled = true;
    float x = 0, w = 0;
    std::map<int, SP<Render::ITexture>> tex; // by monitor scale x100
  };
  std::vector<SMenuLabel> m_menuLabels;
  bool m_bMenusDirty = true;
  int m_iMenuHover = -1;

  size_t getVisibleButtonCount(Config::INTEGER barButtonPadding, Config::INTEGER barPadding, const Vector2D &bufferSize, const float scale);

  friend class COmarchyFloatingBarPassElement;
};
