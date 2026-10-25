/* nav.c: mouse_nav, the mouse's back/forward buttons (docs/mouse-nav.md).
 *
 * The game's window procedure 0x68d630 handles no side buttons. The display init 0x5e2620 registers it as the
 * window class's lpfnWndProc (0x5e271d: mov [esp+0x1c],0x68d630); the immediate is pointed at nav_wndproc, which
 * calls the game's. That patches code, not an import slot, since cnc-ddraw rewrites the exe's user32 slots.
 *
 * The sidebar handler 0x7615d0 (cdecl, the clicked widget's index or -1; every screen calls it) takes the action
 * from the widget, or from 0xdd6bde when the index is -1. Action -2 goes back and -3 forward through the screen
 * history (0x762c97/0x762cfc, gated by the screen manager's 0x7fec60(0) and 0x7febb0, as the sidebar's arrow
 * buttons are). A side button sets 0xdd6bde to that action for one call.
 */

#include <windows.h>
#include <string.h>

#include "cmspeed.h"

#define WNDPROC_IMM_AT 0x5e271d
static const BYTE wndproc_imm_orig[8] = {0xc7, 0x44, 0x24, 0x1c, 0x30, 0xd6, 0x68, 0x00};
#define GAME_WNDPROC ((WNDPROC)0x68d630)

#define SIDEBAR_AT 0x7615d0
static const BYTE sidebar_orig[6] = {0x81, 0xec, 0xa4, 0x02, 0x00, 0x00}; /* sub esp,0x2a4 */
#define SIDEBAR_ACTION ((volatile short *)0xdd6bde)
#define NAV_BACK    -2
#define NAV_FORWARD -3

static short nav_pending;
static int(__cdecl *sidebar)(int widget); /* trampoline: the stock 0x7615d0 */

static int __cdecl nav_sidebar(int widget)
{
    if (!nav_pending || (short)widget != -1) /* a real click this frame comes first */
        return sidebar(widget);
    short saved = *SIDEBAR_ACTION;
    *SIDEBAR_ACTION = nav_pending;
    nav_pending = 0;
    int r = sidebar(-1);
    *SIDEBAR_ACTION = saved;
    return r;
}

static LRESULT CALLBACK nav_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        nav_pending = GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? NAV_FORWARD : NAV_BACK;
        return TRUE;
    case WM_XBUTTONUP: /* DefWindowProc would turn it into a WM_APPCOMMAND as well */
        return TRUE;
    case WM_APPCOMMAND: /* browser keys on keyboards, and side buttons some drivers send this way */
        if (GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_BROWSER_BACKWARD ||
            GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_BROWSER_FORWARD) {
            nav_pending = GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_BROWSER_FORWARD ? NAV_FORWARD : NAV_BACK;
            return TRUE;
        }
        break;
    }
    return CallWindowProcA(GAME_WNDPROC, hwnd, msg, wp, lp);
}

int install_mouse_nav(void)
{
    if (!(sidebar = hook_entry(SIDEBAR_AT, sidebar_orig, sizeof sidebar_orig, nav_sidebar)))
        return 0;
    BYTE repl[8];
    memcpy(repl, wndproc_imm_orig, 4);
    *(void **)(repl + 4) = (void *)nav_wndproc;
    return patch_bytes(WNDPROC_IMM_AT, wndproc_imm_orig, repl, sizeof repl);
}
