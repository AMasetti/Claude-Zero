#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "FlipperClaude"
#define UART_CH FuriHalSerialIdUsart
#define RX_BUF_SIZE 256
#define CMD_BUF_SIZE 256
#define CMD_TEXT_SIZE 128
#define ANIM_FPS_PERIOD 100  // ms per frame tick
#define AUTO_RETURN_FRAMES 20  // frames before returning to idle from approved/denied

// ---------------------------------------------------------------------------
// Pixel bitmaps (exact as specified)
// ---------------------------------------------------------------------------

static const uint8_t BODY[5][14] = {
    {0,0,1,1,1,1,1,1,1,1,1,1,0,0},
    {0,0,1,0,1,1,1,1,1,1,0,1,0,0},
    {0,0,1,0,1,1,1,1,1,1,0,1,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,1,1,1,1,1,1,1,1,1,1,0,0},
};

static const uint8_t LEGS[1][14] = {
    {0,0,1,0,1,0,0,0,0,1,0,1,0,0},
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef enum {
    StateIdle,
    StateThinking,
    StateAlert,
    StateWaiting,
    StateApproved,
    StateDenied,
} AppState;

typedef struct {
    AppState state;
    FuriMutex* mutex;
    ViewPort* view_port;
    Gui* gui;
    FuriThread* serial_thread;
    FuriMessageQueue* event_queue;

    // Serial RX ring buffer
    uint8_t rx_buf[RX_BUF_SIZE];
    volatile size_t rx_head;
    volatile size_t rx_tail;

    // Command line buffer
    char cmd_buf[CMD_BUF_SIZE];
    size_t cmd_len;

    // Notification message to display
    char cmd_text[CMD_TEXT_SIZE];
    int16_t scroll_x;    // marquee x position (starts at 128, decrements)
    int16_t text_width;  // pixel width of cmd_text (approx: len * 6)

    // Animation
    uint32_t anim_frame;     // increments each timer tick
    int8_t   walk_dir;       // 1 or -1 for idle walk direction
    int8_t   walk_x;         // current horizontal offset (-10..+10)
    uint8_t  auto_return;    // countdown for approved/denied → idle

    // Progress (for PROGRESS command extension)
    uint8_t  progress_pct;   // 0-100
    char     progress_msg[64];

    bool running;
} AppContext;

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

static void draw_character(Canvas* canvas, int x, int y, bool arms_up, bool legs_spread) {
    // Draw body bitmap
    for(int row = 0; row < 5; row++) {
        for(int col = 0; col < 14; col++) {
            if(BODY[row][col]) {
                canvas_draw_dot(canvas, x + col, y + row);
            }
        }
    }
    // Draw legs (optionally spread for walking)
    for(int col = 0; col < 14; col++) {
        if(LEGS[0][col]) {
            int leg_y = y + 5;
            // Walking: alternate leg spread
            int spread = legs_spread ? 1 : 0;
            if(col < 7)
                canvas_draw_dot(canvas, x + col - spread, leg_y);
            else
                canvas_draw_dot(canvas, x + col + spread, leg_y);
        }
    }
    // Arms (extra pixels on sides of row 3)
    if(arms_up) {
        // Arms raised
        canvas_draw_dot(canvas, x - 1, y + 2);
        canvas_draw_dot(canvas, x - 2, y + 1);
        canvas_draw_dot(canvas, x + 14, y + 2);
        canvas_draw_dot(canvas, x + 15, y + 1);
    } else {
        // Arms down
        canvas_draw_dot(canvas, x - 1, y + 3);
        canvas_draw_dot(canvas, x - 2, y + 4);
        canvas_draw_dot(canvas, x + 14, y + 3);
        canvas_draw_dot(canvas, x + 15, y + 4);
    }
}

static void draw_idle(Canvas* canvas, AppContext* ctx) {
    // Center base: x=57 (128/2 - 14/2), y=24 (64/2 - 6/2 - 8)
    int base_x = 57 + ctx->walk_x;
    // Bob: ±1 every 8 frames
    int bob = ((ctx->anim_frame / 8) % 2 == 0) ? 0 : -1;
    int base_y = 24 + bob;
    bool legs_spread = (ctx->anim_frame / 4) % 2 == 0;
    draw_character(canvas, base_x, base_y, false, legs_spread);
}

static void draw_thinking(Canvas* canvas, AppContext* ctx) {
    int base_x = 57;
    int base_y = 24;
    draw_character(canvas, base_x, base_y, false, false);

    // Thought bubble: small circles ascending to upper-right
    canvas_draw_circle(canvas, base_x + 12, base_y - 4, 2);
    canvas_draw_circle(canvas, base_x + 16, base_y - 8, 3);
    canvas_draw_circle(canvas, base_x + 21, base_y - 13, 4);

    // ZZZ text inside bubble, toggling every 20 frames
    if((ctx->anim_frame / 20) % 2 == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, base_x + 17, base_y - 7, "ZZZ");
    }
}

static void draw_alert(Canvas* canvas, AppContext* ctx) {
    // Notification overlay layout:
    // Top 12px: scrolling command text
    // Middle: character animation (y=13..48)
    // Bottom 16px: button hints

    // --- Scrolling text ---
    if(ctx->cmd_text[0] != '\0') {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, ctx->scroll_x, 10, ctx->cmd_text);
    }

    // --- Character in alert state (centered in middle zone) ---
    int base_x = 57;
    int base_y = 24;
    bool arms_up = (ctx->anim_frame / 4) % 2 == 0;
    draw_character(canvas, base_x, base_y, arms_up, false);

    // "!" above head
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, base_x + 6, base_y - 2, "!");

    // Flash border every 4 frames
    if((ctx->anim_frame / 4) % 2 == 0) {
        canvas_draw_frame(canvas, 0, 0, 128, 64);
    }

    // --- Button hints at bottom ---
    // Left rounded rect: DENY
    canvas_draw_rframe(canvas, 1, 53, 28, 10, 2);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 4, 62, "DENY");

    // Right rounded rect: OK
    canvas_draw_rframe(canvas, 99, 53, 28, 10, 2);
    canvas_draw_str(canvas, 107, 62, "OK");

    // Arrow hints
    canvas_draw_str(canvas, 33, 62, "<");
    canvas_draw_str(canvas, 90, 62, ">");
}

static void draw_waiting(Canvas* canvas, AppContext* ctx) {
    int base_x = 57;
    int base_y = 26;
    draw_character(canvas, base_x, base_y, false, false);

    // Spinning arc above head — 4 positions cycling every 8 frames
    int spin = (ctx->anim_frame / 8) % 4;
    int cx = base_x + 7;
    int cy = base_y - 7;
    // Draw partial arc using dots (approximate quarter-circle highlights)
    switch(spin) {
    case 0: // top
        canvas_draw_dot(canvas, cx,     cy - 5);
        canvas_draw_dot(canvas, cx + 1, cy - 5);
        canvas_draw_dot(canvas, cx - 1, cy - 5);
        break;
    case 1: // right
        canvas_draw_dot(canvas, cx + 5, cy);
        canvas_draw_dot(canvas, cx + 5, cy + 1);
        canvas_draw_dot(canvas, cx + 5, cy - 1);
        break;
    case 2: // bottom
        canvas_draw_dot(canvas, cx,     cy + 5);
        canvas_draw_dot(canvas, cx + 1, cy + 5);
        canvas_draw_dot(canvas, cx - 1, cy + 5);
        break;
    case 3: // left
        canvas_draw_dot(canvas, cx - 5, cy);
        canvas_draw_dot(canvas, cx - 5, cy + 1);
        canvas_draw_dot(canvas, cx - 5, cy - 1);
        break;
    }
    // Draw circle outline
    canvas_draw_circle(canvas, cx, cy, 5);

    // If progress > 0, show progress bar
    if(ctx->progress_pct > 0) {
        int bar_w = (int)(ctx->progress_pct * 100 / 100);  // 0-100 px
        canvas_draw_frame(canvas, 14, 55, 100, 7);
        canvas_draw_box(canvas, 15, 56, bar_w, 5);
        if(ctx->progress_msg[0] != '\0') {
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, 14, 52, ctx->progress_msg);
        }
    } else {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 46, 60, "Waiting...");
    }
}

// Simple sine approximation lookup for jumping
static const int8_t JUMP_LUT[8] = {0, -2, -4, -4, -3, -1, 0, 0};

static void draw_approved(Canvas* canvas, AppContext* ctx) {
    int jump_offset = JUMP_LUT[ctx->anim_frame % 8];
    int base_x = 57;
    int base_y = 24 + jump_offset;
    draw_character(canvas, base_x, base_y, true, false);

    // Sparkles — small 2x2 blocks at frame-seeded positions
    uint32_t seed = ctx->anim_frame;
    for(int i = 0; i < 5; i++) {
        seed = seed * 1664525u + 1013904223u;  // LCG
        int sx = (seed >> 16) % 120 + 4;
        seed = seed * 1664525u + 1013904223u;
        int sy = (seed >> 16) % 40 + 4;
        canvas_draw_box(canvas, sx, sy, 2, 2);
    }

    // "APPROVED" banner
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 24, 53, 80, 11);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 27, 62, "APPROVED");
    canvas_set_color(canvas, ColorBlack);
}

static void draw_denied(Canvas* canvas, AppContext* ctx) {
    // Shake: oscillate x based on frame, decreasing amplitude
    uint8_t f = ctx->anim_frame % 30;
    int shake = 0;
    if(f < 25) {
        int amp = (25 - (int)f) / 5;  // decreasing 0-5
        shake = ((f % 2) == 0) ? amp : -amp;
    }
    int base_x = 57 + shake;
    int base_y = 24;
    draw_character(canvas, base_x, base_y, false, false);

    // "DENIED" banner
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 29, 53, 70, 11);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 33, 62, "DENIED");
    canvas_set_color(canvas, ColorBlack);
}

// ---------------------------------------------------------------------------
// ViewPort draw callback
// ---------------------------------------------------------------------------

static void draw_callback(Canvas* canvas, void* ctx_ptr) {
    AppContext* ctx = ctx_ptr;
    furi_mutex_acquire(ctx->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    switch(ctx->state) {
    case StateIdle:      draw_idle(canvas, ctx);     break;
    case StateThinking:  draw_thinking(canvas, ctx); break;
    case StateAlert:     draw_alert(canvas, ctx);    break;
    case StateWaiting:   draw_waiting(canvas, ctx);  break;
    case StateApproved:  draw_approved(canvas, ctx); break;
    case StateDenied:    draw_denied(canvas, ctx);   break;
    }

    furi_mutex_release(ctx->mutex);
}

// ---------------------------------------------------------------------------
// ViewPort input callback
// ---------------------------------------------------------------------------

static void input_callback(InputEvent* event, void* ctx_ptr) {
    AppContext* ctx = ctx_ptr;
    furi_message_queue_put(ctx->event_queue, event, 0);
}

// ---------------------------------------------------------------------------
// Serial IRQ callback — fills ring buffer
// ---------------------------------------------------------------------------

static void uart_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx_ptr) {
    UNUSED(handle);
    AppContext* ctx = ctx_ptr;
    if(event == FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(handle);
        size_t next = (ctx->rx_head + 1) % RX_BUF_SIZE;
        if(next != ctx->rx_tail) {  // not full
            ctx->rx_buf[ctx->rx_head] = byte;
            ctx->rx_head = next;
        }
    }
}

// ---------------------------------------------------------------------------
// Serial output helper
// ---------------------------------------------------------------------------

static void serial_send(FuriHalSerialHandle* handle, const char* str) {
    furi_hal_serial_tx(handle, (const uint8_t*)str, strlen(str));
}

// ---------------------------------------------------------------------------
// Command handler — called with a complete newline-terminated line
// ---------------------------------------------------------------------------

static void handle_command(AppContext* ctx, FuriHalSerialHandle* handle, const char* line) {
    FURI_LOG_I(TAG, "CMD: %s", line);

    furi_mutex_acquire(ctx->mutex, FuriWaitForever);

    if(strcmp(line, "IDLE") == 0) {
        ctx->state = StateIdle;
        ctx->anim_frame = 0;
    } else if(strcmp(line, "THINK") == 0) {
        ctx->state = StateThinking;
        ctx->anim_frame = 0;
    } else if(strcmp(line, "WAIT") == 0) {
        ctx->state = StateWaiting;
        ctx->anim_frame = 0;
        ctx->progress_pct = 0;
        ctx->progress_msg[0] = '\0';
    } else if(strncmp(line, "NOTIFY:", 7) == 0) {
        const char* rest = line + 7;
        // Parse type: ALERT, OK, DENY
        if(strncmp(rest, "ALERT:", 6) == 0) {
            ctx->state = StateAlert;
            ctx->anim_frame = 0;
            strncpy(ctx->cmd_text, rest + 6, CMD_TEXT_SIZE - 1);
            ctx->cmd_text[CMD_TEXT_SIZE - 1] = '\0';
            ctx->scroll_x = 128;
            ctx->text_width = (int16_t)(strlen(ctx->cmd_text) * 6);
        } else if(strncmp(rest, "OK:", 3) == 0) {
            ctx->state = StateApproved;
            ctx->anim_frame = 0;
            ctx->auto_return = AUTO_RETURN_FRAMES;
            strncpy(ctx->cmd_text, rest + 3, CMD_TEXT_SIZE - 1);
        } else if(strncmp(rest, "DENY:", 5) == 0) {
            ctx->state = StateDenied;
            ctx->anim_frame = 0;
            ctx->auto_return = AUTO_RETURN_FRAMES;
            strncpy(ctx->cmd_text, rest + 5, CMD_TEXT_SIZE - 1);
        }
    } else if(strncmp(line, "PROGRESS:", 9) == 0) {
        // PROGRESS:<0-100>:<message>
        const char* rest = line + 9;
        int pct = atoi(rest);
        if(pct < 0) pct = 0;
        if(pct > 100) pct = 100;
        ctx->progress_pct = (uint8_t)pct;
        const char* colon = strchr(rest, ':');
        if(colon) {
            strncpy(ctx->progress_msg, colon + 1, sizeof(ctx->progress_msg) - 1);
            ctx->progress_msg[sizeof(ctx->progress_msg) - 1] = '\0';
        }
        ctx->state = StateWaiting;
    }

    furi_mutex_release(ctx->mutex);

    // Responses for alert state transitions are sent from input handler
    // Here we just handle state-change commands
    UNUSED(handle);
}

// ---------------------------------------------------------------------------
// Serial worker thread
// ---------------------------------------------------------------------------

static int32_t serial_worker(void* ctx_ptr) {
    AppContext* ctx = ctx_ptr;

    FuriHalSerialHandle* handle = furi_hal_serial_control_acquire(UART_CH);
    furi_check(handle);
    furi_hal_serial_init(handle, 115200);
    furi_hal_serial_async_rx_start(handle, uart_rx_cb, ctx, false);

    FURI_LOG_I(TAG, "Serial worker started");

    while(ctx->running) {
        // Drain ring buffer byte by byte into cmd_buf
        while(ctx->rx_head != ctx->rx_tail) {
            uint8_t byte = ctx->rx_buf[ctx->rx_tail];
            ctx->rx_tail = (ctx->rx_tail + 1) % RX_BUF_SIZE;

            if(byte == '\n' || byte == '\r') {
                if(ctx->cmd_len > 0) {
                    ctx->cmd_buf[ctx->cmd_len] = '\0';
                    handle_command(ctx, handle, ctx->cmd_buf);
                    ctx->cmd_len = 0;
                }
            } else {
                if(ctx->cmd_len < CMD_BUF_SIZE - 1) {
                    ctx->cmd_buf[ctx->cmd_len++] = (char)byte;
                }
            }
        }
        furi_delay_ms(10);
    }

    // Send serial response helper — expose handle to main loop via closure approach
    // Store handle ref in context for use by input handler
    ctx->serial_thread = NULL;  // signal done

    furi_hal_serial_async_rx_stop(handle);
    furi_hal_serial_deinit(handle);
    furi_hal_serial_control_release(handle);

    FURI_LOG_I(TAG, "Serial worker stopped");
    return 0;
}

// We need the handle available in the input/main loop for sending responses.
// Use a simple global pointer since ufbt apps are single-instance.
static FuriHalSerialHandle* g_serial_handle = NULL;

static int32_t serial_worker_v2(void* ctx_ptr) {
    AppContext* ctx = ctx_ptr;

    g_serial_handle = furi_hal_serial_control_acquire(UART_CH);
    furi_check(g_serial_handle);
    furi_hal_serial_init(g_serial_handle, 115200);
    furi_hal_serial_async_rx_start(g_serial_handle, uart_rx_cb, ctx, false);

    FURI_LOG_I(TAG, "Serial worker started (v2)");

    while(ctx->running) {
        while(ctx->rx_head != ctx->rx_tail) {
            uint8_t byte = ctx->rx_buf[ctx->rx_tail];
            ctx->rx_tail = (ctx->rx_tail + 1) % RX_BUF_SIZE;

            if(byte == '\n' || byte == '\r') {
                if(ctx->cmd_len > 0) {
                    ctx->cmd_buf[ctx->cmd_len] = '\0';
                    handle_command(ctx, g_serial_handle, ctx->cmd_buf);
                    ctx->cmd_len = 0;
                }
            } else {
                if(ctx->cmd_len < CMD_BUF_SIZE - 1) {
                    ctx->cmd_buf[ctx->cmd_len++] = (char)byte;
                }
            }
        }
        furi_delay_ms(10);
    }

    furi_hal_serial_async_rx_stop(g_serial_handle);
    furi_hal_serial_deinit(g_serial_handle);
    furi_hal_serial_control_release(g_serial_handle);
    g_serial_handle = NULL;

    FURI_LOG_I(TAG, "Serial worker stopped (v2)");
    return 0;
}

// ---------------------------------------------------------------------------
// Main application entry point
// ---------------------------------------------------------------------------

int32_t flipper_claude_app(void* p) {
    UNUSED(p);

    // Allocate context
    AppContext* ctx = malloc(sizeof(AppContext));
    furi_check(ctx);
    memset(ctx, 0, sizeof(AppContext));
    ctx->running = true;
    ctx->walk_dir = 1;
    ctx->walk_x = 0;
    ctx->state = StateIdle;

    // Mutex for shared state
    ctx->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(ctx->mutex);

    // Event queue
    ctx->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    furi_check(ctx->event_queue);

    // GUI
    ctx->view_port = view_port_alloc();
    view_port_draw_callback_set(ctx->view_port, draw_callback, ctx);
    view_port_input_callback_set(ctx->view_port, input_callback, ctx);

    ctx->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(ctx->gui, ctx->view_port, GuiLayerFullscreen);

    // Serial thread
    ctx->serial_thread = furi_thread_alloc_ex("ClaudeSerial", 1024, serial_worker_v2, ctx);
    furi_thread_start(ctx->serial_thread);

    // Notifications
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);

    // Animation timer using main loop with furi_delay_ms
    uint32_t last_tick = furi_get_tick();

    FURI_LOG_I(TAG, "App started, entering main loop");

    InputEvent event;
    while(ctx->running) {
        // Process input events
        FuriStatus status = furi_message_queue_get(ctx->event_queue, &event, 50);

        if(status == FuriStatusOk) {
            if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                furi_mutex_acquire(ctx->mutex, FuriWaitForever);
                AppState cur_state = ctx->state;
                furi_mutex_release(ctx->mutex);

                if(cur_state == StateAlert) {
                    if(event.key == InputKeyRight) {
                        // APPROVE
                        if(g_serial_handle) serial_send(g_serial_handle, "OK\n");
                        notification_message(notifications, &sequence_success);
                        furi_mutex_acquire(ctx->mutex, FuriWaitForever);
                        ctx->state = StateApproved;
                        ctx->anim_frame = 0;
                        ctx->auto_return = AUTO_RETURN_FRAMES;
                        furi_mutex_release(ctx->mutex);
                    } else if(event.key == InputKeyLeft) {
                        // DENY
                        if(g_serial_handle) serial_send(g_serial_handle, "DENY\n");
                        notification_message(notifications, &sequence_error);
                        furi_mutex_acquire(ctx->mutex, FuriWaitForever);
                        ctx->state = StateDenied;
                        ctx->anim_frame = 0;
                        ctx->auto_return = AUTO_RETURN_FRAMES;
                        furi_mutex_release(ctx->mutex);
                    } else if(event.key == InputKeyBack) {
                        // CANCEL
                        if(g_serial_handle) serial_send(g_serial_handle, "CANCEL\n");
                        furi_mutex_acquire(ctx->mutex, FuriWaitForever);
                        ctx->state = StateIdle;
                        ctx->anim_frame = 0;
                        furi_mutex_release(ctx->mutex);
                    }
                } else if(event.key == InputKeyBack) {
                    // Exit app from any non-alert state
                    ctx->running = false;
                }
            }
        }

        // Animation tick
        uint32_t now = furi_get_tick();
        if(now - last_tick >= ANIM_FPS_PERIOD) {
            last_tick = now;

            furi_mutex_acquire(ctx->mutex, FuriWaitForever);

            ctx->anim_frame++;

            // Idle walk logic
            if(ctx->state == StateIdle) {
                if(ctx->anim_frame % 8 == 0) {
                    ctx->walk_x += ctx->walk_dir;
                    if(ctx->walk_x >= 10 || ctx->walk_x <= -10) {
                        ctx->walk_dir = -ctx->walk_dir;
                    }
                }
            }

            // Alert: advance scroll marquee
            if(ctx->state == StateAlert) {
                ctx->scroll_x--;
                if(ctx->scroll_x < -(ctx->text_width + 10)) {
                    ctx->scroll_x = 128;
                }
            }

            // Auto-return from approved/denied to idle
            if(ctx->state == StateApproved || ctx->state == StateDenied) {
                if(ctx->auto_return > 0) {
                    ctx->auto_return--;
                } else {
                    ctx->state = StateIdle;
                    ctx->anim_frame = 0;
                    ctx->progress_pct = 0;
                }
            }

            furi_mutex_release(ctx->mutex);

            view_port_update(ctx->view_port);
        }
    }

    FURI_LOG_I(TAG, "App exiting, cleaning up");

    // Cleanup
    furi_record_close(RECORD_NOTIFICATION);

    // Stop serial thread
    ctx->running = false;  // already false but be explicit
    furi_thread_join(ctx->serial_thread);
    furi_thread_free(ctx->serial_thread);

    // GUI cleanup
    gui_remove_view_port(ctx->gui, ctx->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(ctx->view_port);

    furi_message_queue_free(ctx->event_queue);
    furi_mutex_free(ctx->mutex);
    free(ctx);

    return 0;
}
