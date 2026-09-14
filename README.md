A complete, standalone C program for FreeBSD using Xlib and libkvm. It calculates system-wide physical memory statistics (Kernel Wired, Free RAM, Active, and process Resident Set Size) and uses a squarified 2D treemap algorithm to tile memory regions efficiently across the screen.

It also includes interactive mouse tooltips: hovering over any block reveals detailed memory allocation specs.
All memory blocks always fit precisely within the window boundaries without overflowing,

Controls Summary:
- (Minus): Removes the largest box currently displayed.
+ or = (Plus): Restores the most recently removed box.
Left-Click: Zoom into a single process block.
Right-Click / ESC: Unzoom back to the treemap view.
Q: Exit program.

Build instructions:
clang -O2 -I/usr/local/include freebsd_treemap_zooming.c -o freebsd_treemap_zooming -L/usr/local/lib -lkvm -lX11 -lm

Run:
doas ./freebsd_treemap_zooming

