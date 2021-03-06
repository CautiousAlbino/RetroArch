/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2015 - Daniel De Matteis
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* Win32/WGL context. */

/* necessary for mingw32 multimon defines: */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0500 //_WIN32_WINNT_WIN2K
#endif

#include "../../driver.h"
#include "../../runloop.h"
#include "../video_context_driver.h"
#include "../gl_common.h"
#include "../video_monitor.h"
#include "win32_common.h"
#include <windows.h>
#include <commdlg.h>
#include <string.h>

#define IDI_ICON 1
#define MAX_MONITORS 9

static bool g_use_hw_ctx;
static HWND g_hwnd;
static HGLRC g_hrc;
static HGLRC g_hw_hrc;
static HDC g_hdc;
static HMONITOR g_last_hm;
static HMONITOR g_all_hms[MAX_MONITORS];
static unsigned g_num_mons;
static unsigned g_major;
static unsigned g_minor;

static bool g_quit;
static bool g_inited;
static unsigned g_interval;

static unsigned g_resize_width;
static unsigned g_resize_height;
static unsigned g_pos_x = CW_USEDEFAULT;
static unsigned g_pos_y = CW_USEDEFAULT;
static bool g_resized;

static HINSTANCE dll_handle = NULL; /* Handle to OpenGL32.dll */

static bool g_restore_desktop;

static void monitor_info(MONITORINFOEX *mon, HMONITOR *hm_to_use);

static void gfx_ctx_wgl_destroy(void *data);

static BOOL (APIENTRY *p_swap_interval)(int);

typedef HGLRC (APIENTRY *wglCreateContextAttribsProc)(HDC, HGLRC, const int*);
static wglCreateContextAttribsProc pcreate_context;

static void setup_pixel_format(HDC hdc)
{
   PIXELFORMATDESCRIPTOR pfd = {0};
   pfd.nSize        = sizeof(PIXELFORMATDESCRIPTOR);
   pfd.nVersion     = 1;
   pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
   pfd.iPixelType   = PFD_TYPE_RGBA;
   pfd.cColorBits   = 32;
   pfd.cDepthBits   = 0;
   pfd.cStencilBits = 0;
   pfd.iLayerType   = PFD_MAIN_PLANE;

   SetPixelFormat(hdc, ChoosePixelFormat(hdc, &pfd), &pfd);
}

#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#endif
#ifndef WGL_CONTEXT_MINOR_VERSION_ARB
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#endif
#ifndef WGL_CONTEXT_PROFILE_MASK_ARB
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#endif
#ifndef WGL_CONTEXT_CORE_PROFILE_BIT_ARB
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001
#endif
#ifndef WGL_CONTEXT_FLAGS_ARB
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#endif
#ifndef WGL_CONTEXT_DEBUG_BIT_ARB
#define WGL_CONTEXT_DEBUG_BIT_ARB 0x0001
#endif

static void create_gl_context(HWND hwnd)
{
   bool core_context;
   bool debug = g_extern.system.hw_render_callback.debug_context;

#ifdef _WIN32
   dll_handle = LoadLibrary("OpenGL32.dll");
#endif

   g_hdc = GetDC(hwnd);
   setup_pixel_format(g_hdc);

#ifdef GL_DEBUG
   debug = true;
#endif
   core_context = (g_major * 1000 + g_minor) >= 3001;

   if (g_hrc)
   {
      RARCH_LOG("[WGL]: Using cached GL context.\n");
      driver.video_cache_context_ack = true;
   }
   else
   {
      g_hrc = wglCreateContext(g_hdc);
      
      /* We'll create shared context later if not. */
      if (g_hrc && !core_context && !debug) 
      {
         g_hw_hrc = wglCreateContext(g_hdc);
         if (g_hw_hrc)
         {
            if (!wglShareLists(g_hrc, g_hw_hrc))
            {
               RARCH_LOG("[WGL]: Failed to share contexts.\n");
               g_quit = true;
            }
         }
         else
            g_quit = true;
      }
   }

   if (g_hrc)
   {
      if (wglMakeCurrent(g_hdc, g_hrc))
         g_inited = true;
      else
         g_quit = true;
   }
   else
   {
      g_quit = true;
      return;
   }

   if (core_context || debug)
   {
      int attribs[16];
      int *aptr = attribs;

      if (core_context)
      {
         *aptr++ = WGL_CONTEXT_MAJOR_VERSION_ARB;
         *aptr++ = g_major;
         *aptr++ = WGL_CONTEXT_MINOR_VERSION_ARB;
         *aptr++ = g_minor;

         /* Technically, we don't have core/compat until 3.2.
          * Version 3.1 is either compat or not depending 
          * on GL_ARB_compatibility.
          */
         if ((g_major * 1000 + g_minor) >= 3002)
         {
            *aptr++ = WGL_CONTEXT_PROFILE_MASK_ARB;
            *aptr++ = WGL_CONTEXT_CORE_PROFILE_BIT_ARB;
         }
      }

      if (debug)
      {
         *aptr++ = WGL_CONTEXT_FLAGS_ARB;
         *aptr++ = WGL_CONTEXT_DEBUG_BIT_ARB;
      }

      *aptr = 0;

      if (!pcreate_context)
         pcreate_context = (wglCreateContextAttribsProc)
            wglGetProcAddress("wglCreateContextAttribsARB");

      if (pcreate_context)
      {
         HGLRC context = pcreate_context(g_hdc, NULL, attribs);

         if (context)
         {
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(g_hrc);
            g_hrc = context;
            if (!wglMakeCurrent(g_hdc, g_hrc))
               g_quit = true;
         }
         else
            RARCH_ERR("[WGL]: Failed to create core context. Falling back to legacy context.\n");

         if (g_use_hw_ctx)
         {
            g_hw_hrc = pcreate_context(g_hdc, context, attribs);
            if (!g_hw_hrc)
            {
               RARCH_ERR("[WGL]: Failed to create shared context.\n");
               g_quit = true;
            }
         }
      }
      else
         RARCH_ERR("[WGL]: wglCreateContextAttribsARB not supported.\n");
   }
}

#ifdef __cplusplus
extern "C"
#endif
bool dinput_handle_message(void *dinput, UINT message, WPARAM wParam, LPARAM lParam);

static void *dinput_wgl;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message,
      WPARAM wparam, LPARAM lparam)
{
   switch (message)
   {
      case WM_SYSCOMMAND:
         /* Prevent screensavers, etc, while running. */
         switch (wparam)
         {
            case SC_SCREENSAVE:
            case SC_MONITORPOWER:
               return 0;
         }
         break;

      case WM_CHAR:
      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_SYSKEYUP:
      case WM_SYSKEYDOWN:
         return win32_handle_keyboard_event(hwnd, message, wparam, lparam);

      case WM_CREATE:
         create_gl_context(hwnd);
         return 0;

      case WM_CLOSE:
      case WM_DESTROY:
      case WM_QUIT:
      {
         WINDOWPLACEMENT placement;
         GetWindowPlacement(g_hwnd, &placement);
         g_pos_x = placement.rcNormalPosition.left;
         g_pos_y = placement.rcNormalPosition.top;
         g_quit = true;
         return 0;
      }
      case WM_SIZE:
         /* Do not send resize message if we minimize. */
         if (wparam != SIZE_MAXHIDE && wparam != SIZE_MINIMIZED)
         {
            g_resize_width  = LOWORD(lparam);
            g_resize_height = HIWORD(lparam);
            g_resized = true;
         }
         return 0;
	  case WM_COMMAND:
         if (g_settings.ui.menubar_enable)
         {
            LRESULT ret = win32_menu_loop(g_hwnd, wparam);
         }
         break;
   }

   if (dinput_handle_message(dinput_wgl, message, wparam, lparam))
      return 0;
   return DefWindowProc(hwnd, message, wparam, lparam);
}

static void gfx_ctx_wgl_swap_interval(void *data, unsigned interval)
{
   (void)data;
   g_interval = interval;

   if (!g_hrc)
      return;
   if (!p_swap_interval)
      return;

   RARCH_LOG("[WGL]: wglSwapInterval(%u)\n", g_interval);
   if (!p_swap_interval(g_interval))
      RARCH_WARN("[WGL]: wglSwapInterval() failed.\n");
}

static void gfx_ctx_wgl_check_window(void *data, bool *quit,
      bool *resize, unsigned *width, unsigned *height, unsigned frame_count)
{
   MSG msg;

   (void)data;
   (void)frame_count;

   while (PeekMessage(&msg, g_hwnd, 0, 0, PM_REMOVE))
   {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
   }

   *quit = g_quit;

   if (g_resized)
   {
      *resize = true;
      *width  = g_resize_width;
      *height = g_resize_height;
      g_resized = false;
   }
}

static void gfx_ctx_wgl_swap_buffers(void *data)
{
   (void)data;
   SwapBuffers(g_hdc);
}

static void gfx_ctx_wgl_set_resize(void *data,
      unsigned width, unsigned height)
{
   (void)data;
   (void)width;
   (void)height;
}

static void gfx_ctx_wgl_update_window_title(void *data)
{
   char buf[128], buf_fps[128];

   (void)data;

   if (video_monitor_get_fps(buf, sizeof(buf),
            buf_fps, sizeof(buf_fps)))
      SetWindowText(g_hwnd, buf);
   if (g_settings.fps_show)
      rarch_main_msg_queue_push(buf_fps, 1, 1, false);
}

static void gfx_ctx_wgl_get_video_size(void *data, unsigned *width, unsigned *height)
{
   (void)data;

   if (!g_hwnd)
   {
      RECT mon_rect;
      MONITORINFOEX current_mon;
      HMONITOR hm_to_use = NULL;

      monitor_info(&current_mon, &hm_to_use);
      mon_rect = current_mon.rcMonitor;
      *width  = mon_rect.right - mon_rect.left;
      *height = mon_rect.bottom - mon_rect.top;
   }
   else
   {
      *width  = g_resize_width;
      *height = g_resize_height;
   }
}

static BOOL CALLBACK monitor_enum_proc(HMONITOR hMonitor,
      HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
   g_all_hms[g_num_mons++] = hMonitor;
   return TRUE;
}

static bool gfx_ctx_wgl_init(void *data)
{
   WNDCLASSEX wndclass = {0};

   (void)data;

   if (g_inited)
      return false;

   g_quit = false;
   g_restore_desktop = false;

   g_num_mons = 0;
   EnumDisplayMonitors(NULL, NULL, monitor_enum_proc, 0);

   wndclass.cbSize = sizeof(wndclass);
   wndclass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
   wndclass.lpfnWndProc = WndProc;
   wndclass.hInstance = GetModuleHandle(NULL);
   wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
   wndclass.lpszClassName = "RetroArch";
   wndclass.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON));
   wndclass.hIconSm = (HICON)LoadImage(GetModuleHandle(NULL),
         MAKEINTRESOURCE(IDI_ICON), IMAGE_ICON, 16, 16, 0);

   if (!RegisterClassEx(&wndclass))
      return false;

   return true;
}

static bool set_fullscreen(unsigned width, unsigned height, char *dev_name)
{
   DEVMODE devmode;

   memset(&devmode, 0, sizeof(devmode));
   devmode.dmSize       = sizeof(DEVMODE);
   devmode.dmPelsWidth  = width;
   devmode.dmPelsHeight = height;
   devmode.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT;

   RARCH_LOG("[WGL]: Setting fullscreen to %ux%u on device %s.\n", width, height, dev_name);
   return ChangeDisplaySettingsEx(dev_name, &devmode, NULL, CDS_FULLSCREEN, NULL) == DISP_CHANGE_SUCCESSFUL;
}

static void show_cursor(bool show)
{
   if (show)
      while (ShowCursor(TRUE) < 0);
   else
      while (ShowCursor(FALSE) >= 0);
}

static void monitor_info(MONITORINFOEX *mon, HMONITOR *hm_to_use)
{
   unsigned fs_monitor;

   if (!g_last_hm)
      g_last_hm = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
   *hm_to_use = g_last_hm;

   fs_monitor = g_settings.video.monitor_index;
   if (fs_monitor && fs_monitor <= g_num_mons && g_all_hms[fs_monitor - 1])
      *hm_to_use = g_all_hms[fs_monitor - 1];

   memset(mon, 0, sizeof(*mon));
   mon->cbSize = sizeof(MONITORINFOEX);
   GetMonitorInfo(*hm_to_use, (MONITORINFO*)mon);
}

static bool gfx_ctx_wgl_set_video_mode(void *data,
      unsigned width, unsigned height,
      bool fullscreen)
{
   DWORD style;
   MSG msg;
   RECT mon_rect;
   MONITORINFOEX current_mon;
   bool windowed_full;
   RECT rect   = {0};
   HMONITOR hm_to_use = NULL;

   monitor_info(&current_mon, &hm_to_use);
   mon_rect = current_mon.rcMonitor;

   g_resize_width  = width;
   g_resize_height = height;

   windowed_full = g_settings.video.windowed_fullscreen;

   if (fullscreen)
   {
      if (windowed_full)
      {
         style = WS_EX_TOPMOST | WS_POPUP;
         g_resize_width  = width  = mon_rect.right - mon_rect.left;
         g_resize_height = height = mon_rect.bottom - mon_rect.top;
      }
      else
      {
         style = WS_POPUP | WS_VISIBLE;

         if (!set_fullscreen(width, height, current_mon.szDevice))
            goto error;

         /* Display settings might have changed, get new coordinates. */
         GetMonitorInfo(hm_to_use, (MONITORINFO*)&current_mon);
         mon_rect = current_mon.rcMonitor;
         g_restore_desktop = true;
      }
   }
   else
   {
      style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
      rect.right  = width;
      rect.bottom = height;
      AdjustWindowRect(&rect, style, FALSE);
      g_resize_width  = width  = rect.right - rect.left;
      g_resize_height = height = rect.bottom - rect.top;
   }

   g_hwnd = CreateWindowEx(0, "RetroArch", "RetroArch", style,
         fullscreen ? mon_rect.left : g_pos_x,
         fullscreen ? mon_rect.top  : g_pos_y,
         width, height,
         NULL, NULL, NULL, NULL);

   if (!g_hwnd)
      goto error;

   if (!fullscreen || windowed_full)
   {
      if (!fullscreen && g_settings.ui.menubar_enable)
      {
         RECT rc_temp = {0, 0, height, 0x7FFF};
         SetMenu(g_hwnd, LoadMenu(GetModuleHandle(NULL),MAKEINTRESOURCE(IDR_MENU)));
         SendMessage(g_hwnd, WM_NCCALCSIZE, FALSE, (LPARAM)&rc_temp);
         g_resize_height = height += rc_temp.top + rect.top;
         SetWindowPos(g_hwnd, NULL, 0, 0, width, height, SWP_NOMOVE);
      }

      ShowWindow(g_hwnd, SW_RESTORE);
      UpdateWindow(g_hwnd);
      SetForegroundWindow(g_hwnd);
      SetFocus(g_hwnd);
   }

   show_cursor(!fullscreen);

   /* Wait until GL context is created (or failed to do so ...) */
   while (!g_inited && !g_quit && GetMessage(&msg, g_hwnd, 0, 0))
   {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
   }

   if (g_quit)
      goto error;

   p_swap_interval = (BOOL (APIENTRY *)(int))wglGetProcAddress("wglSwapIntervalEXT");

   gfx_ctx_wgl_swap_interval(data, g_interval);

   driver.display_type  = RARCH_DISPLAY_WIN32;
   driver.video_display = 0;
   driver.video_window  = (uintptr_t)g_hwnd;

   return true;

error:
   gfx_ctx_wgl_destroy(data);
   return false;
}

static void gfx_ctx_wgl_destroy(void *data)
{
   (void)data;

   if (g_hrc)
   {
      glFinish();
      wglMakeCurrent(NULL, NULL);

      if (!driver.video_cache_context)
      {
         if (g_hw_hrc)
            wglDeleteContext(g_hw_hrc);
         wglDeleteContext(g_hrc);
         g_hrc = NULL;
         g_hw_hrc = NULL;
      }
   }

   if (g_hwnd && g_hdc)
   {
      ReleaseDC(g_hwnd, g_hdc);
      g_hdc = NULL;
   }

   if (g_hwnd)
   {
      g_last_hm = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
      DestroyWindow(g_hwnd);
      UnregisterClass("RetroArch", GetModuleHandle(NULL));
      g_hwnd = NULL;
   }

   if (g_restore_desktop)
   {
      MONITORINFOEX current_mon;
      memset(&current_mon, 0, sizeof(current_mon));
      current_mon.cbSize = sizeof(MONITORINFOEX);
      GetMonitorInfo(g_last_hm, (MONITORINFO*)&current_mon);
      ChangeDisplaySettingsEx(current_mon.szDevice, NULL, NULL, 0, NULL);
      g_restore_desktop = false;
   }

   g_inited = false;
   g_major = g_minor = 0;
   p_swap_interval = NULL;
}

static void gfx_ctx_wgl_input_driver(void *data,
      const input_driver_t **input, void **input_data)
{
   (void)data;

   dinput_wgl   = input_dinput.init();

   *input       = dinput_wgl ? &input_dinput : NULL;
   *input_data  = dinput_wgl;
}

static bool gfx_ctx_wgl_has_focus(void *data)
{
   (void)data;

   if (!g_inited)
      return false;

   return GetFocus() == g_hwnd;
}

static bool gfx_ctx_wgl_suppress_screensaver(void *data, bool enable)
{
   (void)data;
   (void)enable;

   return false;
}

static bool gfx_ctx_wgl_has_windowed(void *data)
{
   (void)data;

   return true;
}

static gfx_ctx_proc_t gfx_ctx_wgl_get_proc_address(const char *symbol)
{
   void *func = (void *)wglGetProcAddress(symbol);
   if (func)
      return (gfx_ctx_proc_t)wglGetProcAddress(symbol);
   return (gfx_ctx_proc_t)GetProcAddress(dll_handle, symbol);
}

static bool gfx_ctx_wgl_bind_api(void *data,
      enum gfx_ctx_api api, unsigned major, unsigned minor)
{
   (void)data;

   g_major = major;
   g_minor = minor;

   return api == GFX_CTX_OPENGL_API;
}

static void gfx_ctx_wgl_show_mouse(void *data, bool state)
{
   (void)data;
   show_cursor(state);
}

static void gfx_ctx_wgl_bind_hw_render(void *data, bool enable)
{
   g_use_hw_ctx = enable;

   if (g_hdc)
      wglMakeCurrent(g_hdc, enable ? g_hw_hrc : g_hrc);
}

const gfx_ctx_driver_t gfx_ctx_wgl = {
   gfx_ctx_wgl_init,
   gfx_ctx_wgl_destroy,
   gfx_ctx_wgl_bind_api,
   gfx_ctx_wgl_swap_interval,
   gfx_ctx_wgl_set_video_mode,
   gfx_ctx_wgl_get_video_size,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL,
   gfx_ctx_wgl_update_window_title,
   gfx_ctx_wgl_check_window,
   gfx_ctx_wgl_set_resize,
   gfx_ctx_wgl_has_focus,
   gfx_ctx_wgl_suppress_screensaver,
   gfx_ctx_wgl_has_windowed,
   gfx_ctx_wgl_swap_buffers,
   gfx_ctx_wgl_input_driver,
   gfx_ctx_wgl_get_proc_address,
   gfx_ctx_wgl_show_mouse,
   "wgl",
   gfx_ctx_wgl_bind_hw_render,
};

