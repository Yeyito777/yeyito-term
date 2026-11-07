# Modifier change reports (mode 7778)

A terminal sends nothing when a modifier key is pressed or let go on its own,
so an app tracking the pointer only learns about Shift on the next mouse move.
Private mode 7778 lets an app ask for that change.

```
CSI ? 7778 h      enable
CSI ? 7778 l      disable
CSI ? 7778 $ p    query (DECRQM): CSI ? 7778 ; 1 $ y set, ; 2 $ y reset
```

While it is set, each change to Shift, Option (Alt) or Control sends

```
CSI ? 7778 ; MODS ; COL ; ROW ~
```

- `MODS` is the new state: Shift 1, Option 2, Control 4, added together.
  Caps Lock, Command and fn are not reported, and a change to only those
  sends nothing.
- `COL`, `ROW` are the 1-based cell under the pointer.
- Only when the pointer is over the terminal in the key window. A change
  while it is elsewhere sends nothing.

It is not a mouse report: the pointer did not move, so st sends no mouse
motion for a modifier change, and apps that leave the mode off see nothing at
all. Real mouse reports are unchanged and still carry their own modifier bits.

The mode is an application mode: st clears it with the other app modes when
an application boundary is lost (`MODE_APPRESET`), so a TUI should set it
alongside its mouse modes and reset it on exit and suspend.

macOS only for now (`flagsChanged:` in `macos/backend.m`). On Linux the mode
can be set and queried but sends nothing.

Exocortex uses it to show its task panel's Shift+hover highlight as soon as
Shift goes down or up.
