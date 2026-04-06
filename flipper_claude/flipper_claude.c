/* flipper_claude.c — Claude Code Flipper Zero notification + approval UI
 * 7 screens: IDLE / THINK / BASH / TOOL / PERM / APPROVED / DENIED
 * Character drawn at 2× pixel scale.  Animation via furi_timer @ 50 ms.
 */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <string.h>
#include <stdlib.h>

#define TAG      "FC"
#define UART_CH  FuriHalSerialIdUsart
#define SW       128
#define SH        64

/* Character at 2× scale: body 28×10 px, each leg row 28×2 px */
#define CW  28
#define CH  10

/* Alert screen geometry */
#define ALERT_CX   2
#define ALERT_CY  15
#define BOX_X     37   /* ALERT_CX + CW + 7 */
#define BOX_Y     10
#define BOX_W     89   /* SW - BOX_X - 2 */
#define BOX_H     26

/* Integer trig: ×64 fixed-point, 12 steps = 30°/step */
static const int8_t SIN64[12] = { 0, 32, 56, 64, 56, 32,  0,-32,-56,-64,-56,-32};
static const int8_t COS64[12] = {64, 56, 32,  0,-32,-56,-64,-56,-32,  0, 32, 56};

/* Leg cycle [1,2,2,1] – each phase lasts 3 ticks (150 ms ≈ 120 ms target) */
static const uint8_t LEG_CYC[4] = {1, 2, 2, 1};

/* Body bitmap 5 rows × 14 cols */
static const uint8_t BODY[5][14] = {
    {0,0,1,1,1,1,1,1,1,1,1,1,0,0},
    {0,0,1,0,1,1,1,1,1,1,0,1,0,0},
    {0,0,1,0,1,1,1,1,1,1,0,1,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,1,1,1,1,1,1,1,1,1,1,0,0},
};
/* Leg bitmap 1 row × 14 cols */
static const uint8_t LEG[14] = {0,0,1,0,1,0,0,0,0,1,0,1,0,0};

/* ZZZ art 4 rows × 5 cols, drawn at 3×3 px/pixel */
static const uint8_t ZZZ[4][5] = {
    {1,1,1,1,1},
    {0,0,0,1,1},
    {0,1,1,0,0},
    {1,1,1,1,1},
};

/* ── Screen enum ──────────────────────────────────────────────────────────── */
typedef enum {
    ScreenIdle,
    ScreenThink,
    ScreenBash,
    ScreenTool,
    ScreenPerm,
    ScreenApproved,
    ScreenDenied,
} AppScreen;

/* ── App state ────────────────────────────────────────────────────────────── */
typedef struct {
    AppScreen screen;
    FuriMutex*        mutex;
    ViewPort*         view_port;
    Gui*              gui;
    FuriTimer*        timer;
    FuriThread*       serial_thread;
    FuriMessageQueue* event_queue;
    NotificationApp*  notifications;

    /* Serial RX ring buffer (power-of-2 size → mask indexing) */
    uint8_t          rx_buf[256];
    volatile size_t  rx_head;
    volatile size_t  rx_tail;
    char             cmd_buf[256];
    size_t           cmd_len;

    /* Alert message + scroll */
    char    message[64];
    int16_t scroll_x;   /* current text scroll position */
    int16_t msg_w;      /* strlen(message)*6 approx px width */

    /* Animation tick – incremented every 50 ms */
    uint32_t tick;

    /* Alert: 0 = DENY selected, 1 = OK selected */
    uint8_t sel;

    bool running;
} AppContext;

/* ── Character drawing helpers ───────────────────────────────────────────── */

static void draw_char(Canvas* canvas, int x, int y, uint8_t legs) {
    for(int r = 0; r < 5; r++)
        for(int c = 0; c < 14; c++)
            if(BODY[r][c])
                canvas_draw_box(canvas, x + c*2, y + r*2, 2, 2);
    for(int l = 0; l < legs; l++)
        for(int c = 0; c < 14; c++)
            if(LEG[c])
                canvas_draw_box(canvas, x + c*2, y + CH + l*2, 2, 2);
}

static void draw_x(Canvas* canvas, int cx, int cy) {
    /* Bold X via two parallel diagonal line pairs */
    canvas_draw_line(canvas, cx-3, cy-3, cx+3, cy+3);
    canvas_draw_line(canvas, cx-2, cy-3, cx+3, cy+2);
    canvas_draw_line(canvas, cx+3, cy-3, cx-3, cy+3);
    canvas_draw_line(canvas, cx+2, cy-3, cx-3, cy+2);
}

/* ── Screen 1: IDLE ──────────────────────────────────────────────────────── */

static void draw_idle(Canvas* canvas, AppContext* ctx) {
    uint32_t t   = ctx->tick;
    uint8_t legs = LEG_CYC[(t / 3) % 4];
    int bob      = ((t / 8) % 2) ? 1 : 0;
    int char_x   = (SW - CW) / 2;          /* 50 */
    int char_y   = 8 + (48 - 14) / 2 + bob; /* ~25 */

    /* ── Top bar (inverted) ── */
    canvas_draw_box(canvas, 0, 0, SW, 9);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2,  7, "CLAUDE");
    canvas_draw_str(canvas, 72, 7, "IDLE");

    /* Battery: outline body + terminal + 3 cells */
    canvas_draw_frame(canvas, 115, 2, 10, 5);
    canvas_draw_box(canvas, 125, 3,  2, 3);
    for(int i = 0; i < 3; i++)
        canvas_draw_box(canvas, 116 + i*3, 3, 2, 3);

    /* Signal: 3 ascending bars */
    for(int i = 0; i < 3; i++) {
        int bh = 3 + i*2;
        canvas_draw_box(canvas, 104 + i*4, 7 - bh, 3, bh);
    }
    canvas_set_color(canvas, ColorBlack);

    /* ── Character ── */
    draw_char(canvas, char_x, char_y, legs);

    /* ── Bottom bar ── */
    canvas_draw_line(canvas, 0, 56, SW-1, 56);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 63, "STANDBY");

    /* 3 scrolling dots left→right at y=60, 40 px apart */
    int phase = (int)((t * 2) % (uint32_t)SW);
    for(int i = 0; i < 3; i++) {
        int dx = (phase + i * 40) % SW;
        canvas_draw_box(canvas, dx, 60, 3, 3);
    }
}

/* ── Screen 2: THINK ─────────────────────────────────────────────────────── */

static void draw_think(Canvas* canvas, AppContext* ctx) {
    uint32_t t   = ctx->tick;
    uint8_t legs = LEG_CYC[(t / 3) % 4];
    int char_x   = 4;
    int char_y   = 8 + (48 - 14) / 2; /* 25 */

    /* ── Top bar: outline only ── */
    canvas_draw_line(canvas, 0, 0, SW-1, 0);
    canvas_draw_line(canvas, 0, 8, SW-1, 8);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 7, "THINKING");

    /* ── Character ── */
    draw_char(canvas, char_x, char_y, legs);

    /* ── Thought bubble: anchored at char_x+29, char_y+2 ── */
    int bx = char_x + 29; /* 33 */
    int by = char_y + 2;  /* 27 */
    uint8_t phase = (uint8_t)((t / 10) % 4);

    /* Phase 0+: small 2×2 dot */
    canvas_draw_box(canvas, bx, by, 2, 2);

    if(phase >= 1)
        /* Phase 1+: medium 3×3 dot */
        canvas_draw_box(canvas, bx+5, by-5, 3, 3);

    if(phase >= 2) {
        /* Phase 2+: larger dot + rectangle outline 22×16 */
        canvas_draw_box(canvas, bx+11, by-11, 3, 3);
        canvas_draw_frame(canvas, bx+11, by-27, 22, 16);
    }

    if(phase >= 3) {
        /* Phase 3: ZZZ inside the rect (5 cols × 4 rows @ 3×3 px) */
        int zx = bx + 13;
        int zy = by - 25;
        for(int r = 0; r < 4; r++)
            for(int c = 0; c < 5; c++)
                if(ZZZ[r][c])
                    canvas_draw_box(canvas, zx + c*3, zy + r*3, 3, 3);
    }

    /* ── Bottom bar ── */
    canvas_draw_line(canvas, 0, 56, SW-1, 56);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 63, "PROCESSING");
}

/* ── Screens 3/4/5: BASH / TOOL / PERM alert (shared) ───────────────────── */

static void draw_alert(Canvas* canvas, AppContext* ctx,
                       const char* top_label, const char* badge_label) {
    uint32_t t   = ctx->tick;
    uint8_t legs = LEG_CYC[(t / 3) % 4];
    bool arm_up  = (bool)((t / 4) % 2);   /* arm frame: toggle every 200 ms */
    bool blink   = (bool)((t / 5) % 2);   /* 250 ms blink */

    int cx = ALERT_CX;
    int cy = ALERT_CY;

    /* ── Top bar (inverted) ── */
    canvas_draw_box(canvas, 0, 0, SW, 9);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 7, top_label);
    canvas_set_color(canvas, ColorBlack);

    /* ── Character ── */
    draw_char(canvas, cx, cy, legs);

    /* Arms: 3 px wide, 4 px outside char edges.
     * arm_up=true  → left at rows 1-2 (y+2), right at rows 3-4 (y+6)
     * arm_up=false → swapped                                          */
    int la_y = arm_up ? cy+2 : cy+6;
    int ra_y = arm_up ? cy+6 : cy+2;
    canvas_draw_box(canvas, cx - 4,      la_y, 3, 4);   /* may clip left */
    canvas_draw_box(canvas, cx + CW + 4, ra_y, 3, 4);

    /* Exclamation mark – blinks on arm_up frames */
    if(arm_up && blink) {
        int ex = cx + 13;
        int ey = cy - 6;
        canvas_draw_box(canvas, ex, ey,     2, 2); /* segment 1 */
        canvas_draw_box(canvas, ex, ey + 2, 2, 2); /* segment 2 */
        /* gap at ey+4 */
        canvas_draw_box(canvas, ex, ey + 5, 2, 2); /* dot */
    }

    /* ── Command box ── */
    canvas_draw_frame(canvas, BOX_X, BOX_Y, BOX_W, BOX_H);

    /* Badge bar (top 8 px of box, inverted) */
    canvas_draw_box(canvas, BOX_X+1, BOX_Y+1, BOX_W-2, 8);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, BOX_X+3, BOX_Y+7, badge_label);
    canvas_set_color(canvas, ColorBlack);

    /* Scrolling message text at y = BOX_Y+19, clipped to box x bounds */
    int sx = (int)ctx->scroll_x;
    if(sx < BOX_X + BOX_W && sx + (int)ctx->msg_w > BOX_X) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, sx, BOX_Y + 19, ctx->message);
        /* Redraw box left/right borders to clip text bleed */
        canvas_draw_line(canvas, BOX_X,           BOX_Y, BOX_X,           BOX_Y + BOX_H);
        canvas_draw_line(canvas, BOX_X + BOX_W-1, BOX_Y, BOX_X + BOX_W-1, BOX_Y + BOX_H);
    }

    /* ── Bottom selector ── */
    canvas_draw_line(canvas, 0,  46, SW-1, 46); /* horizontal divider */
    canvas_draw_line(canvas, 64, 46, 64,   63); /* vertical centre */

    /* DENY button (x=1, w=62) */
    if(ctx->sel == 0) {
        canvas_draw_box(canvas, 1, 47, 62, 16);
        canvas_set_color(canvas, ColorWhite);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 8, 58, "< DENY");
        canvas_set_color(canvas, ColorBlack);
        if(blink) canvas_draw_box(canvas, 1, 61, 62, 2);
    } else {
        canvas_draw_frame(canvas, 1, 47, 62, 16);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 8, 58, "< DENY");
    }

    /* OK button (x=65, w=62) */
    if(ctx->sel == 1) {
        canvas_draw_box(canvas, 65, 47, 62, 16);
        canvas_set_color(canvas, ColorWhite);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 82, 58, "OK >");
        canvas_set_color(canvas, ColorBlack);
        if(blink) canvas_draw_box(canvas, 65, 61, 62, 2);
    } else {
        canvas_draw_frame(canvas, 65, 47, 62, 16);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 82, 58, "OK >");
    }
}

/* ── Screen 6: APPROVED ──────────────────────────────────────────────────── */

static void draw_approved(Canvas* canvas, AppContext* ctx) {
    uint32_t t = ctx->tick;

    /* Bounce ±3 px */
    int bounce  = (int)(SIN64[(t * 2) % 12]) * 3 / 64;
    int char_x  = (SW - CW) / 2;   /* 50 */
    int char_y  = 23 + bounce;
    int ray_cx  = char_x + CW / 2; /* 64 */
    int ray_cy  = char_y + CH / 2;

    canvas_draw_frame(canvas, 0, 0, SW, SH);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 20, 15, "APPROVED");

    draw_char(canvas, char_x, char_y, 2);

    /* 6 rotating aura rays, inner r=26 outer r=32 */
    int rot = (int)((t / 3) % 12);
    for(int i = 0; i < 6; i++) {
        int idx = (rot + i * 2) % 12;
        int x1 = ray_cx + SIN64[idx] * 26 / 64;
        int y1 = ray_cy - COS64[idx] * 26 / 64;
        int x2 = ray_cx + SIN64[idx] * 32 / 64;
        int y2 = ray_cy - COS64[idx] * 32 / 64;
        canvas_draw_line(canvas, x1, y1, x2, y2);
    }

    /* 4 sparkle dots at corners, alternating pairs every 4 ticks */
    bool sp = (bool)((t / 4) % 2);
    if(sp) {
        canvas_draw_box(canvas,  14, 16, 3, 3);
        canvas_draw_box(canvas,  10, 46, 3, 3);
    } else {
        canvas_draw_box(canvas, 108, 14, 3, 3);
        canvas_draw_box(canvas, 110, 44, 3, 3);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 34, 61, "CMD EXEC");
}

/* ── Screen 7: DENIED ────────────────────────────────────────────────────── */

static void draw_denied(Canvas* canvas, AppContext* ctx) {
    uint32_t t = ctx->tick;

    /* Fast horizontal shake ±2 px */
    int shake  = (int)(SIN64[(t * 3) % 12]) * 2 / 64;
    bool blink = (bool)((t / 5) % 2);

    canvas_draw_frame(canvas, 0, 0, SW, SH);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 32, 15, "DENIED");

    draw_char(canvas, (SW - CW) / 2 + shake, 23, 1);

    /* 3 blinking X marks */
    if(blink) {
        draw_x(canvas,  16, 30);
        draw_x(canvas, 102, 30);
        draw_x(canvas,  64, 52);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 36, 61, "ABORTED");
}

/* ── Draw callback ───────────────────────────────────────────────────────── */

static void draw_cb(Canvas* canvas, void* ctx_ptr) {
    AppContext* ctx = ctx_ptr;
    furi_mutex_acquire(ctx->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    switch(ctx->screen) {
    case ScreenIdle:     draw_idle(canvas, ctx);                             break;
    case ScreenThink:    draw_think(canvas, ctx);                            break;
    case ScreenBash:     draw_alert(canvas, ctx, "! BASH", "BASH");          break;
    case ScreenTool:     draw_alert(canvas, ctx, "? TOOL", "TOOL");          break;
    case ScreenPerm:     draw_alert(canvas, ctx, "* PERM", "PERM");          break;
    case ScreenApproved: draw_approved(canvas, ctx);                         break;
    case ScreenDenied:   draw_denied(canvas, ctx);                           break;
    }

    furi_mutex_release(ctx->mutex);
}

/* ── set_screen helper ───────────────────────────────────────────────────── */

static void set_screen(AppContext* ctx, AppScreen screen, const char* msg) {
    furi_mutex_acquire(ctx->mutex, FuriWaitForever);
    ctx->screen = screen;
    ctx->tick   = 0;
    ctx->sel    = 1; /* default OK selected */

    if(msg) {
        strncpy(ctx->message, msg, sizeof(ctx->message) - 1);
        ctx->message[sizeof(ctx->message) - 1] = '\0';
        ctx->msg_w = (int16_t)(strlen(ctx->message) * 6);
    } else {
        ctx->message[0] = '\0';
        ctx->msg_w = 0;
    }

    if(screen == ScreenBash || screen == ScreenTool || screen == ScreenPerm)
        ctx->scroll_x = (int16_t)(BOX_X + BOX_W); /* start off right edge */

    furi_mutex_release(ctx->mutex);
}

/* ── Timer callback (50 ms / 20 fps) ────────────────────────────────────── */

static void timer_cb(void* ctx_ptr) {
    AppContext* ctx = ctx_ptr;
    furi_mutex_acquire(ctx->mutex, FuriWaitForever);

    ctx->tick++;

    /* Advance scroll for alert screens */
    if(ctx->screen == ScreenBash || ctx->screen == ScreenTool || ctx->screen == ScreenPerm) {
        ctx->scroll_x--;
        if(ctx->scroll_x < BOX_X - ctx->msg_w - 10)
            ctx->scroll_x = (int16_t)(BOX_X + BOX_W);
    }

    /* Auto-return from APPROVED/DENIED to IDLE after ~2 s (40 ticks) */
    if((ctx->screen == ScreenApproved || ctx->screen == ScreenDenied) && ctx->tick > 40) {
        ctx->screen = ScreenIdle;
        ctx->tick   = 0;
    }

    furi_mutex_release(ctx->mutex);
    view_port_update(ctx->view_port);
}

/* ── Input callback ──────────────────────────────────────────────────────── */

static void input_cb(InputEvent* event, void* ctx_ptr) {
    AppContext* ctx = ctx_ptr;
    furi_message_queue_put(ctx->event_queue, event, 0);
}

/* ── Serial RX IRQ callback ──────────────────────────────────────────────── */

static void uart_rx_cb(FuriHalSerialHandle* handle,
                       FuriHalSerialRxEvent event,
                       void* ctx_ptr) {
    UNUSED(handle);
    AppContext* ctx = ctx_ptr;
    if(event == FuriHalSerialRxEventData) {
        uint8_t b    = furi_hal_serial_async_rx(handle);
        size_t  next = (ctx->rx_head + 1) % sizeof(ctx->rx_buf);
        if(next != ctx->rx_tail) {
            ctx->rx_buf[ctx->rx_head] = b;
            ctx->rx_head = next;
        }
    }
}

/* ── Command parser ──────────────────────────────────────────────────────── */

static void handle_cmd(AppContext* ctx, const char* line) {
    FURI_LOG_I(TAG, "CMD: %s", line);
    if(strcmp(line, "IDLE") == 0) {
        set_screen(ctx, ScreenIdle, NULL);
    } else if(strcmp(line, "THINK") == 0) {
        set_screen(ctx, ScreenThink, NULL);
    } else if(strncmp(line, "NOTIFY:BASH:", 12) == 0) {
        set_screen(ctx, ScreenBash, line + 12);
    } else if(strncmp(line, "NOTIFY:TOOL:", 12) == 0) {
        set_screen(ctx, ScreenTool, line + 12);
    } else if(strncmp(line, "NOTIFY:PERM:", 12) == 0) {
        set_screen(ctx, ScreenPerm, line + 12);
    }
    /* Unknown commands are silently ignored */
}

/* ── Serial worker thread ────────────────────────────────────────────────── */

static FuriHalSerialHandle* g_serial_handle = NULL;

static void serial_send(const char* s) {
    if(g_serial_handle)
        furi_hal_serial_tx(g_serial_handle, (const uint8_t*)s, strlen(s));
}

static int32_t serial_worker(void* ctx_ptr) {
    AppContext* ctx = ctx_ptr;

    g_serial_handle = furi_hal_serial_control_acquire(UART_CH);
    furi_check(g_serial_handle);
    furi_hal_serial_init(g_serial_handle, 115200);
    furi_hal_serial_async_rx_start(g_serial_handle, uart_rx_cb, ctx, false);
    FURI_LOG_I(TAG, "serial_worker started");

    while(ctx->running) {
        while(ctx->rx_head != ctx->rx_tail) {
            uint8_t b = ctx->rx_buf[ctx->rx_tail];
            ctx->rx_tail = (ctx->rx_tail + 1) % sizeof(ctx->rx_buf);

            if(b == '\n' || b == '\r') {
                if(ctx->cmd_len > 0) {
                    ctx->cmd_buf[ctx->cmd_len] = '\0';
                    handle_cmd(ctx, ctx->cmd_buf);
                    ctx->cmd_len = 0;
                }
            } else if(ctx->cmd_len < sizeof(ctx->cmd_buf) - 1) {
                ctx->cmd_buf[ctx->cmd_len++] = (char)b;
            }
        }
        furi_delay_ms(10);
    }

    furi_hal_serial_async_rx_stop(g_serial_handle);
    furi_hal_serial_deinit(g_serial_handle);
    furi_hal_serial_control_release(g_serial_handle);
    g_serial_handle = NULL;
    FURI_LOG_I(TAG, "serial_worker stopped");
    return 0;
}

/* ── App entry point ─────────────────────────────────────────────────────── */

int32_t flipper_claude_app(void* p) {
    UNUSED(p);

    AppContext* ctx = malloc(sizeof(AppContext));
    furi_check(ctx);
    memset(ctx, 0, sizeof(AppContext));
    ctx->running = true;
    ctx->screen  = ScreenIdle;
    ctx->sel     = 1;

    ctx->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(ctx->mutex);

    ctx->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    furi_check(ctx->event_queue);

    ctx->view_port = view_port_alloc();
    view_port_draw_callback_set(ctx->view_port, draw_cb, ctx);
    view_port_input_callback_set(ctx->view_port, input_cb, ctx);

    ctx->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(ctx->gui, ctx->view_port, GuiLayerFullscreen);

    ctx->notifications = furi_record_open(RECORD_NOTIFICATION);

    /* 50 ms animation timer */
    ctx->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, ctx);
    furi_timer_start(ctx->timer, 50);

    /* Serial thread */
    ctx->serial_thread =
        furi_thread_alloc_ex("FCSerial", 1024, serial_worker, ctx);
    furi_thread_start(ctx->serial_thread);

    FURI_LOG_I(TAG, "flipper_claude_app running");

    /* ── Main event loop ── */
    InputEvent ev;
    while(ctx->running) {
        if(furi_message_queue_get(ctx->event_queue, &ev, 100) != FuriStatusOk)
            continue;
        if(ev.type != InputTypeShort && ev.type != InputTypeRepeat)
            continue;

        furi_mutex_acquire(ctx->mutex, FuriWaitForever);
        AppScreen screen = ctx->screen;
        furi_mutex_release(ctx->mutex);

        bool is_alert = (screen == ScreenBash ||
                         screen == ScreenTool ||
                         screen == ScreenPerm);

        /* Down button: cycle through all screens for testing */
        if(ev.key == InputKeyDown) {
            furi_mutex_acquire(ctx->mutex, FuriWaitForever);
            AppScreen next = (AppScreen)((ctx->screen + 1) % 7);
            furi_mutex_release(ctx->mutex);
            /* Use a test message for alert screens */
            const char* test_msg = "rm -rf /tmp/old_build";
            set_screen(ctx, next, test_msg);
            continue;
        }

        if(ev.key == InputKeyBack) {
            if(is_alert) {
                serial_send("CANCEL\n");
                set_screen(ctx, ScreenIdle, NULL);
            } else {
                ctx->running = false;
            }
        } else if(is_alert) {
            if(ev.key == InputKeyLeft) {
                furi_mutex_acquire(ctx->mutex, FuriWaitForever);
                ctx->sel = 0;
                furi_mutex_release(ctx->mutex);
            } else if(ev.key == InputKeyRight) {
                furi_mutex_acquire(ctx->mutex, FuriWaitForever);
                ctx->sel = 1;
                furi_mutex_release(ctx->mutex);
            } else if(ev.key == InputKeyOk) {
                furi_mutex_acquire(ctx->mutex, FuriWaitForever);
                uint8_t sel = ctx->sel;
                furi_mutex_release(ctx->mutex);

                if(sel == 0) {
                    serial_send("DENY\n");
                    notification_message(ctx->notifications, &sequence_error);
                    set_screen(ctx, ScreenDenied, NULL);
                } else {
                    serial_send("OK\n");
                    notification_message(ctx->notifications, &sequence_success);
                    set_screen(ctx, ScreenApproved, NULL);
                }
            }
        }
    }

    /* ── Cleanup ── */
    furi_timer_stop(ctx->timer);
    furi_timer_free(ctx->timer);

    ctx->running = false;
    furi_thread_join(ctx->serial_thread);
    furi_thread_free(ctx->serial_thread);

    furi_record_close(RECORD_NOTIFICATION);
    gui_remove_view_port(ctx->gui, ctx->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(ctx->view_port);
    furi_message_queue_free(ctx->event_queue);
    furi_mutex_free(ctx->mutex);
    free(ctx);

    return 0;
}
