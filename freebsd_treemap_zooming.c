#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <sys/select.h>
#include <kvm.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#ifndef _POSIX2_LINE_MAX
#define _POSIX2_LINE_MAX 2048
#endif

#define MAX_NODES 512
#define WIN_WIDTH 1024
#define WIN_HEIGHT 720

typedef struct {
    char name[COMMLEN + 32];
    long pages;
    int count;
    unsigned long pixel;
    int x, y, w, h;
} RectNode;

static RectNode nodes[MAX_NODES];
static double prefix_sums[MAX_NODES + 1];
static int node_count = 0;
static int removed_blocks_count = 0;
static long total_sys_pages = 0;
static long page_size = 4096;
static int zoomed_node_idx = -1;

static Pixmap back_buffer = None;
static int buffer_w = 0, buffer_h = 0;

static const char *treemap_colors[] = {
"#2B5C8F", "#D4A370",
  "#8B0000", "#74FFFF",
  "#2E8B57", "#D174A8",
  "#D2691E", "#2D96E1",
  "#6A5ACD", "#95A532",
  "#008B8B", "#FF7474",
  "#B22222", "#4DDDDD",
  "#483D8B", "#B7C274",
  "#008080", "#FF7F7F",
  "#C71585", "#38EA7A",
  "#4682B4", "#B97D4B",
  "#9ACD32", "#6532CD",
  "#8B4513", "#74BAEC",
  "#5F9EA0", "#A0615F",
  "#D87093", "#278F6C",
  "#1E3D59", "#E1C2A6",
  "#5C0000", "#A3FFFF",
  "#1E5939", "#E1A6C6",
  "#8B4513", "#74BAEC",
  "#463D8B", "#B9C274",
  "#005C5C", "#FFA3A3",
  "#7A1717", "#85E8E8",
  "#2E275C", "#D1C8A3",
  "#005050", "#FFAFDF",
  "#850E59", "#7AE1A6",
  "#2E5677", "#D1A988",
  "#668821", "#9977DE",
  "#5C2E0C", "#A3D1F3",
  "#3E686A", "#C19795",
  "#8F4A61", "#70B59E",
  "#142A3E", "#EBD5C1",
  "#3E0000", "#C1FFFF"
};
#define PALETTE_SIZE (sizeof(treemap_colors) / sizeof(treemap_colors[0]))

static unsigned long parse_hex_color(Display *dpy, Colormap cmap, const char *hex) {
    XColor xcolor;
    if (XParseColor(dpy, cmap, hex, &xcolor) && XAllocColor(dpy, cmap, &xcolor)) {
        return xcolor.pixel;
    }
    return WhitePixel(dpy, DefaultScreen(dpy));
}

static int compare_nodes(const void *a, const void *b) {
    long pa = ((const RectNode *)a)->pages;
    long pb = ((const RectNode *)b)->pages;
    return (pb > pa) - (pb < pa);
}

static void collect_memory_data(Display *dpy, Colormap cmap) {
    size_t len;
    long physmem_bytes = 0;
    len = sizeof(physmem_bytes);
    if (sysctlbyname("hw.physmem", &physmem_bytes, &len, NULL, 0) == 0) {
        total_sys_pages = physmem_bytes / page_size;
    }

    u_int free_c = 0, wire_c = 0;
    len = sizeof(free_c);
    sysctlbyname("vm.stats.vm.v_free_count", &free_c, &len, NULL, 0);
    sysctlbyname("vm.stats.vm.v_wire_count", &wire_c, &len, NULL, 0);

    char errbuf[_POSIX2_LINE_MAX];
    kvm_t *kd = kvm_openfiles(NULL, "/dev/null", NULL, O_RDONLY, errbuf);
    if (!kd) return;

    int nprocs = 0;
    struct kinfo_proc *procs = kvm_getprocs(kd, KERN_PROC_PROC, 0, &nprocs);
    if (!procs) {
        kvm_close(kd);
        return;
    }

    node_count = 0;

    if (wire_c > 0) {
        strncpy(nodes[node_count].name, "[Kernel Wired]", COMMLEN);
        nodes[node_count].pages = wire_c;
        nodes[node_count].count = 1;
        nodes[node_count].pixel = parse_hex_color(dpy, cmap, "#1E293B");
        node_count++;
    }

    for (int i = 0; i < nprocs && node_count < MAX_NODES - 2; i++) {
        long rss = procs[i].ki_rssize;
        if (rss <= 0) continue;

        const char *comm = procs[i].ki_comm;
        int found = 0;

        for (int j = 1; j < node_count; j++) {
            if (strcmp(nodes[j].name, comm) == 0) {
                nodes[j].pages += rss;
                nodes[j].count++;
                found = 1;
                break;
            }
        }

        if (!found) {
            strncpy(nodes[node_count].name, comm, COMMLEN);
            nodes[node_count].name[COMMLEN] = '\0';
            nodes[node_count].pages = rss;
            nodes[node_count].count = 1;
            nodes[node_count].pixel = parse_hex_color(dpy, cmap, treemap_colors[node_count % PALETTE_SIZE]);
            node_count++;
        }
    }
    kvm_close(kd);

    if (free_c > 0 && node_count < MAX_NODES) {
        strncpy(nodes[node_count].name, "[Free Memory]", COMMLEN);
        nodes[node_count].pages = free_c;
        nodes[node_count].count = 1;
        nodes[node_count].pixel = parse_hex_color(dpy, cmap, "#15803D");
        node_count++;
    }

    qsort(nodes, node_count, sizeof(RectNode), compare_nodes);

    prefix_sums[0] = 0;
    for (int i = 0; i < node_count; i++) {
        prefix_sums[i + 1] = prefix_sums[i] + nodes[i].pages;
    }

    if (removed_blocks_count >= node_count) {
        removed_blocks_count = node_count - 1;
        if (removed_blocks_count < 0) removed_blocks_count = 0;
    }
}

static void layout_treemap(int start_idx, int end_idx, double x, double y, double w, double h) {
    if (start_idx >= end_idx || w <= 0.5 || h <= 0.5) return;

    if (start_idx == end_idx - 1) {
        nodes[start_idx].x = (int)round(x);
        nodes[start_idx].y = (int)round(y);
        nodes[start_idx].w = (int)round(w);
        nodes[start_idx].h = (int)round(h);
        return;
    }

    double current_sum = prefix_sums[end_idx] - prefix_sums[start_idx];
    if (current_sum <= 0) return;

    double target = prefix_sums[start_idx] + (current_sum / 2.0);
    int split = start_idx + 1;

    for (int i = start_idx; i < end_idx - 1; i++) {
        if (prefix_sums[i + 1] >= target) {
            split = i + 1;
            break;
        }
    }

    double left_pages = prefix_sums[split] - prefix_sums[start_idx];
    double ratio = left_pages / current_sum;

    if (w > h) {
        double w_left = w * ratio;
        layout_treemap(start_idx, split, x, y, w_left, h);
        layout_treemap(split, end_idx, x + w_left, y, w - w_left, h);
    } else {
        double h_top = h * ratio;
        layout_treemap(start_idx, split, x, y, w, h_top);
        layout_treemap(split, end_idx, x, y + h_top, w, h - h_top);
    }
}

static void draw_help_legend(Display *dpy, Drawable drw, GC gc, int win_h, unsigned long dark_slate) {
    int leg_w = 280, leg_h = 95, leg_x = 10;
    int leg_y = win_h - leg_h - 10;
    if (leg_y < 10) return;

    XSetForeground(dpy, gc, dark_slate);
    XFillRectangle(dpy, drw, gc, leg_x, leg_y, leg_w, leg_h);
    XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
    XDrawRectangle(dpy, drw, gc, leg_x, leg_y, leg_w, leg_h);

    const char *lines[] = {
        "[ CONTROLS LEGEND ]",
        " - : Remove largest memory block from view",
        " + : Restore hidden memory block to view",
        " L-Click : Zoom block",
        " R-Click/ESC : Reset Zoom",
        " Q : Quit Application"
    };

    for (int i = 0; i < 6; i++) {
        XDrawString(dpy, drw, gc, leg_x + 10, leg_y + 16 + (i * 14), lines[i], strlen(lines[i]));
    }
}

static void draw_treemap(Display *dpy, Window win, GC gc, int win_w, int win_h, int mx, int my, unsigned long dark_slate, unsigned long tooltip_yellow) {
    if (node_count == 0 || total_sys_pages <= 0) return;

    if (!back_buffer || buffer_w != win_w || buffer_h != win_h) {
        if (back_buffer) XFreePixmap(dpy, back_buffer);
        back_buffer = XCreatePixmap(dpy, win, win_w, win_h, DefaultDepth(dpy, DefaultScreen(dpy)));
        buffer_w = win_w;
        buffer_h = win_h;
    }

    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    XFillRectangle(dpy, back_buffer, gc, 0, 0, win_w, win_h);

    int hovered_idx = -1;
    int active_start_idx = removed_blocks_count;
    if (active_start_idx >= node_count) active_start_idx = node_count - 1;

    if (zoomed_node_idx >= 0 && zoomed_node_idx < node_count) {
        int idx = zoomed_node_idx;
        nodes[idx].x = 0; nodes[idx].y = 0;
        nodes[idx].w = win_w; nodes[idx].h = win_h;

        XSetForeground(dpy, gc, nodes[idx].pixel);
        XFillRectangle(dpy, back_buffer, gc, 0, 0, win_w, win_h);

        XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
        XSetLineAttributes(dpy, gc, 3, LineSolid, CapProjecting, JoinMiter);
        XDrawRectangle(dpy, back_buffer, gc, 1, 1, win_w - 3, win_h - 3);
        XSetLineAttributes(dpy, gc, 1, LineSolid, CapProjecting, JoinMiter);

        char banner[128];
        long mb = (nodes[idx].pages * page_size) / (1024 * 1024);
        snprintf(banner, sizeof(banner), "ZOOMED: %s (%ld MB) - Right-Click or ESC to Reset", nodes[idx].name, mb);
        
        XFillRectangle(dpy, back_buffer, gc, 10, 10, 480, 25);
        XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
        XDrawString(dpy, back_buffer, gc, 20, 27, banner, strlen(banner));

        hovered_idx = idx;
    } else {
        layout_treemap(active_start_idx, node_count, 0.0, 0.0, (double)win_w, (double)win_h);

        for (int i = active_start_idx; i < node_count; i++) {
            if (nodes[i].x + nodes[i].w > win_w) nodes[i].w = win_w - nodes[i].x;
            if (nodes[i].y + nodes[i].h > win_h) nodes[i].h = win_h - nodes[i].y;

            if (nodes[i].w <= 0 || nodes[i].h <= 0) continue;

            if (mx >= nodes[i].x && mx < (nodes[i].x + nodes[i].w) &&
                my >= nodes[i].y && my < (nodes[i].y + nodes[i].h)) {
                hovered_idx = i;
            }

            XSetForeground(dpy, gc, nodes[i].pixel);
            XFillRectangle(dpy, back_buffer, gc, nodes[i].x, nodes[i].y, nodes[i].w, nodes[i].h);

            XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
            XDrawRectangle(dpy, back_buffer, gc, nodes[i].x, nodes[i].y, nodes[i].w - 1, nodes[i].h - 1);

            if (nodes[i].w > 45 && nodes[i].h > 20) {
                char txt[64];
                long mb = (nodes[i].pages * page_size) / (1024 * 1024);
                snprintf(txt, sizeof(txt), "%s (%ldM)", nodes[i].name, mb);
                XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
                XDrawString(dpy, back_buffer, gc, nodes[i].x + 6, nodes[i].y + 16, txt, strlen(txt));
            }
        }

        if (removed_blocks_count > 0) {
            char status[128];
            snprintf(status, sizeof(status), "Filtered Out: %d Largest Block(s) [- to remove, + to restore]", removed_blocks_count);
            XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
            XFillRectangle(dpy, back_buffer, gc, 5, 5, 410, 20);
            XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
            XDrawString(dpy, back_buffer, gc, 12, 19, status, strlen(status));
        }
    }

    draw_help_legend(dpy, back_buffer, gc, win_h, dark_slate);

    if (hovered_idx != -1) {
        int i = hovered_idx;
        long mb = (nodes[i].pages * page_size) / (1024 * 1024);
        double pct = ((double)nodes[i].pages / total_sys_pages) * 100.0;

        char line1[128], line2[128], line3[128];
        snprintf(line1, sizeof(line1), "NAME: %s", nodes[i].name);
        snprintf(line2, sizeof(line2), "SIZE: %ld MB (%.2f%%)", mb, pct);
        snprintf(line3, sizeof(line3), "COUNT: %d process(es)", nodes[i].count);

        int tip_w = 230, tip_h = 58;
        int tip_x = mx + 12, tip_y = my + 12;

        if (tip_x + tip_w > win_w) tip_x = win_w - tip_w - 5;
        if (tip_y + tip_h > win_h) tip_y = win_h - tip_h - 5;

        XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
        XFillRectangle(dpy, back_buffer, gc, tip_x + 3, tip_y + 3, tip_w, tip_h);

        XSetForeground(dpy, gc, tooltip_yellow);
        XFillRectangle(dpy, back_buffer, gc, tip_x, tip_y, tip_w, tip_h);

        XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
        XDrawRectangle(dpy, back_buffer, gc, tip_x, tip_y, tip_w, tip_h);

        XDrawString(dpy, back_buffer, gc, tip_x + 8, tip_y + 16, line1, strlen(line1));
        XDrawString(dpy, back_buffer, gc, tip_x + 8, tip_y + 32, line2, strlen(line2));
        XDrawString(dpy, back_buffer, gc, tip_x + 8, tip_y + 48, line3, strlen(line3));
    }

    XCopyArea(dpy, back_buffer, win, gc, 0, 0, win_w, win_h, 0, 0);
}

int main(int argc, char **argv) {
    page_size = sysconf(_SC_PAGESIZE);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Unable to open X display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Colormap cmap = DefaultColormap(dpy, screen);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
                                     50, 50, WIN_WIDTH, WIN_HEIGHT, 1,
                                     BlackPixel(dpy, screen), WhitePixel(dpy, screen));

    XStoreName(dpy, win, "FreeBSD Dynamic Memory Treemap");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | PointerMotionMask | ButtonPressMask | StructureNotifyMask);
    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, NULL);
    unsigned long dark_slate = parse_hex_color(dpy, cmap, "#0F172A");
    unsigned long tooltip_yellow = parse_hex_color(dpy, cmap, "#FEF08A");

    int win_w = WIN_WIDTH, win_h = WIN_HEIGHT;
    int mouse_x = -1, mouse_y = -1;

    collect_memory_data(dpy, cmap);

    int x11_fd = ConnectionNumber(dpy);
    fd_set in_fds;
    struct timeval tv;

    XEvent ev;
    int needs_redraw = 1;

    while (1) {
        FD_ZERO(&in_fds);
        FD_SET(x11_fd, &in_fds);
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        int retval = select(x11_fd + 1, &in_fds, NULL, NULL, &tv);

        if (retval == 0) {
            collect_memory_data(dpy, cmap);
            needs_redraw = 1;
        }

        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            if (ev.type == ConfigureNotify) {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
                needs_redraw = 1;
            } else if (ev.type == MotionNotify) {
                mouse_x = ev.xmotion.x;
                mouse_y = ev.xmotion.y;
                needs_redraw = 1;
            } else if (ev.type == ButtonPress) {
                if (ev.xbutton.button == Button1) {
                    for (int i = 0; i < node_count; i++) {
                        if (ev.xbutton.x >= nodes[i].x && ev.xbutton.x < (nodes[i].x + nodes[i].w) &&
                            ev.xbutton.y >= nodes[i].y && ev.xbutton.y < (nodes[i].y + nodes[i].h)) {
                            zoomed_node_idx = i;
                            needs_redraw = 1;
                            break;
                        }
                    }
                } else if (ev.xbutton.button == Button3) {
                    zoomed_node_idx = -1;
                    needs_redraw = 1;
                }
            } else if (ev.type == Expose && ev.xexpose.count == 0) {
                needs_redraw = 1;
            } else if (ev.type == KeyPress) {
                KeySym k = XLookupKeysym(&ev.xkey, 0);
                if (k == XK_q || k == XK_Q) {
                    if (back_buffer) XFreePixmap(dpy, back_buffer);
                    XCloseDisplay(dpy);
                    return 0;
                } else if (k == XK_minus || k == XK_KP_Subtract) {
                    if (removed_blocks_count < node_count - 1) {
                        removed_blocks_count++;
                        zoomed_node_idx = -1;
                        needs_redraw = 1;
                    }
                } else if (k == XK_plus || k == XK_equal || k == XK_KP_Add) {
                    if (removed_blocks_count > 0) {
                        removed_blocks_count--;
                        zoomed_node_idx = -1;
                        needs_redraw = 1;
                    }
                } else if (k == XK_Escape || k == XK_BackSpace) {
                    zoomed_node_idx = -1;
                    needs_redraw = 1;
                }
            }
        }

        if (needs_redraw) {
            draw_treemap(dpy, win, gc, win_w, win_h, mouse_x, mouse_y, dark_slate, tooltip_yellow);
            needs_redraw = 0;
        }
    }

    return 0;
}
