// -*- mode: C++; indent-tabs-mode: nil; -*-
// blackbox.cc for Blackbox - an X11 Window manager
// Copyright (c) 2001 - 2002 Sean 'Shaleh' Perry <shaleh@debian.org>
// Copyright (c) 1997 - 2000 Brad Hughes (bhughes@tcac.net)
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

#ifdef    HAVE_CONFIG_H
#  include "../config.h"
#endif // HAVE_CONFIG_H

extern "C" {
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#ifdef    SHAPE
#include <X11/extensions/shape.h>
#endif // SHAPE

#ifdef    HAVE_STDIO_H
#  include <stdio.h>
#endif // HAVE_STDIO_H

#ifdef HAVE_STDLIB_H
#  include <stdlib.h>
#endif // HAVE_STDLIB_H

#ifdef HAVE_STRING_H
#  include <string.h>
#endif // HAVE_STRING_H

#ifdef    HAVE_UNISTD_H
#  include <sys/types.h>
#  include <unistd.h>
#endif // HAVE_UNISTD_H

#ifdef    HAVE_SYS_PARAM_H
#  include <sys/param.h>
#endif // HAVE_SYS_PARAM_H

#ifdef    HAVE_SYS_SELECT_H
#  include <sys/select.h>
#endif // HAVE_SYS_SELECT_H

#ifdef    HAVE_SIGNAL_H
#  include <signal.h>
#endif // HAVE_SIGNAL_H

#ifdef    HAVE_SYS_SIGNAL_H
#  include <sys/signal.h>
#endif // HAVE_SYS_SIGNAL_H

#ifdef    HAVE_SYS_STAT_H
#  include <sys/types.h>
#  include <sys/stat.h>
#endif // HAVE_SYS_STAT_H

#ifdef    TIME_WITH_SYS_TIME
#  include <sys/time.h>
#  include <time.h>
#else // !TIME_WITH_SYS_TIME
#  ifdef    HAVE_SYS_TIME_H
#    include <sys/time.h>
#  else // !HAVE_SYS_TIME_H
#    include <time.h>
#  endif // HAVE_SYS_TIME_H
#endif // TIME_WITH_SYS_TIME

#ifdef    HAVE_LIBGEN_H
#  include <libgen.h>
#endif // HAVE_LIBGEN_H
}

#include <algorithm>

#include "i18n.hh"
#include "blackbox.hh"
#include "Basemenu.hh"
#include "Clientmenu.hh"
#include "Image.hh"
#include "Rootmenu.hh"
#include "Screen.hh"
#include "Slit.hh"
#include "Toolbar.hh"
#include "Util.hh"
#include "Window.hh"
#include "Workspace.hh"
#include "Workspacemenu.hh"


// X event scanner for enter/leave notifies - adapted from twm
struct scanargs {
  Window w;
  Bool leave, inferior, enter;
};

static Bool queueScanner(Display *, XEvent *e, char *args) {
  scanargs *scan = (scanargs *) args;
  if ((e->type == LeaveNotify) &&
      (e->xcrossing.window == scan->w) &&
      (e->xcrossing.mode == NotifyNormal)) {
    scan->leave = True;
    scan->inferior = (e->xcrossing.detail == NotifyInferior);
  } else if ((e->type == EnterNotify) && (e->xcrossing.mode == NotifyUngrab)) {
    scan->enter = True;
  }

  return False;
}

Blackbox *blackbox;


Blackbox::Blackbox(int m_argc, char **m_argv, char *dpy_name, char *rc)
  : BaseDisplay(m_argv[0], dpy_name)
{
  if (! XSupportsLocale())
    fprintf(stderr, "X server does not support locale\n");

  if (XSetLocaleModifiers("") == NULL)
    fprintf(stderr, "cannot set locale modifiers\n");

  ::blackbox = this;
  argc = m_argc;
  argv = m_argv;
  if (! rc) rc = "~/.blackboxrc";
  rc_file = expandTilde(rc);

  no_focus = False;

  resource.auto_raise_delay.tv_sec = resource.auto_raise_delay.tv_usec = 0;

  focused_window = masked_window = (BlackboxWindow *) 0;
  masked = None;

  XrmInitialize();
  load_rc();

  init_icccm();

  cursor.session = XCreateFontCursor(getXDisplay(), XC_left_ptr);
  cursor.move = XCreateFontCursor(getXDisplay(), XC_fleur);
  cursor.ll_angle = XCreateFontCursor(getXDisplay(), XC_ll_angle);
  cursor.lr_angle = XCreateFontCursor(getXDisplay(), XC_lr_angle);

  for (unsigned int i = 0; i < getNumberOfScreens(); i++) {
    BScreen *screen = new BScreen(this, i);

    if (! screen->isScreenManaged()) {
      delete screen;
      continue;
    }

    screenList.push_back(screen);
  }

  if (screenList.empty()) {
    fprintf(stderr,
            i18n(blackboxSet, blackboxNoManagableScreens,
              "Blackbox::Blackbox: no managable screens found, aborting.\n"));
    ::exit(3);
  }

  XSynchronize(getXDisplay(), False);
  XSync(getXDisplay(), False);

  reconfigure_wait = reread_menu_wait = False;

  timer = new BTimer(this, this);
  timer->setTimeout(0l);
}


Blackbox::~Blackbox(void) {
  std::for_each(screenList.begin(), screenList.end(), PointerAssassin());

  std::for_each(menuTimestamps.begin(), menuTimestamps.end(),
                PointerAssassin());

  delete timer;
}


void Blackbox::process_event(XEvent *e) {
  switch (e->type) {
  case ButtonPress: {
    // strip the lock key modifiers
    e->xbutton.state &= ~(NumLockMask | ScrollLockMask | LockMask);

    last_time = e->xbutton.time;

    BlackboxWindow *win = (BlackboxWindow *) 0;
    Basemenu *menu = (Basemenu *) 0;
    Slit *slit = (Slit *) 0;
    Toolbar *tbar = (Toolbar *) 0;

    if ((win = searchWindow(e->xbutton.window))) {
      win->buttonPressEvent(&e->xbutton);

      /* XXX: is this sane on low colour desktops? */
      if (e->xbutton.button == 1)
        win->installColormap(True);
    } else if ((menu = searchMenu(e->xbutton.window))) {
      menu->buttonPressEvent(&e->xbutton);
    } else if ((slit = searchSlit(e->xbutton.window))) {
      slit->buttonPressEvent(&e->xbutton);
    } else if ((tbar = searchToolbar(e->xbutton.window))) {
      tbar->buttonPressEvent(&e->xbutton);
    } else {
      ScreenList::iterator it = screenList.begin();
      for (; it != screenList.end(); ++it) {
        if (e->xbutton.window == (*it)->getRootWindow()) {
          (*it)->buttonPressEvent(&e->xbutton);
          break;
        }
      }
    }
    break;
  }

  case ButtonRelease: {
    // strip the lock key modifiers
    e->xbutton.state &= ~(NumLockMask | ScrollLockMask | LockMask);

    last_time = e->xbutton.time;

    BlackboxWindow *win = (BlackboxWindow *) 0;
    Basemenu *menu = (Basemenu *) 0;
    Toolbar *tbar = (Toolbar *) 0;

    if ((win = searchWindow(e->xbutton.window)))
      win->buttonReleaseEvent(&e->xbutton);
    else if ((menu = searchMenu(e->xbutton.window)))
      menu->buttonReleaseEvent(&e->xbutton);
    else if ((tbar = searchToolbar(e->xbutton.window)))
      tbar->buttonReleaseEvent(&e->xbutton);

    break;
  }

  case ConfigureRequest: {
    // compress configure requests...
    XEvent realevent;
    unsigned int i = 0;
    while(XCheckTypedWindowEvent(getXDisplay(), e->xconfigurerequest.window,
                                 ConfigureRequest, &realevent)) {
      i++;
    }
    if ( i > 0 )
      e = &realevent;

    BlackboxWindow *win = (BlackboxWindow *) 0;
    Slit *slit = (Slit *) 0;

    if ((win = searchWindow(e->xconfigurerequest.window))) {
      win->configureRequestEvent(&e->xconfigurerequest);
    } else if ((slit = searchSlit(e->xconfigurerequest.window))) {
      slit->configureRequestEvent(&e->xconfigurerequest);
    } else {
      if (validateWindow(e->xconfigurerequest.window)) {
        XWindowChanges xwc;

        xwc.x = e->xconfigurerequest.x;
        xwc.y = e->xconfigurerequest.y;
        xwc.width = e->xconfigurerequest.width;
        xwc.height = e->xconfigurerequest.height;
        xwc.border_width = e->xconfigurerequest.border_width;
        xwc.sibling = e->xconfigurerequest.above;
        xwc.stack_mode = e->xconfigurerequest.detail;

        XConfigureWindow(getXDisplay(), e->xconfigurerequest.window,
                         e->xconfigurerequest.value_mask, &xwc);
      }
    }

    break;
  }

  case MapRequest: {
#ifdef    DEBUG
    fprintf(stderr,
            i18n(blackboxSet, blackboxMapRequest,
                 "Blackbox::process_event(): MapRequest for 0x%lx\n"),
            e->xmaprequest.window);
#endif // DEBUG

    BlackboxWindow *win = searchWindow(e->xmaprequest.window);

    if (! win) {
      BScreen *screen = searchScreen(e->xmaprequest.parent);
      if (screen)
        screen->manageWindow(e->xmaprequest.window);
    }

    break;
  }

  case MapNotify: {
    BlackboxWindow *win = searchWindow(e->xmap.window);

    if (win)
      win->mapNotifyEvent(&e->xmap);

    break;
  }

  case UnmapNotify: {
    BlackboxWindow *win = (BlackboxWindow *) 0;
    Slit *slit = (Slit *) 0;

    if ((win = searchWindow(e->xunmap.window))) {
      win->unmapNotifyEvent(&e->xunmap);
    } else if ((slit = searchSlit(e->xunmap.window))) {
      slit->removeClient(e->xunmap.window);
    }

    break;
  }

  case DestroyNotify: {
    BlackboxWindow *win = (BlackboxWindow *) 0;
    Slit *slit = (Slit *) 0;

    if ((win = searchWindow(e->xdestroywindow.window))) {
      win->destroyNotifyEvent(&e->xdestroywindow);
    } else if ((slit = searchSlit(e->xdestroywindow.window))) {
      slit->removeClient(e->xdestroywindow.window, False);
    }

    break;
  }

  // this event is quite rare and is usually handled in unmapNotify
  // however, if the window is unmapped when the reparent event occurs
  // the window manager never sees it because an unmap event is not sent
  // to an already unmapped window
  case ReparentNotify: {
    BlackboxWindow *win = searchWindow(e->xreparent.window);
    if (win) {
      win->reparentNotifyEvent(&e->xreparent);
    } else {
      Slit *slit = searchSlit(e->xreparent.window);
      if (slit)
        slit->removeClient(e->xreparent.window, True);
    }
    break;
  }

  case MotionNotify: {
    // motion notify compression...
    XEvent realevent;
    unsigned int i = 0;
    while (XCheckTypedWindowEvent(getXDisplay(), e->xmotion.window,
                                  MotionNotify, &realevent)) {
      i++;
    }

    // if we have compressed some motion events, use the last one
    if ( i > 0 )
      e = &realevent;

    // strip the lock key modifiers
    e->xbutton.state &= ~(NumLockMask | ScrollLockMask | LockMask);

    last_time = e->xmotion.time;

    BlackboxWindow *win = (BlackboxWindow *) 0;
    Basemenu *menu = (Basemenu *) 0;

    if ((win = searchWindow(e->xmotion.window)))
      win->motionNotifyEvent(&e->xmotion);
    else if ((menu = searchMenu(e->xmotion.window)))
      menu->motionNotifyEvent(&e->xmotion);

    break;
  }

  case PropertyNotify: {
    last_time = e->xproperty.time;

    if (e->xproperty.state != PropertyDelete) {
      BlackboxWindow *win = searchWindow(e->xproperty.window);

      if (win)
        win->propertyNotifyEvent(e->xproperty.atom);
    }

    break;
  }

  case EnterNotify: {
    last_time = e->xcrossing.time;

    BScreen *screen = (BScreen *) 0;
    BlackboxWindow *win = (BlackboxWindow *) 0;
    Basemenu *menu = (Basemenu *) 0;
    Toolbar *tbar = (Toolbar *) 0;
    Slit *slit = (Slit *) 0;

    if (e->xcrossing.mode == NotifyGrab) break;

    XEvent dummy;
    scanargs sa;
    sa.w = e->xcrossing.window;
    sa.enter = sa.leave = False;
    XCheckIfEvent(getXDisplay(), &dummy, queueScanner, (char *) &sa);

    if ((e->xcrossing.window == e->xcrossing.root) &&
        (screen = searchScreen(e->xcrossing.window))) {
      screen->getImageControl()->installRootColormap();
    } else if ((win = searchWindow(e->xcrossing.window))) {
      if (win->getScreen()->isSloppyFocus() &&
          (! win->isFocused()) && (! no_focus)) {
        if (((! sa.leave) || sa.inferior) && win->isVisible() &&
            win->setInputFocus())
          win->installColormap(True); // XXX: shouldnt we honour no install?
      }
    } else if ((menu = searchMenu(e->xcrossing.window))) {
      menu->enterNotifyEvent(&e->xcrossing);
    } else if ((tbar = searchToolbar(e->xcrossing.window))) {
      tbar->enterNotifyEvent(&e->xcrossing);
    } else if ((slit = searchSlit(e->xcrossing.window))) {
      slit->enterNotifyEvent(&e->xcrossing);
    }
    break;
  }

  case LeaveNotify: {
    last_time = e->xcrossing.time;

    BlackboxWindow *win = (BlackboxWindow *) 0;
    Basemenu *menu = (Basemenu *) 0;
    Toolbar *tbar = (Toolbar *) 0;
    Slit *slit = (Slit *) 0;

    if ((menu = searchMenu(e->xcrossing.window)))
      menu->leaveNotifyEvent(&e->xcrossing);
    else if ((win = searchWindow(e->xcrossing.window)))
      win->installColormap(False);
    else if ((tbar = searchToolbar(e->xcrossing.window)))
      tbar->leaveNotifyEvent(&e->xcrossing);
    else if ((slit = searchSlit(e->xcrossing.window)))
      slit->leaveNotifyEvent(&e->xcrossing);
    break;
  }

  case Expose: {
    // compress expose events
    XEvent realevent;
    unsigned int i = 0;
    int  ex1, ey1, ex2, ey2;
    ex1 = e->xexpose.x;
    ey1 = e->xexpose.y;
    ex2 = ex1 + e->xexpose.width - 1;
    ey2 = ey1 + e->xexpose.height - 1;
    while (XCheckTypedWindowEvent(getXDisplay(), e->xexpose.window,
                                  Expose, &realevent)) {
      i++;

      // merge expose area
      ex1 = std::min(realevent.xexpose.x, ex1);
      ey1 = std::min(realevent.xexpose.y, ey1);
      ex2 = std::max(realevent.xexpose.x + realevent.xexpose.width - 1,
                     ex2);
      ey2 = std::max(realevent.xexpose.y + realevent.xexpose.height - 1,
                     ey2);
    }
    if ( i > 0 )
      e = &realevent;

    // use the merged area
    e->xexpose.x = ex1;
    e->xexpose.y = ey1;
    e->xexpose.width = ex2 - ex1 + 1;
    e->xexpose.height = ey2 - ey1 + 1;

    BlackboxWindow *win = (BlackboxWindow *) 0;
    Basemenu *menu = (Basemenu *) 0;
    Toolbar *tbar = (Toolbar *) 0;

    if ((win = searchWindow(e->xexpose.window)))
      win->exposeEvent(&e->xexpose);
    else if ((menu = searchMenu(e->xexpose.window)))
      menu->exposeEvent(&e->xexpose);
    else if ((tbar = searchToolbar(e->xexpose.window)))
      tbar->exposeEvent(&e->xexpose);

    break;
  }

  case KeyPress: {
    Toolbar *tbar = searchToolbar(e->xkey.window);

    if (tbar && tbar->isEditing())
      tbar->keyPressEvent(&e->xkey);

    break;
  }

  case ColormapNotify: {
    BScreen *screen = searchScreen(e->xcolormap.window);

    if (screen)
      screen->setRootColormapInstalled((e->xcolormap.state ==
                                        ColormapInstalled) ? True : False);

    break;
  }

  case FocusIn: {
    if (e->xfocus.mode == NotifyUngrab || e->xfocus.detail == NotifyPointer)
      break;

    BlackboxWindow *win = searchWindow(e->xfocus.window);
    if (win && ! win->isFocused())
      setFocusedWindow(win);

    break;
  }

  case ClientMessage: {
    if (e->xclient.format == 32) {
      if (e->xclient.message_type == getWMChangeStateAtom()) {
        BlackboxWindow *win = searchWindow(e->xclient.window);
        if (! win || ! win->validateClient()) return;

        if (e->xclient.data.l[0] == IconicState)
          win->iconify();
        if (e->xclient.data.l[0] == NormalState)
          win->deiconify();
      } else if(e->xclient.message_type == getBlackboxChangeWorkspaceAtom()) {
        BScreen *screen = searchScreen(e->xclient.window);

        if (screen && e->xclient.data.l[0] >= 0 &&
            e->xclient.data.l[0] < (signed)screen->getWorkspaceCount())
          screen->changeWorkspaceID(e->xclient.data.l[0]);
      } else if (e->xclient.message_type == getBlackboxChangeWindowFocusAtom()) {
        BlackboxWindow *win = searchWindow(e->xclient.window);

        if (win && win->isVisible() && win->setInputFocus())
          win->installColormap(True);
      } else if (e->xclient.message_type == getBlackboxCycleWindowFocusAtom()) {
        BScreen *screen = searchScreen(e->xclient.window);

        if (screen) {
          if (! e->xclient.data.l[0])
            screen->prevFocus();
          else
            screen->nextFocus();
        }
      } else if (e->xclient.message_type == getBlackboxChangeAttributesAtom()) {
        BlackboxWindow *win = searchWindow(e->xclient.window);

        if (win && win->validateClient()) {
          BlackboxHints net;
          net.flags = e->xclient.data.l[0];
          net.attrib = e->xclient.data.l[1];
          net.workspace = e->xclient.data.l[2];
          net.stack = e->xclient.data.l[3];
          net.decoration = e->xclient.data.l[4];

          win->changeBlackboxHints(&net);
        }
      }
    }

    break;
  }

  case FocusOut:
  case NoExpose:
  case ConfigureNotify:
    break; // not handled, just ignore

  default: {
#ifdef    SHAPE
    if (e->type == getShapeEventBase()) {
      XShapeEvent *shape_event = (XShapeEvent *) e;
      BlackboxWindow *win = (BlackboxWindow *) 0;

      if ((win = searchWindow(e->xany.window)) ||
          (shape_event->kind != ShapeBounding))
        win->shapeEvent(shape_event);
    }
#endif // SHAPE
  }
  } // switch
}


Bool Blackbox::handleSignal(int sig) {
  switch (sig) {
  case SIGHUP:
    reconfigure();
    break;

  case SIGUSR1:
    reload_rc();
    break;

  case SIGUSR2:
    rereadMenu();
    break;

  case SIGPIPE:
  case SIGSEGV:
  case SIGFPE:
  case SIGINT:
  case SIGTERM:
    shutdown();

  default:
    return False;
  }

  return True;
}


void Blackbox::init_icccm(void) {
  xa_wm_colormap_windows =
    XInternAtom(getXDisplay(), "WM_COLORMAP_WINDOWS", False);
  xa_wm_protocols = XInternAtom(getXDisplay(), "WM_PROTOCOLS", False);
  xa_wm_state = XInternAtom(getXDisplay(), "WM_STATE", False);
  xa_wm_change_state = XInternAtom(getXDisplay(), "WM_CHANGE_STATE", False);
  xa_wm_delete_window = XInternAtom(getXDisplay(), "WM_DELETE_WINDOW", False);
  xa_wm_take_focus = XInternAtom(getXDisplay(), "WM_TAKE_FOCUS", False);
  motif_wm_hints = XInternAtom(getXDisplay(), "_MOTIF_WM_HINTS", False);

  blackbox_hints = XInternAtom(getXDisplay(), "_BLACKBOX_HINTS", False);
  blackbox_attributes =
    XInternAtom(getXDisplay(), "_BLACKBOX_ATTRIBUTES", False);
  blackbox_change_attributes =
    XInternAtom(getXDisplay(), "_BLACKBOX_CHANGE_ATTRIBUTES", False);
  blackbox_structure_messages =
    XInternAtom(getXDisplay(), "_BLACKBOX_STRUCTURE_MESSAGES", False);
  blackbox_notify_startup =
    XInternAtom(getXDisplay(), "_BLACKBOX_NOTIFY_STARTUP", False);
  blackbox_notify_window_add =
    XInternAtom(getXDisplay(), "_BLACKBOX_NOTIFY_WINDOW_ADD", False);
  blackbox_notify_window_del =
    XInternAtom(getXDisplay(), "_BLACKBOX_NOTIFY_WINDOW_DEL", False);
  blackbox_notify_current_workspace =
    XInternAtom(getXDisplay(), "_BLACKBOX_NOTIFY_CURRENT_WORKSPACE", False);
  blackbox_notify_workspace_count =
    XInternAtom(getXDisplay(), "_BLACKBOX_NOTIFY_WORKSPACE_COUNT", False);
  blackbox_notify_window_focus =
    XInternAtom(getXDisplay(), "_BLACKBOX_NOTIFY_WINDOW_FOCUS", False);
  blackbox_notify_window_raise =
    XInternAtom(getXDisplay(), "_BLACKBOX_NOTIFY_WINDOW_RAISE", False);
  blackbox_notify_window_lower =
    XInternAtom(getXDisplay(), "_BLACKBOX_NOTIFY_WINDOW_LOWER", False);
  blackbox_change_workspace =
    XInternAtom(getXDisplay(), "_BLACKBOX_CHANGE_WORKSPACE", False);
  blackbox_change_window_focus =
    XInternAtom(getXDisplay(), "_BLACKBOX_CHANGE_WINDOW_FOCUS", False);
  blackbox_cycle_window_focus =
    XInternAtom(getXDisplay(), "_BLACKBOX_CYCLE_WINDOW_FOCUS", False);

#ifdef    NEWWMSPEC
  net_supported = XInternAtom(getXDisplay(), "_NET_SUPPORTED", False);
  net_client_list = XInternAtom(getXDisplay(), "_NET_CLIENT_LIST", False);
  net_client_list_stacking =
    XInternAtom(getXDisplay(), "_NET_CLIENT_LIST_STACKING", False);
  net_number_of_desktops =
    XInternAtom(getXDisplay(), "_NET_NUMBER_OF_DESKTOPS", False);
  net_desktop_geometry =
    XInternAtom(getXDisplay(), "_NET_DESKTOP_GEOMETRY", False);
  net_desktop_viewport =
    XInternAtom(getXDisplay(), "_NET_DESKTOP_VIEWPORT", False);
  net_current_desktop =
    XInternAtom(getXDisplay(), "_NET_CURRENT_DESKTOP", False);
  net_desktop_names = XInternAtom(getXDisplay(), "_NET_DESKTOP_NAMES", False);
  net_active_window = XInternAtom(getXDisplay(), "_NET_ACTIVE_WINDOW", False);
  net_workarea = XInternAtom(getXDisplay(), "_NET_WORKAREA", False);
  net_supporting_wm_check =
    XInternAtom(getXDisplay(), "_NET_SUPPORTING_WM_CHECK", False);
  net_virtual_roots = XInternAtom(getXDisplay(), "_NET_VIRTUAL_ROOTS", False);
  net_close_window = XInternAtom(getXDisplay(), "_NET_CLOSE_WINDOW", False);
  net_wm_moveresize = XInternAtom(getXDisplay(), "_NET_WM_MOVERESIZE", False);
  net_properties = XInternAtom(getXDisplay(), "_NET_PROPERTIES", False);
  net_wm_name = XInternAtom(getXDisplay(), "_NET_WM_NAME", False);
  net_wm_desktop = XInternAtom(getXDisplay(), "_NET_WM_DESKTOP", False);
  net_wm_window_type =
    XInternAtom(getXDisplay(), "_NET_WM_WINDOW_TYPE", False);
  net_wm_state = XInternAtom(getXDisplay(), "_NET_WM_STATE", False);
  net_wm_strut = XInternAtom(getXDisplay(), "_NET_WM_STRUT", False);
  net_wm_icon_geometry =
    XInternAtom(getXDisplay(), "_NET_WM_ICON_GEOMETRY", False);
  net_wm_icon = XInternAtom(getXDisplay(), "_NET_WM_ICON", False);
  net_wm_pid = XInternAtom(getXDisplay(), "_NET_WM_PID", False);
  net_wm_handled_icons =
    XInternAtom(getXDisplay(), "_NET_WM_HANDLED_ICONS", False);
  net_wm_ping = XInternAtom(getXDisplay(), "_NET_WM_PING", False);
#endif // NEWWMSPEC

#ifdef    HAVE_GETPID
  blackbox_pid = XInternAtom(getXDisplay(), "_BLACKBOX_PID", False);
#endif // HAVE_GETPID
}


Bool Blackbox::validateWindow(Window window) {
  XEvent event;
  if (XCheckTypedWindowEvent(getXDisplay(), window, DestroyNotify, &event)) {
    XPutBackEvent(getXDisplay(), &event);

    return False;
  }

  return True;
}


BScreen *Blackbox::searchScreen(Window window) {
  ScreenList::iterator it = screenList.begin();

  for (; it != screenList.end(); ++it) {
    BScreen *s = *it;
    if (s->getRootWindow() == window)
      return s;
  }

  return (BScreen *) 0;
}


BlackboxWindow *Blackbox::searchWindow(Window window) {
  WindowLookup::iterator it = windowSearchList.find(window);
  if (it == windowSearchList.end())
    return (BlackboxWindow*) 0;

  return it->second;
}


BlackboxWindow *Blackbox::searchGroup(Window window, BlackboxWindow *win) {
  WindowLookup::iterator it = groupSearchList.find(window);
  if (it != groupSearchList.end()) {
    if (it->second->getClientWindow() != win->getClientWindow())
      return win;
  }
  return (BlackboxWindow*) 0;
}


Basemenu *Blackbox::searchMenu(Window window) {
  MenuLookup::iterator it = menuSearchList.find(window);
  if (it == menuSearchList.end())
    return (Basemenu*) 0;

  return it->second;
}


Toolbar *Blackbox::searchToolbar(Window window) {
  ToolbarLookup::iterator it = toolbarSearchList.find(window);
  if (it == toolbarSearchList.end())
    return (Toolbar*) 0;

  return it->second;
}


Slit *Blackbox::searchSlit(Window window) {
  SlitLookup::iterator it = slitSearchList.find(window);
  if (it == slitSearchList.end())
    return (Slit*) 0;

  return it->second;
}


void Blackbox::saveWindowSearch(Window window, BlackboxWindow *data) {
  windowSearchList.insert(WindowLookupPair(window, data));
}


void Blackbox::saveGroupSearch(Window window, BlackboxWindow *data) {
  groupSearchList.insert(WindowLookupPair(window, data));
}


void Blackbox::saveMenuSearch(Window window, Basemenu *data) {
  menuSearchList.insert(MenuLookupPair(window, data));
}


void Blackbox::saveToolbarSearch(Window window, Toolbar *data) {
  toolbarSearchList.insert(ToolbarLookupPair(window, data));
}


void Blackbox::saveSlitSearch(Window window, Slit *data) {
  slitSearchList.insert(SlitLookupPair(window, data));
}


void Blackbox::removeWindowSearch(Window window) {
  windowSearchList.erase(window);
}


void Blackbox::removeGroupSearch(Window window) {
  groupSearchList.erase(window);
}


void Blackbox::removeMenuSearch(Window window) {
  menuSearchList.erase(window);
}


void Blackbox::removeToolbarSearch(Window window) {
  toolbarSearchList.erase(window);
}


void Blackbox::removeSlitSearch(Window window) {
  slitSearchList.erase(window);
}


void Blackbox::restart(const char *prog) {
  shutdown();

  if (prog) {
    execlp(prog, prog, NULL);
    perror(prog);
  }

  // fall back in case the above execlp doesn't work
  execvp(argv[0], argv);
  execvp(basename(argv[0]), argv);
}


void Blackbox::shutdown(void) {
  BaseDisplay::shutdown();

  XSetInputFocus(getXDisplay(), PointerRoot, None, CurrentTime);

  std::for_each(screenList.begin(), screenList.end(),
                std::mem_fun(&BScreen::shutdown));

  XSync(getXDisplay(), False);

  save_rc();
}


void Blackbox::save_rc(void) {
  XrmDatabase new_blackboxrc = (XrmDatabase) 0;
  char rc_string[1024];

  load_rc();

  sprintf(rc_string, "session.menuFile:  %s", getMenuFilename());
  XrmPutLineResource(&new_blackboxrc, rc_string);

  sprintf(rc_string, "session.colorsPerChannel:  %d",
          resource.colors_per_channel);
  XrmPutLineResource(&new_blackboxrc, rc_string);

  sprintf(rc_string, "session.doubleClickInterval:  %lu",
          resource.double_click_interval);
  XrmPutLineResource(&new_blackboxrc, rc_string);

  sprintf(rc_string, "session.autoRaiseDelay:  %lu",
          ((resource.auto_raise_delay.tv_sec * 1000) +
           (resource.auto_raise_delay.tv_usec / 1000)));
  XrmPutLineResource(&new_blackboxrc, rc_string);

  sprintf(rc_string, "session.cacheLife: %lu", resource.cache_life / 60000);
  XrmPutLineResource(&new_blackboxrc, rc_string);

  sprintf(rc_string, "session.cacheMax: %lu", resource.cache_max);
  XrmPutLineResource(&new_blackboxrc, rc_string);

  ScreenList::iterator it = screenList.begin();
  for (; it != screenList.end(); ++it) {
    BScreen *screen = *it;
    int screen_number = screen->getScreenNumber();

    char *placement = (char *) 0;

    switch (screen->getSlitPlacement()) {
    case Slit::TopLeft: placement = "TopLeft"; break;
    case Slit::CenterLeft: placement = "CenterLeft"; break;
    case Slit::BottomLeft: placement = "BottomLeft"; break;
    case Slit::TopCenter: placement = "TopCenter"; break;
    case Slit::BottomCenter: placement = "BottomCenter"; break;
    case Slit::TopRight: placement = "TopRight"; break;
    case Slit::BottomRight: placement = "BottomRight"; break;
    case Slit::CenterRight: default: placement = "CenterRight"; break;
    }

    sprintf(rc_string, "session.screen%d.slit.placement: %s", screen_number,
            placement);
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.slit.direction: %s", screen_number,
            ((screen->getSlitDirection() == Slit::Horizontal) ? "Horizontal" :
             "Vertical"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.slit.onTop: %s", screen_number,
            ((screen->getSlit()->isOnTop()) ? "True" : "False"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.slit.autoHide: %s", screen_number,
            ((screen->getSlit()->doAutoHide()) ? "True" : "False"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.opaqueMove: %s",
            ((screen->doOpaqueMove()) ? "True" : "False"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.imageDither: %s",
            ((screen->getImageControl()->doDither()) ? "True" : "False"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.fullMaximization: %s", screen_number,
            ((screen->doFullMax()) ? "True" : "False"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.focusNewWindows: %s", screen_number,
            ((screen->doFocusNew()) ? "True" : "False"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.focusLastWindow: %s", screen_number,
            ((screen->doFocusLast()) ? "True" : "False"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.rowPlacementDirection: %s",
            screen_number,
            ((screen->getRowPlacementDirection() == BScreen::LeftRight) ?
             "LeftToRight" : "RightToLeft"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.colPlacementDirection: %s",
            screen_number,
            ((screen->getColPlacementDirection() == BScreen::TopBottom) ?
             "TopToBottom" : "BottomToTop"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    switch (screen->getPlacementPolicy()) {
    case BScreen::CascadePlacement:
      placement = "CascadePlacement";
      break;

    case BScreen::ColSmartPlacement:
      placement = "ColSmartPlacement";
      break;

    case BScreen::RowSmartPlacement:
    default:
      placement = "RowSmartPlacement";
      break;
    }
    sprintf(rc_string, "session.screen%d.windowPlacement:  %s", screen_number,
            placement);
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.focusModel:  %s", screen_number,
            ((screen->isSloppyFocus()) ?
             ((screen->doAutoRaise()) ? "AutoRaiseSloppyFocus" :
              "SloppyFocus") :
             "ClickToFocus"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.workspaces:  %d", screen_number,
            screen->getWorkspaceCount());
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.toolbar.onTop:  %s", screen_number,
            ((screen->getToolbar()->isOnTop()) ? "True" : "False"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.toolbar.autoHide:  %s",
            screen_number,
            ((screen->getToolbar()->doAutoHide()) ? "True" : "False"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    switch (screen->getToolbarPlacement()) {
    case Toolbar::TopLeft: placement = "TopLeft"; break;
    case Toolbar::BottomLeft: placement = "BottomLeft"; break;
    case Toolbar::TopCenter: placement = "TopCenter"; break;
    case Toolbar::TopRight: placement = "TopRight"; break;
    case Toolbar::BottomRight: placement = "BottomRight"; break;
    case Toolbar::BottomCenter: default:
      placement = "BottomCenter"; break;
    }

    sprintf(rc_string, "session.screen%d.toolbar.placement: %s",
            screen_number,
            placement);
    XrmPutLineResource(&new_blackboxrc, rc_string);

    load_rc(screen);

    // these are static, but may not be saved in the users .blackboxrc,
    // writing these resources will allow the user to edit them at a later
    // time... but loading the defaults before saving allows us to rewrite the
    // users changes...

#ifdef    HAVE_STRFTIME
    sprintf(rc_string, "session.screen%d.strftimeFormat: %s", screen_number,
            screen->getStrftimeFormat());
    XrmPutLineResource(&new_blackboxrc, rc_string);
#else // !HAVE_STRFTIME
    sprintf(rc_string, "session.screen%d.dateFormat:  %s", screen_number,
            ((screen->getDateFormat() == B_EuropeanDate) ?
             "European" : "American"));
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.clockFormat:  %d", screen_number,
            ((screen->isClock24Hour()) ? 24 : 12));
    XrmPutLineResource(&new_blackboxrc, rc_string);
#endif // HAVE_STRFTIME

    sprintf(rc_string, "session.screen%d.edgeSnapThreshold: %d",
            screen_number, screen->getEdgeSnapThreshold());
    XrmPutLineResource(&new_blackboxrc, rc_string);

    sprintf(rc_string, "session.screen%d.toolbar.widthPercent:  %d",
            screen_number, screen->getToolbarWidthPercent());
    XrmPutLineResource(&new_blackboxrc, rc_string);

    // write out the user's workspace names

    string save_string = screen->getWorkspace(0)->getName();
    for (unsigned int i = 1; i < screen->getWorkspaceCount(); ++i) {
      save_string += ',';
      save_string += screen->getWorkspace(i)->getName();
    }

    char *resource_string = new char[save_string.length() + 48];
    sprintf(resource_string, "session.screen%d.workspaceNames:  %s",
            screen_number, save_string.c_str());
    XrmPutLineResource(&new_blackboxrc, resource_string);

    delete [] resource_string;
  }

  XrmDatabase old_blackboxrc = XrmGetFileDatabase(rc_file.c_str());

  XrmMergeDatabases(new_blackboxrc, &old_blackboxrc);
  XrmPutFileDatabase(old_blackboxrc, rc_file.c_str());
  XrmDestroyDatabase(old_blackboxrc);
}


void Blackbox::load_rc(void) {
  XrmDatabase database = (XrmDatabase) 0;

  database = XrmGetFileDatabase(rc_file.c_str());

  XrmValue value;
  char *value_type;

  if (XrmGetResource(database, "session.menuFile", "Session.MenuFile",
                     &value_type, &value)) {
    resource.menu_file = expandTilde(value.addr);
  } else {
    resource.menu_file = DEFAULTMENU;
  }

  if (XrmGetResource(database, "session.colorsPerChannel",
                     "Session.ColorsPerChannel", &value_type, &value)) {
    if (sscanf(value.addr, "%d", &resource.colors_per_channel) != 1) {
      resource.colors_per_channel = 4;
    } else {
      if (resource.colors_per_channel < 2) resource.colors_per_channel = 2;
      if (resource.colors_per_channel > 6) resource.colors_per_channel = 6;
    }
  } else {
    resource.colors_per_channel = 4;
  }

  if (XrmGetResource(database, "session.styleFile", "Session.StyleFile",
                     &value_type, &value))
    resource.style_file = expandTilde(value.addr);
  else
    resource.style_file = DEFAULTSTYLE;

  if (XrmGetResource(database, "session.doubleClickInterval",
                     "Session.DoubleClickInterval", &value_type, &value)) {
    if (sscanf(value.addr, "%lu", &resource.double_click_interval) != 1)
      resource.double_click_interval = 250;
  } else {
    resource.double_click_interval = 250;
  }

  if (XrmGetResource(database, "session.autoRaiseDelay",
                     "Session.AutoRaiseDelay", &value_type, &value)) {
    if (sscanf(value.addr, "%ld", &resource.auto_raise_delay.tv_usec) != 1)
      resource.auto_raise_delay.tv_usec = 400;
  } else {
    resource.auto_raise_delay.tv_usec = 400;
  }

  resource.auto_raise_delay.tv_sec = resource.auto_raise_delay.tv_usec / 1000;
  resource.auto_raise_delay.tv_usec -=
    (resource.auto_raise_delay.tv_sec * 1000);
  resource.auto_raise_delay.tv_usec *= 1000;

  if (XrmGetResource(database, "session.cacheLife", "Session.CacheLife",
                     &value_type, &value)) {
    if (sscanf(value.addr, "%lu", &resource.cache_life) != 1)
      resource.cache_life = 5l;
  } else {
    resource.cache_life = 5l;
  }

  resource.cache_life *= 60000;

  if (XrmGetResource(database, "session.cacheMax", "Session.CacheMax",
                     &value_type, &value)) {
    if (sscanf(value.addr, "%lu", &resource.cache_max) != 1)
      resource.cache_max = 200;
  } else {
    resource.cache_max = 200;
  }
}


void Blackbox::load_rc(BScreen *screen) {
  XrmDatabase database = (XrmDatabase) 0;

  database = XrmGetFileDatabase(rc_file.c_str());

  XrmValue value;
  char *value_type, name_lookup[1024], class_lookup[1024];
  int screen_number = screen->getScreenNumber();

  sprintf(name_lookup,  "session.screen%d.fullMaximization", screen_number);
  sprintf(class_lookup, "Session.Screen%d.FullMaximization", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "true", value.size))
      screen->saveFullMax(True);
    else
      screen->saveFullMax(False);
  } else {
    screen->saveFullMax(False);
  }
  sprintf(name_lookup,  "session.screen%d.focusNewWindows", screen_number);
  sprintf(class_lookup, "Session.Screen%d.FocusNewWindows", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "true", value.size))
      screen->saveFocusNew(True);
    else
      screen->saveFocusNew(False);
  } else {
    screen->saveFocusNew(False);
  }
  sprintf(name_lookup,  "session.screen%d.focusLastWindow", screen_number);
  sprintf(class_lookup, "Session.Screen%d.focusLastWindow", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "true", value.size))
      screen->saveFocusLast(True);
    else
      screen->saveFocusLast(False);
  } else {
    screen->saveFocusLast(False);
  }
  sprintf(name_lookup,  "session.screen%d.rowPlacementDirection",
          screen_number);
  sprintf(class_lookup, "Session.Screen%d.RowPlacementDirection",
          screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "righttoleft", value.size))
      screen->saveRowPlacementDirection(BScreen::RightLeft);
    else
      screen->saveRowPlacementDirection(BScreen::LeftRight);
  } else {
    screen->saveRowPlacementDirection(BScreen::LeftRight);
  }
  sprintf(name_lookup,  "session.screen%d.colPlacementDirection",
          screen_number);
  sprintf(class_lookup, "Session.Screen%d.ColPlacementDirection",
          screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "bottomtotop", value.size))
      screen->saveColPlacementDirection(BScreen::BottomTop);
    else
      screen->saveColPlacementDirection(BScreen::TopBottom);
  } else {
    screen->saveColPlacementDirection(BScreen::TopBottom);
  }
  sprintf(name_lookup,  "session.screen%d.workspaces", screen_number);
  sprintf(class_lookup, "Session.Screen%d.Workspaces", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    int workspace_count;
    if (sscanf(value.addr, "%d", &workspace_count) != 1)
      workspace_count = 1;
    else if (workspace_count <= 0 || workspace_count > 128)
      workspace_count = 1;
    screen->saveWorkspaces(workspace_count);
  } else {
    screen->saveWorkspaces(1);
  }
  sprintf(name_lookup,  "session.screen%d.toolbar.widthPercent",
          screen_number);
  sprintf(class_lookup, "Session.Screen%d.Toolbar.WidthPercent",
          screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    int i;
    if (sscanf(value.addr, "%d", &i) != 1)
      i = 66;
    else if (i <= 0 || i > 100)
      i = 66;

    screen->saveToolbarWidthPercent(i);
  } else {
    screen->saveToolbarWidthPercent(66);
  }
  sprintf(name_lookup, "session.screen%d.toolbar.placement", screen_number);
  sprintf(class_lookup, "Session.Screen%d.Toolbar.Placement", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "TopLeft", value.size))
      screen->saveToolbarPlacement(Toolbar::TopLeft);
    else if (! strncasecmp(value.addr, "BottomLeft", value.size))
      screen->saveToolbarPlacement(Toolbar::BottomLeft);
    else if (! strncasecmp(value.addr, "TopCenter", value.size))
      screen->saveToolbarPlacement(Toolbar::TopCenter);
    else if (! strncasecmp(value.addr, "TopRight", value.size))
      screen->saveToolbarPlacement(Toolbar::TopRight);
    else if (! strncasecmp(value.addr, "BottomRight", value.size))
      screen->saveToolbarPlacement(Toolbar::BottomRight);
    else
      screen->saveToolbarPlacement(Toolbar::BottomCenter);
  } else {
    screen->saveToolbarPlacement(Toolbar::BottomCenter);
  }
  screen->removeWorkspaceNames();

  sprintf(name_lookup,  "session.screen%d.workspaceNames", screen_number);
  sprintf(class_lookup, "Session.Screen%d.WorkspaceNames", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    string search = value.addr;
    string::const_iterator it = search.begin(),
      end = search.end();
    while(1) {
      string::const_iterator tmp = it; // current string.begin()
      it = std::find(tmp, end, ',');   // look for comma between tmp and end
      string s(tmp, it);               // s = search[tmp:it]
      screen->addWorkspaceName(s);
      if (it == end) break;
      ++it;
    }
  }

  sprintf(name_lookup,  "session.screen%d.toolbar.onTop", screen_number);
  sprintf(class_lookup, "Session.Screen%d.Toolbar.OnTop", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "true", value.size))
      screen->saveToolbarOnTop(True);
    else
      screen->saveToolbarOnTop(False);
  } else {
    screen->saveToolbarOnTop(False);
  }
  sprintf(name_lookup,  "session.screen%d.toolbar.autoHide", screen_number);
  sprintf(class_lookup, "Session.Screen%d.Toolbar.autoHide", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "true", value.size))
      screen->saveToolbarAutoHide(True);
    else
      screen->saveToolbarAutoHide(False);
  } else {
    screen->saveToolbarAutoHide(False);
  }
  sprintf(name_lookup,  "session.screen%d.focusModel", screen_number);
  sprintf(class_lookup, "Session.Screen%d.FocusModel", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "clicktofocus", value.size)) {
      screen->saveAutoRaise(False);
      screen->saveSloppyFocus(False);
    } else if(! strncasecmp(value.addr, "autoraisesloppyfocus", value.size)) {
      screen->saveSloppyFocus(True);
      screen->saveAutoRaise(True);
    } else {
      screen->saveSloppyFocus(True);
      screen->saveAutoRaise(False);
    }
  } else {
    screen->saveSloppyFocus(True);
    screen->saveAutoRaise(False);
  }

  sprintf(name_lookup,  "session.screen%d.windowPlacement", screen_number);
  sprintf(class_lookup, "Session.Screen%d.WindowPlacement", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "RowSmartPlacement", value.size))
      screen->savePlacementPolicy(BScreen::RowSmartPlacement);
    else if (! strncasecmp(value.addr, "ColSmartPlacement", value.size))
      screen->savePlacementPolicy(BScreen::ColSmartPlacement);
    else
      screen->savePlacementPolicy(BScreen::CascadePlacement);
  } else {
    screen->savePlacementPolicy(BScreen::RowSmartPlacement);
  }

  sprintf(name_lookup, "session.screen%d.slit.placement", screen_number);
  sprintf(class_lookup, "Session.Screen%d.Slit.Placement", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "TopLeft", value.size))
      screen->saveSlitPlacement(Slit::TopLeft);
    else if (! strncasecmp(value.addr, "CenterLeft", value.size))
      screen->saveSlitPlacement(Slit::CenterLeft);
    else if (! strncasecmp(value.addr, "BottomLeft", value.size))
      screen->saveSlitPlacement(Slit::BottomLeft);
    else if (! strncasecmp(value.addr, "TopCenter", value.size))
      screen->saveSlitPlacement(Slit::TopCenter);
    else if (! strncasecmp(value.addr, "BottomCenter", value.size))
      screen->saveSlitPlacement(Slit::BottomCenter);
    else if (! strncasecmp(value.addr, "TopRight", value.size))
      screen->saveSlitPlacement(Slit::TopRight);
    else if (! strncasecmp(value.addr, "BottomRight", value.size))
      screen->saveSlitPlacement(Slit::BottomRight);
    else
      screen->saveSlitPlacement(Slit::CenterRight);
  } else {
    screen->saveSlitPlacement(Slit::CenterRight);
  }
  sprintf(name_lookup, "session.screen%d.slit.direction", screen_number);
  sprintf(class_lookup, "Session.Screen%d.Slit.Direction", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "Horizontal", value.size))
      screen->saveSlitDirection(Slit::Horizontal);
    else
      screen->saveSlitDirection(Slit::Vertical);
  } else {
    screen->saveSlitDirection(Slit::Vertical);
  }
  sprintf(name_lookup, "session.screen%d.slit.onTop", screen_number);
  sprintf(class_lookup, "Session.Screen%d.Slit.OnTop", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "True", value.size))
      screen->saveSlitOnTop(True);
    else
      screen->saveSlitOnTop(False);
  } else {
    screen->saveSlitOnTop(False);
  }
  sprintf(name_lookup, "session.screen%d.slit.autoHide", screen_number);
  sprintf(class_lookup, "Session.Screen%d.Slit.AutoHide", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (! strncasecmp(value.addr, "true", value.size))
      screen->saveSlitAutoHide(True);
    else
      screen->saveSlitAutoHide(False);
  } else {
    screen->saveSlitAutoHide(False);
  }

#ifdef    HAVE_STRFTIME
  sprintf(name_lookup,  "session.screen%d.strftimeFormat", screen_number);
  sprintf(class_lookup, "Session.Screen%d.StrftimeFormat", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    screen->saveStrftimeFormat(value.addr);
  } else {
    screen->saveStrftimeFormat("%I:%M %p");
  }
#else //  HAVE_STRFTIME
  sprintf(name_lookup,  "session.screen%d.dateFormat", screen_number);
  sprintf(class_lookup, "Session.Screen%d.DateFormat", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    if (strncasecmp(value.addr, "european", value.size))
      screen->saveDateFormat(B_AmericanDate);
    else
      screen->saveDateFormat(B_EuropeanDate);
  } else {
    screen->saveDateFormat(B_AmericanDate);
  }
  sprintf(name_lookup,  "session.screen%d.clockFormat", screen_number);
  sprintf(class_lookup, "Session.Screen%d.ClockFormat", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    int clock;
    if (sscanf(value.addr, "%d", &clock) != 1) screen->saveClock24Hour(False);
    else if (clock == 24) screen->saveClock24Hour(True);
    else screen->saveClock24Hour(False);
  } else {
    screen->saveClock24Hour(False);
  }
#endif // HAVE_STRFTIME

  sprintf(name_lookup,  "session.screen%d.edgeSnapThreshold", screen_number);
  sprintf(class_lookup, "Session.Screen%d.EdgeSnapThreshold", screen_number);
  if (XrmGetResource(database, name_lookup, class_lookup, &value_type,
                     &value)) {
    int threshold;
    if (sscanf(value.addr, "%d", &threshold) != 1)
      screen->saveEdgeSnapThreshold(0);
    else
      screen->saveEdgeSnapThreshold(threshold);
  } else {
    screen->saveEdgeSnapThreshold(0);
  }
  sprintf(name_lookup,  "session.screen%d.imageDither", screen_number);
  sprintf(class_lookup, "Session.Screen%d.ImageDither", screen_number);
  if (XrmGetResource(database, "session.imageDither", "Session.ImageDither",
                     &value_type, &value)) {
    if (! strncasecmp("true", value.addr, value.size))
      screen->saveImageDither(True);
    else
      screen->saveImageDither(False);
  } else {
    screen->saveImageDither(True);
  }

  if (XrmGetResource(database, "session.opaqueMove", "Session.OpaqueMove",
                     &value_type, &value)) {
    if (! strncasecmp("true", value.addr, value.size))
      screen->saveOpaqueMove(True);
    else
      screen->saveOpaqueMove(False);
  } else {
    screen->saveOpaqueMove(False);
  }
  XrmDestroyDatabase(database);
}


void Blackbox::reload_rc(void) {
  load_rc();
  reconfigure();
}


void Blackbox::reconfigure(void) {
  reconfigure_wait = True;

  if (! timer->isTiming()) timer->start();
}


void Blackbox::real_reconfigure(void) {
  XrmDatabase new_blackboxrc = (XrmDatabase) 0;
  char *style = new char[resource.style_file.length() + 20];

  sprintf(style, "session.styleFile: %s", getStyleFilename());
  XrmPutLineResource(&new_blackboxrc, style);

  delete [] style;

  XrmDatabase old_blackboxrc = XrmGetFileDatabase(rc_file.c_str());

  XrmMergeDatabases(new_blackboxrc, &old_blackboxrc);
  XrmPutFileDatabase(old_blackboxrc, rc_file.c_str());
  if (old_blackboxrc) XrmDestroyDatabase(old_blackboxrc);

  std::for_each(menuTimestamps.begin(), menuTimestamps.end(),
                PointerAssassin());
  menuTimestamps.clear();

  std::for_each(screenList.begin(), screenList.end(),
                std::mem_fun(&BScreen::reconfigure));
}


void Blackbox::checkMenu(void) {
  Bool reread = False;
  MenuTimestampList::iterator it = menuTimestamps.begin();
  for(; it != menuTimestamps.end(); ++it) {
    MenuTimestamp *tmp = *it;
    struct stat buf;

    if (! stat(tmp->filename.c_str(), &buf)) {
      if (tmp->timestamp != buf.st_ctime)
        reread = True;
    } else {
      reread = True;
    }
  }

  if (reread) rereadMenu();
}


void Blackbox::rereadMenu(void) {
  reread_menu_wait = True;

  if (! timer->isTiming()) timer->start();
}


void Blackbox::real_rereadMenu(void) {
  std::for_each(menuTimestamps.begin(), menuTimestamps.end(),
                PointerAssassin());
  menuTimestamps.clear();

  std::for_each(screenList.begin(), screenList.end(),
                std::mem_fun(&BScreen::rereadMenu));
}


void Blackbox::saveStyleFilename(const string& filename) {
  assert(! filename.empty());
  resource.style_file = filename;
}


void Blackbox::saveMenuFilename(const string& filename) {
  assert(! filename.empty());
  Bool found = False;

  MenuTimestampList::iterator it = menuTimestamps.begin();
  for (; it != menuTimestamps.end() && !found; ++it) {
    if ((*it)->filename == filename) found = True;
  }
  if (! found) {
    struct stat buf;

    if (! stat(filename.c_str(), &buf)) {
      MenuTimestamp *ts = new MenuTimestamp;

      ts->filename = filename;
      ts->timestamp = buf.st_ctime;

      menuTimestamps.push_back(ts);
    }
  }
}


void Blackbox::timeout(void) {
  if (reconfigure_wait)
    real_reconfigure();

  if (reread_menu_wait)
    real_rereadMenu();

  reconfigure_wait = reread_menu_wait = False;
}


void Blackbox::setFocusedWindow(BlackboxWindow *win) {
  BScreen *old_screen = (BScreen *) 0, *screen = (BScreen *) 0;
  Toolbar *old_tbar = (Toolbar *) 0, *tbar = (Toolbar *) 0;

  if (focused_window) {
    BlackboxWindow *old_win = focused_window;
    old_screen = old_win->getScreen();
    old_tbar = old_screen->getToolbar();
    Workspace *old_wkspc =
      old_screen->getWorkspace(old_win->getWorkspaceNumber());

    old_win->setFocusFlag(False);
    old_wkspc->getMenu()->setItemSelected(old_win->getWindowNumber(), False);
  }

  if (win && ! win->isIconic()) {
    screen = win->getScreen();
    tbar = screen->getToolbar();
    Workspace *wkspc = screen->getWorkspace(win->getWorkspaceNumber());

    focused_window = win;

    win->setFocusFlag(True);
    wkspc->getMenu()->setItemSelected(win->getWindowNumber(), True);
  } else {
    focused_window = (BlackboxWindow *) 0;
  }

  if (tbar)
    tbar->redrawWindowLabel(True);
  if (screen)
    screen->updateNetizenWindowFocus();

  if (old_tbar && old_tbar != tbar)
    old_tbar->redrawWindowLabel(True);
  if (old_screen && old_screen != screen)
    old_screen->updateNetizenWindowFocus();
}
