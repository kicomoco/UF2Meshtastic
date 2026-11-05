// F1 - Conversations list and chat view (mirrors the Android "Messages" tab).
#include <stdio.h>
#include <string.h>

#include "ui/ui.h"
#include "mesh/mesh.h"
#include "mesh/ble_link.h"

#define MAX_CONVS 64
#define ROW_H 36
#define BUBBLE_MAX_W 236
#define BUBBLE_CHARS ((BUBBLE_MAX_W - 10) / 6)
#define COMPOSE_H 20

static conv_t convs[MAX_CONVS];
static int conv_count;
static listnav_t nav;

static bool in_chat;
static uint32_t chat_key;
static int scroll_px;  // 0 = newest at the bottom
static int max_scroll;
static textbox_t compose;
static char title_buf[48];

static void refresh_convs(void) { conv_count = mesh_conversations(convs, MAX_CONVS); }

bool messages_is_open(uint32_t key) { return in_chat && chat_key == key; }

void messages_open_conversation(uint32_t key) {
    in_chat = true;
    chat_key = key;
    scroll_px = 0;
    tb_init(&compose, MAX_TEXT_BYTES, NULL);
    mesh_mark_read(key);
    ui_goto_tab(TAB_MESSAGES);
}

static void enter(void) {
    refresh_convs();
    if (in_chat) mesh_mark_read(chat_key);
}

static const char *dyn_title(void) {
    if (!in_chat) return NULL;
    mesh_conv_title(chat_key, title_buf, sizeof(title_buf));
    return title_buf;
}

static void status_text(const msg_t *m, char *out, size_t n, uint16_t *col) {
    *col = C_DIM;
    switch (m->status) {
    case MSG_QUEUED: snprintf(out, n, "Queued"); break;
    case MSG_SENT: snprintf(out, n, "Sent"); break;
    case MSG_ACKED:
        snprintf(out, n, m->to == NODENUM_BROADCAST ? "Heard by mesh" : "Acknowledged");
        *col = C_ACCENT;
        break;
    case MSG_DELIVERED:
        snprintf(out, n, "Delivered");
        *col = C_ACCENT;
        break;
    case MSG_FAILED:
        if (m->to == NODENUM_BROADCAST && (m->error == 0 || m->error == 3 || m->error == 5))
            snprintf(out, n, "Not heard by mesh");
        else
            snprintf(out, n, "Failed: %s", mesh_routing_error_name(m->error ? m->error : 3));
        *col = C_RED;
        break;
    default: out[0] = 0; break;
    }
}

// ------------------------------------------------------------------ list

static void draw_list(void) {
    refresh_convs();
    nav.count = conv_count;
    int visible = CONTENT_H / ROW_H;
    list_clamp(&nav, visible);
    if (!conv_count) {
        gfx_text_center(LCD_W / 2, 120, "No conversations yet", FONT, C_DIM, TRANSPARENT);
        gfx_text_center(LCD_W / 2, 136, "Connect to a radio with F5", FONT, C_DIM, TRANSPARENT);
        return;
    }
    for (int i = 0; i < visible && nav.top + i < conv_count; i++) {
        int idx = nav.top + i;
        const conv_t *c = &convs[idx];
        int y = CONTENT_Y + i * ROW_H;
        bool sel = idx == nav.sel;
        if (sel) gfx_fill(0, y, LCD_W, ROW_H, C_SURFACE);
        gfx_hline(52, y + ROW_H - 1, LCD_W - 52, C_SURFACE);

        if (CONV_IS_CHANNEL(c->key)) {
            gfx_round_fill(6, y + 6, 40, 24, 4, C_ACCENT_DARK);
            char b[4];
            snprintf(b, sizeof(b), "%lu", (unsigned long)CONV_CHANNEL_INDEX(c->key));
            gfx_text_center(26, y + 10, b, FONT_BIG, C_TEXT, TRANSPARENT);
        } else {
            draw_node_badge(6, y + 6, 40, 24, c->key, FONT);
        }

        char title[48];
        mesh_conv_title(c->key, title, sizeof(title));
        const msg_t *m = mesh_msg(c->last_msg);
        int right = LCD_W - 6;
        if (m) {
            char t[16];
            ui_format_datetime(m->time, t, sizeof(t));
            gfx_text_right(right, y + 4, t, FONT, C_DIM, TRANSPARENT);
            right -= gfx_text_width(t, FONT) + 6;
        }
        gfx_text_fit(52, y + 4, title, right - 52, FONT, C_TEXT, TRANSPARENT);

        int pr = LCD_W - 6;
        if (c->unread) {
            char u[8];
            snprintf(u, sizeof(u), "%d", c->unread > 99 ? 99 : c->unread);
            int w = gfx_text_width(u, FONT) + 10;
            gfx_round_fill(pr - w, y + 19, w, 13, 6, C_ACCENT);
            gfx_text_center(pr - w / 2, y + 20, u, FONT, C_ON_ACCENT, TRANSPARENT);
            pr -= w + 6;
        }
        if (m) {
            char prev[64];
            char who[8];
            if (m->from == g_mesh.my_num) snprintf(who, sizeof(who), "You");
            else mesh_node_label(m->from, who, sizeof(who));
            if (CONV_IS_CHANNEL(c->key) || m->from == g_mesh.my_num) snprintf(prev, sizeof(prev), "%s: %s", who, m->text);
            else snprintf(prev, sizeof(prev), "%s", m->text);
            for (char *p = prev; *p; p++) if (*p == '\n') *p = ' ';
            gfx_text_fit(52, y + 20, prev, pr - 52, FONT, c->unread ? C_TEXT : C_DIM, TRANSPARENT);
        } else {
            gfx_text(52, y + 20, "No messages", FONT, C_FAINT, TRANSPARENT);
        }
    }
    draw_scrollbar(LCD_W - 3, CONTENT_Y, CONTENT_H, conv_count, nav.top, visible);
}

// ------------------------------------------------------------------ chat

static int bubble_layout(const msg_t *m, wrap_line_t *lines, int *nlines, int *bw) {
    bool mine = m->from == g_mesh.my_num;
    *nlines = wrap_text(m->text, BUBBLE_CHARS, lines, 12);
    int w = 0;
    for (int i = 0; i < *nlines; i++) {
        char tmp[64];
        int l = lines[i].len < (int)sizeof(tmp) - 1 ? lines[i].len : (int)sizeof(tmp) - 1;
        memcpy(tmp, m->text + lines[i].start, (size_t)l);
        tmp[l] = 0;
        int lw = gfx_text_width(tmp, FONT);
        if (lw > w) w = lw;
    }
    int footer_w = 100;
    if (mine) {
        char t[16], st[40];
        uint16_t sc;
        ui_format_datetime(m->time, t, sizeof(t));
        status_text(m, st, sizeof(st), &sc);
        footer_w = gfx_text_width(t, FONT) + 6 + gfx_text_width(st, FONT) + 6;
    }
    if (w < footer_w) w = footer_w;
    if (!mine && w < 120) w = 120;
    *bw = w + 10;
    if (*bw > BUBBLE_MAX_W) *bw = BUBBLE_MAX_W;
    // header (others) + lines + footer + padding
    return (mine ? 0 : 12) + *nlines * 12 + 12 + 6;
}

static void draw_chat(void) {
    int area_top = CONTENT_Y;
    int area_bottom = CONTENT_BOTTOM - COMPOSE_H - 2;
    gfx_clip(0, area_top, LCD_W, area_bottom - area_top);

    int y = area_bottom - 4 + scroll_px;
    int total = 0;
    int count = mesh_msg_count();
    bool any = false;
    for (int i = count - 1; i >= 0; i--) {
        const msg_t *m = mesh_msg(i);
        if (mesh_msg_conv_key(m) != chat_key) continue;
        any = true;
        wrap_line_t lines[12];
        int nl, bw;
        int h = bubble_layout(m, lines, &nl, &bw);
        total += h + 6;
        int top = y - h;
        if (top < area_bottom && y > area_top) {
            bool mine = m->from == g_mesh.my_num;
            int x = mine ? LCD_W - 8 - bw : (CONV_IS_CHANNEL(chat_key) ? 30 : 8);
            gfx_round_fill(x, top, bw, h, 6, mine ? C_BUBBLE_ME : C_BUBBLE_THEM);
            int ty = top + 3;
            if (!mine) {
                if (CONV_IS_CHANNEL(chat_key)) draw_node_badge(2, top, 26, 14, m->from, FONT);
                char name[40];
                mesh_node_longname(m->from, name, sizeof(name));
                uint16_t nc = RGB888_TO_565(mesh_node_color(m->from));
                if (gfx_contrast(nc) == C_WHITE) nc = C_ACCENT;
                gfx_text_fit(x + 5, ty, name, bw - 10, FONT, nc, TRANSPARENT);
                ty += 12;
            }
            for (int l = 0; l < nl; l++) {
                gfx_text_b(x + 5, ty, m->text + lines[l].start, lines[l].len, FONT, C_TEXT, TRANSPARENT);
                ty += 12;
            }
            char t[16];
            ui_format_datetime(m->time, t, sizeof(t));
            if (mine) {
                char st[40];
                uint16_t sc;
                status_text(m, st, sizeof(st), &sc);
                gfx_text(x + 5, ty, t, FONT, C_DIM, TRANSPARENT);
                gfx_text_fit(x + 5 + gfx_text_width(t, FONT) + 6, ty, st, bw - 16 - gfx_text_width(t, FONT), FONT, sc,
                             TRANSPARENT);
            } else {
                char info[40];
                if (m->hops) snprintf(info, sizeof(info), "%s  %d hop%s", t, m->hops, m->hops == 1 ? "" : "s");
                else snprintf(info, sizeof(info), "%s  SNR %.1f", t, m->snr4 / 4.0);
                gfx_text_fit(x + 5, ty, info, bw - 10, FONT, C_DIM, TRANSPARENT);
            }
        }
        y = top - 6;
    }
    max_scroll = total > (area_bottom - area_top) ? total - (area_bottom - area_top) + 8 : 0;
    if (!any) {
        gfx_text_center(LCD_W / 2, 120, "No messages yet - say hello!", FONT, C_DIM, TRANSPARENT);
    }
    if (scroll_px > 0) {
        gfx_round_fill(LCD_W - 70, area_bottom - 16, 64, 14, 6, C_SURFACE2);
        gfx_text(LCD_W - 64, area_bottom - 15, "v newer", FONT, C_TEXT, TRANSPARENT);
    }
    gfx_clip(0, CONTENT_Y, LCD_W, CONTENT_H);

    // Compose line
    int cy = CONTENT_BOTTOM - COMPOSE_H;
    gfx_fill(0, cy - 2, LCD_W, COMPOSE_H + 2, C_BAR);
    bool can_send = g_mesh.link_up;
    if (compose.len == 0) {
        gfx_fill(4, cy + 2, LCD_W - 50, 16, C_SURFACE);
        gfx_text(7, cy + 4, can_send ? "Type a message..." : "Not connected", FONT, C_FAINT, TRANSPARENT);
    } else {
        tb_draw(&compose, 4, cy + 2, LCD_W - 50, FONT, C_TEXT, C_SURFACE, true);
    }
    char cnt[12];
    snprintf(cnt, sizeof(cnt), "%d", MAX_TEXT_BYTES - compose.len);
    gfx_text_right(LCD_W - 4, cy + 4, cnt, FONT, compose.len > MAX_TEXT_BYTES - 20 ? C_YELLOW : C_DIM, TRANSPARENT);
}

static void send_compose(void) {
    if (!compose.len) return;
    if (!g_mesh.link_up) {
        ui_toast("Not connected to a radio");
        return;
    }
    uint32_t to;
    uint8_t ch;
    if (CONV_IS_CHANNEL(chat_key)) {
        to = NODENUM_BROADCAST;
        ch = (uint8_t)CONV_CHANNEL_INDEX(chat_key);
    } else {
        to = chat_key;
        const node_t *n = mesh_node_find(to);
        ch = n ? n->channel : 0;
    }
    if (mesh_send_text(to, ch, compose.buf)) {
        tb_clear(&compose);
        scroll_px = 0;
    } else {
        ui_toast("Send failed (queue full?)");
    }
}

static bool key(const key_event_t *ev) {
    if (!in_chat) {
        refresh_convs();
        nav.count = conv_count;
        if (list_key(&nav, ev, CONTENT_H / ROW_H)) return true;
        if (ev->key == KEY_ENTER && conv_count) {
            messages_open_conversation(convs[nav.sel].key);
            return true;
        }
        return false;
    }
    int page = CONTENT_H - COMPOSE_H - 40;
    switch (ev->key) {
    case KEY_ESC:
        if (compose.len && (ev->mods & MOD_SHIFT)) {
            tb_clear(&compose);
        } else {
            in_chat = false;
            refresh_convs();
        }
        return true;
    case KEY_UP: scroll_px += 24; break;
    case KEY_DOWN: scroll_px -= 24; break;
    case KEY_PAGE_UP: scroll_px += page; break;
    case KEY_PAGE_DOWN: scroll_px -= page; break;
    case KEY_ENTER:
        send_compose();
        return true;
    default: {
        int r = tb_key(&compose, ev);
        return r != TB_UNHANDLED;
    }
    }
    if (scroll_px > max_scroll) scroll_px = max_scroll;
    if (scroll_px < 0) scroll_px = 0;
    return true;
}

static void draw(void) {
    if (in_chat) draw_chat();
    else draw_list();
}

const screen_t scr_messages = {
    .tab_label = "Chat",
    .title = "Messages",
    .enter = enter,
    .draw = draw,
    .key = key,
    .dyn_title = dyn_title,
};
