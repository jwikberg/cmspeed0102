# Mouse back/forward buttons

Option: `mouse_nav` (default 1).

## What the game does

The sidebar's ← → arrows go back and forward through the screen history. The sidebar handler `0x7615d0` (cdecl, the
clicked widget's index or −1) is called every frame on every screen with a sidebar, about 1 000 times a second at an idle
menu. With −1 it takes its action from the word `0xdd6bde`. Action −2 goes back and −3 forward (`0x762c97`,
`0x762cfc`). Each is gated by the screen manager's `0x7fec60(0)` and `0x7febb0`, the same checks the arrows use.

The window procedure handles no `WM_XBUTTON*` or `WM_APPCOMMAND` messages, so the mouse's side buttons do nothing.

## How cmspeed works

- cmspeed puts its own window procedure in front of the game's (`0x68d630`), by changing the window class's
  `lpfnWndProc` immediate at `0x5e271d`. That patches code rather than an import slot, because cnc-ddraw rewrites the
  exe's user32 import slots.
- It marks a press on `WM_XBUTTONDOWN` (and its double-click variant): back for button 1, forward for button 2. It swallows `WM_XBUTTONUP`, which would otherwise
  become a `WM_APPCOMMAND` too.
- `WM_APPCOMMAND` with `BROWSER_BACKWARD` or `BROWSER_FORWARD` does the same. That covers the browser keys on keyboards,
  and mice whose driver sends the side buttons this way.
- A detour on the sidebar handler then runs the stock handler once with −1 and `0xdd6bde` set to the action, and
  restores the word afterwards. A real click in the same frame comes first.

The press goes through the game's own back/forward code, with the same checks as the arrows.

## How it was researched

The decompile of the sidebar handler showed the action word and the two history actions. Counting its calls showed
that it runs every frame, so a pending press is picked up at once.

## Verified

Headless (Xvfb, Wine, cnc-ddraw) with real X input through XTest: main menu → Hall of Fame → button 8 (back) → main
menu → button 9 (forward) → Hall of Fame → back → main menu. A further back stays on the main menu. The same test
passed with Wine's own DirectDraw instead of cnc-ddraw.

## Limits

- On screens without the sidebar, such as modal dialogs like Game Settings, the handler isn't called, so the buttons do
  nothing there, as the arrows do.
