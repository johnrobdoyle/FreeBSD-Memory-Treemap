A complete, standalone C program for FreeBSD using Xlib and libkvm. It calculates system-wide physical memory statistics (Kernel Wired, Free RAM, Active, and process Resident Set Size) and uses a squarified 2D treemap algorithm (similar to AMD Radeon Memory Visualizer) to tile memory regions efficiently across the screen.

It also includes interactive mouse tooltips: hovering over any block reveals detailed memory allocation specs.
all memory blocks always fit precisely within the window boundaries without overflowing,
