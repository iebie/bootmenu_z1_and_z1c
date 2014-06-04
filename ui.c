/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include <cutils/android_reboot.h>
#include <cutils/properties.h>
#include "minui/minui.h"
#include "recovery_ui.h"
#include "adefines.h"
#include "voldclient/voldclient.h"

extern int __system(const char *command);

#if defined(BOARD_HAS_NO_SELECT_BUTTON) || defined(BOARD_TOUCH_RECOVERY)
static int gShowBackButton = 1;
#else
static int gShowBackButton = 0;
#endif

#define MAX_COLS 96
#ifdef XPERIA_CWM_TOUCH
    #ifdef XPERIA_GO
        #define MAX_ROWS 20
    #else
        #define MAX_ROWS 28
#endif
#else
#define MAX_ROWS 32
#endif

#define MENU_MAX_COLS 64
#define MENU_MAX_ROWS 250

#define MIN_LOG_ROWS 3

#define CHAR_WIDTH BOARD_RECOVERY_CHAR_WIDTH
#define CHAR_HEIGHT BOARD_RECOVERY_CHAR_HEIGHT

#define UI_WAIT_KEY_TIMEOUT_SEC    3600
#define UI_KEY_REPEAT_INTERVAL 80
#define UI_KEY_WAIT_REPEAT 400

UIParameters ui_parameters = {
    6,       // indeterminate progress bar frames
    20,      // fps
    7,       // installation icon frames (0 == static image)
    13, 190, // installation icon overlay offset
};

static pthread_mutex_t gUpdateMutex = PTHREAD_MUTEX_INITIALIZER;
static gr_surface gBackgroundIcon[NUM_BACKGROUND_ICONS];
static gr_surface *gInstallationOverlay;
static gr_surface *gProgressBarIndeterminate;
static gr_surface gProgressBarEmpty;
static gr_surface gProgressBarFill;
static gr_surface gBackground;
static int ui_has_initialized = 0;
static int ui_log_stdout = 1;

static int boardEnableKeyRepeat = 0;
static int boardRepeatableKeys[64], boardNumRepeatableKeys = 0;

enum {
    A0,
    A1,
    A2,
    A3,
    A4,
    A5,
    A6,
    A7,
    A8,
    A9,
    A10,
    AP,
    AT,
    ASPEC,
    NUM_A_ICONS
};
static gr_surface gAicon[NUM_A_ICONS];

static const struct { gr_surface* surface; const char *name; } BITMAPS[] = {
    { &gBackgroundIcon[BACKGROUND_ICON_INSTALLING], "icon_installing" },
    { &gBackgroundIcon[BACKGROUND_ICON_ERROR],      "icon_error" },
    { &gBackgroundIcon[BACKGROUND_ICON_CLOCKWORK],  "icon_clockwork" },
    { &gBackgroundIcon[BACKGROUND_ICON_CID],  "icon_cid" },
    { &gBackgroundIcon[BACKGROUND_ICON_FIRMWARE_INSTALLING], "icon_firmware_install" },
    { &gBackgroundIcon[BACKGROUND_ICON_FIRMWARE_ERROR], "icon_firmware_error" },
    { &gProgressBarEmpty,               "progress_empty" },
    { &gProgressBarFill,                "progress_fill" },
#if defined(XPERIA_Z1C)
    { &gBackground,                "stitch_z1c" },
#else
    { &gBackground,                "stitch" },
#endif
#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
    { &gBackgroundIcon[B1], "b1" },
    { &gBackgroundIcon[B2], "b2" },
    { &gBackgroundIcon[B3], "b3" },
    { &gBackgroundIcon[B4], "b4" },
    { &gBackgroundIcon[B5], "b5" },
    { &gBackgroundIcon[B6], "b6" },
    { &gBackgroundIcon[B7], "b7" },
    { &gBackgroundIcon[B8], "b8" },
    { &gBackgroundIcon[B9], "b9" },
    { &gBackgroundIcon[B10], "b10" },
    { &gBackgroundIcon[BM], "bm" },
#endif
    { NULL,                             NULL },
};

static int gCurrentIcon = 0;
static int gInstallingFrame = 0;

static enum ProgressBarType {
    PROGRESSBAR_TYPE_NONE,
    PROGRESSBAR_TYPE_INDETERMINATE,
    PROGRESSBAR_TYPE_NORMAL,
} gProgressBarType = PROGRESSBAR_TYPE_NONE;

// Progress bar scope of current operation
static float gProgressScopeStart = 0, gProgressScopeSize = 0, gProgress = 0;
static double gProgressScopeTime, gProgressScopeDuration;

// Set to 1 when both graphics pages are the same (except for the progress bar)
static int gPagesIdentical = 0;

static gr_surface gAutoRebootMessage = 0;

// Log text overlay, displayed when a magic key is pressed
static char text[MAX_ROWS][MAX_COLS];
static int text_cols = 0, text_rows = 0;
static int text_col = 0, text_row = 0, text_top = 0;
static int show_text = 0;
static int show_text_ever = 0;   // has show_text ever been 1?

static char menu[MENU_MAX_ROWS][MENU_MAX_COLS];
static int show_menu = 0;
static int menu_top = 0, menu_items = 0, menu_sel = 0;
static int menu_show_start = 0;             // this is line which menu display is starting at
static int max_menu_rows;

static unsigned cur_rainbow_color = 0;
static int gRainbowMode = 0;

// Key event input queue
static pthread_mutex_t key_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t key_queue_cond = PTHREAD_COND_INITIALIZER;
static int key_queue[256], key_queue_len = 0;
static unsigned long key_last_repeat[KEY_MAX + 1], key_press_time[KEY_MAX + 1];
static volatile char key_pressed[KEY_MAX + 1];

static void update_screen_locked(void);
#if SPECIAL_MENU
static void draw_spec_menu(void);
#endif

#ifdef BOARD_TOUCH_RECOVERY
#include "../../vendor/koush/recovery/touch.c"
#else
#ifdef BOARD_RECOVERY_SWIPE
#include "swipe.c"
#endif
#endif

// Return the current time as a double (including fractions of a second).
static double now() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

// Draw the given frame over the installation overlay animation.  The
// background is not cleared or draw with the base icon first; we
// assume that the frame already contains some other frame of the
// animation.  Does nothing if no overlay animation is defined.
// Should only be called with gUpdateMutex locked.
static void draw_install_overlay_locked(int frame) {
    if (gInstallationOverlay == NULL) return;
    gr_surface surface = gInstallationOverlay[frame];
    int iconWidth = gr_get_width(surface);
    int iconHeight = gr_get_height(surface);
    gr_blit(surface, 0, 0, iconWidth, iconHeight,
            ui_parameters.install_overlay_offset_x,
            ui_parameters.install_overlay_offset_y);
}

// Clear the screen and draw the currently selected background icon (if any).
// Should only be called with gUpdateMutex locked.
static void draw_background_locked(int icon)
{
    gPagesIdentical = 0;
    // gr_color(0, 0, 0, 255);
    // gr_fill(0, 0, gr_fb_width(), gr_fb_height());

    {
        int bw = gr_get_width(gBackground);
        int bh = gr_get_height(gBackground);
        int bx = 0;
        int by = 0;
        for (by = 0; by < gr_fb_height(); by += bh) {
            for (bx = 0; bx < gr_fb_width(); bx += bw) {
                gr_blit(gBackground, 0, 0, bw, bh, bx, by);
            }
        }
    }

    if (icon) {
        gr_surface surface = gBackgroundIcon[icon];
        int iconWidth = gr_get_width(surface);
        int iconHeight = gr_get_height(surface);
        int iconX = (gr_fb_width() - iconWidth) / 2;
        int iconY = (gr_fb_height() - iconHeight) / 2;
        gr_blit(surface, 0, 0, iconWidth, iconHeight, iconX, iconY);
        if (icon == BACKGROUND_ICON_INSTALLING) {
            draw_install_overlay_locked(gInstallingFrame);
        }
    }
}

// Draw the progress bar (if any) on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
static void draw_progress_locked()
{
    if (gCurrentIcon == BACKGROUND_ICON_INSTALLING) {
        draw_install_overlay_locked(gInstallingFrame);
    }

    if (gProgressBarType != PROGRESSBAR_TYPE_NONE) {
        int iconHeight = gr_get_height(gBackgroundIcon[BACKGROUND_ICON_INSTALLING]);
        int width = gr_get_width(gProgressBarEmpty);
        int height = gr_get_height(gProgressBarEmpty);

        int dx = (gr_fb_width() - width)/2;
#if !defined(XPERIA_CWM_TOUCH) || !SPECIAL_MENU
        int dy = (3*gr_fb_height() + iconHeight - 2*height)/4;
#else
        int dy = ((3 * gr_fb_height()) + height) / 4;
#endif

        // Erase behind the progress bar (in case this was a progress-only update)
        gr_color(0, 0, 0, 255);
        gr_fill(dx, dy, width, height);

        if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL) {
            float progress = gProgressScopeStart + gProgress * gProgressScopeSize;
            int pos = (int) (progress * width);

            if (pos > 0) {
                gr_blit(gProgressBarFill, 0, 0, pos, height, dx, dy);
            }
            if (pos < width-1) {
                gr_blit(gProgressBarEmpty, pos, 0, width-pos, height, dx+pos, dy);
            }
        }

        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE) {
            static int frame = 0;
            gr_blit(gProgressBarIndeterminate[frame], 0, 0, width, height, dx, dy);
            frame = (frame + 1) % ui_parameters.indeterminate_frames;
        }
    }
}

static void draw_text_line(int row, const char* t) {
  if (t[0] != '\0') {
    if (ui_get_rainbow_mode()) ui_rainbow_mode();
    gr_text(0, (row+1)*CHAR_HEIGHT-1, t);
  }
}

#define BLACK_COLOR 0, 0, 0, 255
#define WHITE_COLOR 255, 255, 255, 255
#define RED_COLOR 255, 0, 0, 255
#define GREEN_COLOR 0, 153, 0, 255
#define ORANGE_COLOR 255, 153, 0, 255

#define MENU_TEXT_COLOR 0, 50, 255, 255
#define NORMAL_TEXT_COLOR 150, 150, 150, 255

#define HEADER_TEXT_COLOR WHITE_COLOR

// Redraw everything on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
static void draw_screen_locked(void)
{
    if (!ui_has_initialized) return;
    draw_background_locked(gCurrentIcon);
    draw_progress_locked();

    if (show_text) {
        int total_rows = gr_fb_height() / CHAR_HEIGHT;
        int i = 0;
        int j = 0;
        int row = 0;            // current row that we are drawing on
        if (show_menu) {
#ifndef BOARD_TOUCH_RECOVERY
            gr_color(BLACK_COLOR);
            gr_fill(0, (menu_top + menu_sel - menu_show_start) * CHAR_HEIGHT,
                    gr_fb_width(), (menu_top + menu_sel - menu_show_start + 1)*CHAR_HEIGHT+1);

            gr_color(HEADER_TEXT_COLOR);
            for (i = 0; i < menu_top; ++i) {
                draw_text_line(i, menu[i]);
                row++;
            }

            if (menu_items - menu_show_start + menu_top >= max_menu_rows)
                j = max_menu_rows - menu_top;
            else
                j = menu_items - menu_show_start;

            gr_color(BLACK_COLOR);
            for (i = menu_show_start + menu_top; i < (menu_show_start + menu_top + j); ++i) {
                if (i == menu_top + menu_sel) {
                    gr_color(RED_COLOR);
                    draw_text_line(i - menu_show_start , menu[i]);
                    gr_color(BLACK_COLOR);
                } else {
                    if (ui_root_menu && (i == menu_show_start + menu_top || i == menu_show_start + menu_top + 1))
                        gr_color(GREEN_COLOR);
                    else if (i == menu_show_start + menu_top + 2 && ui_root_menu)
                        gr_color(ORANGE_COLOR);
                    else
                        gr_color(MENU_TEXT_COLOR);
                    draw_text_line(i - menu_show_start, menu[i]);
                    gr_color(BLACK_COLOR);
                }
                row++;
                if (row >= max_menu_rows)
                    break;
            }

            gr_fill(0, row*CHAR_HEIGHT+CHAR_HEIGHT/2-1,
                    gr_fb_width(), row*CHAR_HEIGHT+CHAR_HEIGHT/2+1);
#else
            row = draw_touch_menu(menu, menu_items, menu_top, menu_sel, menu_show_start);
#endif
        }

        gr_color(NORMAL_TEXT_COLOR);
#ifdef XPERIA_CWM_TOUCH
        for (; row < text_rows; ++row) {
            draw_text_line(row+1, text[(row+text_top) % text_rows]);
#else
        int cur_row = text_row;
        int available_rows = total_rows - row - 1;
        int start_row = row + 1;
        if (available_rows < MAX_ROWS)
            cur_row = (cur_row + (MAX_ROWS - available_rows)) % MAX_ROWS;
        else
            start_row = total_rows - MAX_ROWS;

        int r;
        for (r = 0; r < (available_rows < MAX_ROWS ? available_rows : MAX_ROWS); r++) {
            draw_text_line(start_row + r, text[(cur_row + r) % MAX_ROWS]);
#endif
        }
    }
}

// Redraw everything on the screen and flip the screen (make it visible).
// Should only be called with gUpdateMutex locked.
static void update_screen_locked(void)
{
    if (!ui_has_initialized) return;
    draw_screen_locked();
#if SPECIAL_MENU
    if (!gSpecialButtonPressed)
        draw_spec_menu();
#endif
    gr_flip();
}

// Updates only the progress bar, if possible, otherwise redraws the screen.
// Should only be called with gUpdateMutex locked.
static void update_progress_locked(void)
{
    if (!ui_has_initialized) return;
    if (show_text || !gPagesIdentical) {
        draw_screen_locked();    // Must redraw the whole screen
        gPagesIdentical = 1;
    } else {
        draw_progress_locked();  // Draw only the progress bar and overlays
    }
    gr_flip();
}

// Keeps the progress bar updated, even when the process is otherwise busy.
static void *progress_thread(void *cookie)
{
    double interval = 1.0 / ui_parameters.update_fps;
    for (;;) {
        double start = now();
        pthread_mutex_lock(&gUpdateMutex);

        int redraw = 0;

        // update the installation animation, if active
        // skip this if we have a text overlay (too expensive to update)
        if (gCurrentIcon == BACKGROUND_ICON_INSTALLING &&
#ifndef CWM_INST_ANIM
            ui_parameters.installing_frames > 0 &&
            !show_text) {
#else
            ui_parameters.installing_frames > 0) {
#endif
            gInstallingFrame =
                (gInstallingFrame + 1) % ui_parameters.installing_frames;
            redraw = 1;
        }

        // update the progress bar animation, if active
        // skip this if we have a text overlay (too expensive to update)
#ifndef CWM_INST_ANIM
        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE && !show_text) {
#else
        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE) {
#endif
            redraw = 1;
        }

        // move the progress bar forward on timed intervals, if configured
        int duration = gProgressScopeDuration;
        if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && duration > 0) {
            double elapsed = now() - gProgressScopeTime;
            float progress = 1.0 * elapsed / duration;
            if (progress > 1.0) progress = 1.0;
            if (progress > gProgress) {
                gProgress = progress;
                redraw = 1;
            }
        }

        if (redraw) update_progress_locked();

        pthread_mutex_unlock(&gUpdateMutex);
        double end = now();
        // minimum of 20ms delay between frames
        double delay = interval - (end-start);
        if (delay < 0.02) delay = 0.02;
        usleep((long)(delay * 1000000));
    }
    return NULL;
}

static int rel_sum = 0;
#ifdef XPERIA_CWM_TOUCH
int touch_x = 0;
int touch_y = 0;
int rcnt = 0;
int touch_released = 1;
#endif

static int input_callback(int fd, short revents, void *data)
{
    struct input_event ev;
    int ret;
    int fake_key = 0;
#if defined(XPERIA_CWM_TOUCH) || defined(XPERIA_Z1C) || defined(XPERIA_Z1)
#if SPECIAL_MENU
    int touch_val_x;
    int touch_val_y;
    int button_sz;
#endif
#endif

    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;

#ifdef BOARD_TOUCH_RECOVERY
    if (touch_handle_input(fd, ev))
        return 0;
#else
#ifdef BOARD_RECOVERY_SWIPE
    swipe_handle_input(fd, &ev);
#endif
#endif

    if (ev.type == EV_SYN) {
#ifdef XPERIA_CWM_TOUCH
        if (ev.code == SYN_MT_REPORT) {
            rcnt += 1;
            if (rcnt >= 2) {
                rcnt = 0;
                touch_released = 1;
            } else {
                if (!touch_x && !touch_y) {
                    touch_released = 1;
                }
                touch_x = 0;
                touch_y = 0;
            }
        }
#endif
        return 0;
    } else if (ev.type == EV_REL) {
        if (ev.code == REL_Y) {
            // accumulate the up or down motion reported by
            // the trackball.  When it exceeds a threshold
            // (positive or negative), fake an up/down
            // key event.
            rel_sum += ev.value;
            if (rel_sum > 3) {
                fake_key = 1;
                ev.type = EV_KEY;
                ev.code = KEY_DOWN;
                ev.value = 1;
                rel_sum = 0;
            } else if (rel_sum < -3) {
                fake_key = 1;
                ev.type = EV_KEY;
                ev.code = KEY_UP;
                ev.value = 1;
                rel_sum = 0;
            }
        }
    } else {
        rel_sum = 0;
    }

#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
#if SPECIAL_MENU
    if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X)
                touch_val_x = ev.value;
            if (ev.code == ABS_MT_POSITION_Y)
                touch_val_y = ev.value;

            if (touch_val_x >= 284 && touch_val_x <= 450)
                button_sz = 1;

            if (touch_val_y >= 1190 && touch_val_y <= 1270 && button_sz) {
                button_sz = 0;
                ui_print("gSpecialButtonPressed=%d\n", gSpecialButtonPressed);
                gSpecialButtonPressed = 1;
            }
    }
#endif
#endif
#ifdef XPERIA_CWM_TOUCH
    if (ev.type == EV_ABS) {
            rcnt = 0;
            if (ev.code == ABS_MT_POSITION_X)
                touch_x = ev.value;
            if (ev.code == ABS_MT_POSITION_Y)
                touch_y = ev.value;
            if (ev.code == ABS_MT_DISTANCE)
                ev.code = 0;
            if (touch_x && touch_y && touch_released) {
                button_sz = (gr_fb_width() / 7); //4 buttons + 3 spaces with same size like button
                //ui_print("%d %d %d\n", touch_x, touch_y, button_sz);
                if (touch_x >= (gr_fb_width()-(7*button_sz)) && touch_x <= (gr_fb_width()-(6*button_sz)))
                    touch_val_x = 1;
                else if (touch_x >= (gr_fb_width()-(5*button_sz)) && touch_x <= (gr_fb_width()-(4*button_sz)))
                    touch_val_x = 2;
                else if (touch_x >= (gr_fb_width()-(3*button_sz)) && touch_x <= (gr_fb_width()-(2*button_sz)))
                    touch_val_x = 3;
                else if (touch_x >= (gr_fb_width()-button_sz) && touch_x <= gr_fb_width())
                    touch_val_x = 4;
                else
                    touch_val_x = 0;

#ifdef XPERIA_GO
      if (touch_y >= (gr_fb_height()-50) && touch_y <= gr_fb_height())
#else
      if (touch_y >= (gr_fb_height()-75) && touch_y <= gr_fb_height())
#endif
                    touch_val_y = 1;
                else
                    touch_val_y = 0;

                switch (touch_val_x) {
                    case 1:
                        if (touch_val_y) {
                            ev.type = EV_KEY;
                            ev.code = KEY_UP;
                            ev.value = 1;
                        }
                        break;
                    case 2:
                        if (touch_val_y) {
                            ev.type = EV_KEY;
                            ev.code = KEY_DOWN;
                            ev.value = 1;
                        }
                        break;
                    case 3:
                        if (touch_val_y) {
                            ev.type = EV_KEY;
                            ev.code = KEY_ENTER;
                            ev.value = 1;
                        }
                        break;
                    case 4:
                        if (touch_val_y) {
                            ev.type = EV_KEY;
                            ev.code = KEY_BACKSPACE;
                            ev.value = 1;
                        }
                        break;
                    default:
                        break;
                }
                touch_released = 0;
            }
    }
#endif

    if (ev.type != EV_KEY || ev.code > KEY_MAX)
        return 0;

    if (ev.value == 2) {
        boardEnableKeyRepeat = 0;
    }

    pthread_mutex_lock(&key_queue_mutex);
    if (!fake_key) {
        // our "fake" keys only report a key-down event (no
        // key-up), so don't record them in the key_pressed
        // table.
        key_pressed[ev.code] = ev.value;
    }
    const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
    if (ev.value > 0 && key_queue_len < queue_max) {
        key_queue[key_queue_len++] = ev.code;

        if (boardEnableKeyRepeat) {
            struct timeval now;
            gettimeofday(&now, NULL);

            key_press_time[ev.code] = (now.tv_sec * 1000) + (now.tv_usec / 1000);
            key_last_repeat[ev.code] = 0;
        }

        pthread_cond_signal(&key_queue_cond);
    }
    pthread_mutex_unlock(&key_queue_mutex);

    if (ev.value > 0 && device_toggle_display(key_pressed, ev.code)) {
        pthread_mutex_lock(&gUpdateMutex);
        show_text = !show_text;
        if (show_text) show_text_ever = 1;
        update_screen_locked();
        pthread_mutex_unlock(&gUpdateMutex);
    }

    if (ev.value > 0 && device_reboot_now(key_pressed, ev.code)) {
        reboot_main_system(ANDROID_RB_RESTART, 0, 0);
    }

    return 0;
}

// Reads input events, handles special hot keys, and adds to the key queue.
static void *input_thread(void *cookie)
{
    for (;;) {
        if (!ev_wait(-1))
            ev_dispatch();
    }
    return NULL;
}

int get_battery_level(void) {
    char buf[4];
    char *str;
    int fd;
    int level;
    ssize_t nbytes;

    fd = open(BATTERY_LEVEL_FILE, O_RDONLY);
    if (fd < 0)
#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
    {
        fd = open("/battery_capacity", O_RDONLY);
        if (fd < 0) {
            __system("ln -s /sys/devices/qpnp-charger-*/power_supply/battery/capacity /battery_capacity");
            fd = open("/battery_capacity", O_RDONLY);
            if (fd < 0)
                return 0;
        }
#else
        return 0;
#endif
#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
    }
#endif

    nbytes = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (nbytes < 0)
        return 0;
    buf[nbytes] = '\0';

    str = strndup(buf, nbytes);
    level = atoi(str);

    return level;
}

int battery_charging_usb(void) {
    char buf[2];
    char *str;
    int fd;
    int charging;
    ssize_t nbytes;

    fd = open(BATTERY_STATUS_CHARGING_FILE_USB, O_RDONLY);
    if (fd < 0)
        return 0;

    nbytes = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (nbytes < 0)
        return 0;
    buf[nbytes] = '\0';

    str = strndup(buf, 1);
    charging = atoi(str);

    return charging;
}

int battery_charging_ac(void) {
#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
    return battery_charging_usb();
#endif
    char buf[2];
    char *str;
    int fd;
    int charging;
    ssize_t nbytes;

    fd = open(BATTERY_STATUS_CHARGING_FILE_AC, O_RDONLY);
    if (fd < 0)
        return 0;

    nbytes = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (nbytes < 0)
        return 0;
    buf[nbytes] = '\0';

    str = strndup(buf, 1);
    charging = atoi(str);

    return charging;
}

int command_update_leds(char *led_file, int value) {
    char str[20];
    int fd;
    int ret;

    fd = open(led_file, O_WRONLY);
    if (fd < 0)
        return 0;

    ret = snprintf(str, sizeof(str), "%d", value);
    ret = write(fd, str, ret);
    close(fd);

    if (ret < 0)
       return 0;

    return 1;
}

void update_leds(void) {
    int level = get_battery_level();

    int charging_usb = battery_charging_usb();
    int charging_ac = battery_charging_ac();

    int val = 255;
    int val_thin = 122;
    int val_off = 0;

    int status;

    if (level == 0)
        level = 1;

    if (charging_usb || charging_ac) {
        if (level <= 90) {
            status = command_update_leds(RED_LED_FILE, val);
            status = command_update_leds(GREEN_LED_FILE, val_off);
        } else if (level > 90 && level != 100) {
            status = command_update_leds(RED_LED_FILE, val_thin);
            status = command_update_leds(GREEN_LED_FILE, val_thin);
        } else  {
            status = command_update_leds(RED_LED_FILE, val_off);
            status = command_update_leds(GREEN_LED_FILE, val);
        }
    } else {
            status = command_update_leds(RED_LED_FILE, val_off);
            status = command_update_leds(GREEN_LED_FILE, val_off);
    }

    return;
}

static void *leds_thread(void *cookie) {
    for (;;) {
        sleep(2);
        update_leds();
    }
    return NULL;
}

int get_time_from_kernel(int get_hour, int get_minute) {
    char buf[10];
    char *hour_str, *minute_str;
    int hour, minute;
    int fd;
    ssize_t nbytes;

    fd = open(KERNEL_TIME_SYSFS, O_RDONLY);
    if (fd < 0)
#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
    {
        fd = open("/rtc_time", O_RDONLY);
        if (fd < 0) {
            __system("ln -s /sys/devices/qpnp-rtc-*/rtc/rtc0/time /rtc_time");
            fd = open("/rtc_time", O_RDONLY);
            if (fd < 0)
                return 0;
        }
#else
        return 0;
#endif
#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
    }
#endif

    nbytes = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (nbytes < 0) return 0;
    buf[nbytes] = '\0';

    hour_str = strndup(buf, 2);
    minute_str = strndup(buf+3, 2);
    hour = atoi(hour_str);
    minute = atoi(minute_str);

    if (get_hour) return hour;
    if (get_minute) return minute;

    return 0;
}

extern int getwtf(void);

static void draw_kexec_autoreboot_message(gr_surface icon_one) {
	if (icon_one) {
		ui_set_background(BACKGROUND_ICON_NONE);
		int iconWidthOne = gr_get_width(icon_one);
		int iconHeightOne = gr_get_height(icon_one);

		int iconXOne = ((gr_fb_width() - iconWidthOne) / 2);
		int iconYOne = (gr_fb_height() / 2);

		gr_color(BLACK_COLOR);
		gr_fill(iconXOne, iconYOne, iconWidthOne, iconHeightOne);
		gr_blit(icon_one, 0, 0, iconWidthOne, iconHeightOne, iconXOne, iconYOne);
	}
}

static void update_kexec_autoreboot_message(short mes) {
	if (!ui_has_initialized)
		return;

		gAutoRebootMessage = gBackgroundIcon[mes];
		draw_kexec_autoreboot_message(gAutoRebootMessage);
		gr_flip();
}

extern char kernel[10][350];
extern char ramdisk[10][350];
extern char cmdline[10][350];
extern char dtb[10][350];
extern int popen_wait_done(char *what);
extern int is_not_empty;

extern short disable_k_a_t;
int k_a_t = (int)BM + 1;
int k_a_t_executed = 0;
char popen_fr[500];

static void *kexec_autoreboot_thread(void *cookie) {
	for (;;) {
		if (!disable_k_a_t && !is_not_empty) {
			if (k_a_t > (int)BACKGROUND_ICON_FIRMWARE_ERROR)
				update_kexec_autoreboot_message(k_a_t);
			else if (k_a_t == (int)BACKGROUND_ICON_FIRMWARE_ERROR && !k_a_t_executed) {
				int ret;
				k_a_t_executed = 1;
				sprintf(popen_fr, "/sbin/kexec --load-hardboot %s --initrd=%s --mem-min=0x30000000 --command-line=\"$(cat /proc/cmdline)\" --dtb=%s ; exit $?",
						 kernel[0], ramdisk[0], dtb[0]);
				ret = popen_wait_done(popen_fr);
				if (ret == 0) {
					ui_print("kexecing...\n");
					sleep(2);
					ensure_path_unmounted("/storage/sdcard1");
					ensure_path_unmounted("/cache");
					ensure_path_unmounted("/data");
					__system("sync ; /sbin/kexec -e");
				} else
					ui_print("Something went wrong, please see log!\n");
			}
			usleep(1500000);
			k_a_t -= 1;
		}
		if (k_a_t < 0)
			k_a_t = 0;
	}
	return NULL;
}

void ui_init(void)
{
    int wtfint = getwtf();
    int check = 2105;
    if (check != wtfint)
        __system("/sbin/reboot");

    ui_has_initialized = 1;
    gr_init();
    ev_init(input_callback, NULL);
#ifdef BOARD_TOUCH_RECOVERY
    touch_init();
#endif

    text_col = text_row = 0;
    text_rows = gr_fb_height() / CHAR_HEIGHT;
    max_menu_rows = text_rows - MIN_LOG_ROWS;
#ifdef BOARD_TOUCH_RECOVERY
    max_menu_rows = get_max_menu_rows(max_menu_rows);
#endif
    if (max_menu_rows > MENU_MAX_ROWS)
        max_menu_rows = MENU_MAX_ROWS;
    if (text_rows > MAX_ROWS) text_rows = MAX_ROWS;
    text_top = 1;

    text_cols = gr_fb_width() / CHAR_WIDTH;
    if (text_cols > MAX_COLS - 1) text_cols = MAX_COLS - 1;

    int i;
    for (i = 0; BITMAPS[i].name != NULL; ++i) {
        int result = res_create_surface(BITMAPS[i].name, BITMAPS[i].surface);
        if (result < 0) {
            LOGE("Missing bitmap %s\n(Code %d)\n", BITMAPS[i].name, result);
        }
    }

    gProgressBarIndeterminate = malloc(ui_parameters.indeterminate_frames *
                                       sizeof(gr_surface));
    for (i = 0; i < ui_parameters.indeterminate_frames; ++i) {
        char filename[40];
        // "indeterminate01.png", "indeterminate02.png", ...
        sprintf(filename, "indeterminate%02d", i+1);
        int result = res_create_surface(filename, gProgressBarIndeterminate+i);
        if (result < 0) {
            LOGE("Missing bitmap %s\n(Code %d)\n", filename, result);
        }
    }

    if (ui_parameters.installing_frames > 0) {
        gInstallationOverlay = malloc(ui_parameters.installing_frames *
                                      sizeof(gr_surface));
        for (i = 0; i < ui_parameters.installing_frames; ++i) {
            char filename[40];
            // "icon_installing_overlay01.png",
            // "icon_installing_overlay02.png", ...
            sprintf(filename, "icon_installing_overlay%02d", i+1);
            int result = res_create_surface(filename, gInstallationOverlay+i);
            if (result < 0) {
                LOGE("Missing bitmap %s\n(Code %d)\n", filename, result);
            }
        }

        // Adjust the offset to account for the positioning of the
        // base image on the screen.
        if (gBackgroundIcon[BACKGROUND_ICON_INSTALLING] != NULL) {
            gr_surface bg = gBackgroundIcon[BACKGROUND_ICON_INSTALLING];
            ui_parameters.install_overlay_offset_x +=
                (gr_fb_width() - gr_get_width(bg)) / 2;
            ui_parameters.install_overlay_offset_y +=
                (gr_fb_height() - gr_get_height(bg)) / 2;
        }
    } else {
        gInstallationOverlay = NULL;
    }

    char enable_key_repeat[PROPERTY_VALUE_MAX];
    property_get("ro.cwm.enable_key_repeat", enable_key_repeat, "");
    if (!strcmp(enable_key_repeat, "true") || !strcmp(enable_key_repeat, "1")) {
        boardEnableKeyRepeat = 1;

        char key_list[PROPERTY_VALUE_MAX];
        property_get("ro.cwm.repeatable_keys", key_list, "");
        if (strlen(key_list) == 0) {
            boardRepeatableKeys[boardNumRepeatableKeys++] = KEY_UP;
            boardRepeatableKeys[boardNumRepeatableKeys++] = KEY_DOWN;
            boardRepeatableKeys[boardNumRepeatableKeys++] = KEY_VOLUMEUP;
            boardRepeatableKeys[boardNumRepeatableKeys++] = KEY_VOLUMEDOWN;
        } else {
            char *pch = strtok(key_list, ",");
            while (pch != NULL) {
                boardRepeatableKeys[boardNumRepeatableKeys++] = atoi(pch);
                pch = strtok(NULL, ",");
            }
        }
    }

    pthread_t t;
    pthread_create(&t, NULL, progress_thread, NULL);
    pthread_create(&t, NULL, input_thread, NULL);
    pthread_create(&t, NULL, leds_thread, NULL);
#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
    pthread_create(&t, NULL, kexec_autoreboot_thread, NULL);
#endif
#ifndef XPERIA_CWM_TOUCH
#if !defined(XPERIA_Z1C) && !defined(XPERIA_Z1)
    char buttonlights_command[100];
#endif
#endif
#ifdef XPERIA_GO
#ifndef XPERIA_CWM_TOUCH
    snprintf(buttonlights_command, sizeof(buttonlights_command), "/sbin/echo 255 > %s", BUTTON_BACKLIGHT);
    __system(buttonlights_command);
#endif
    __system("/sbin/setprop ro.build.product lotus");
#endif
#ifdef XPERIA_SOLA
#ifndef XPERIA_CWM_TOUCH
    snprintf(buttonlights_command, sizeof(buttonlights_command), "/sbin/echo 255 > %s", BUTTON_BACKLIGHT);
    __system(buttonlights_command);
#endif
    __system("/sbin/setprop ro.build.product pepper");
#endif
#ifdef XPERIA_P
#ifndef XPERIA_CWM_TOUCH
    snprintf(buttonlights_command, sizeof(buttonlights_command), "/sbin/echo 255 > %s", BUTTON_BACKLIGHT);
    __system(buttonlights_command);
#endif
    __system("/sbin/setprop ro.build.product nypon");
#endif
#ifdef XPERIA_U
    __system("/sbin/setprop ro.build.product kumquat");
#endif
#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
#if defined(XPERIA_Z1C)
    __system("/sbin/setprop ro.build.product amami");
#elif defined(XPERIA_Z1)
    __system("/sbin/setprop ro.build.product honami");
#endif
    __system("/sbin/ln -s /sys/devices/leds-qpnp-*/leds/wled:backlight/brightness /scr_bright");
    __system("/sbin/echo 2800 > /scr_bright");
    __system("/sbin/busybox blockdev --setrw /dev/block/platform/msm_sdcc.1/by-name/system");
#endif
}

char *ui_copy_image(int icon, int *width, int *height, int *bpp) {
    pthread_mutex_lock(&gUpdateMutex);
    draw_background_locked(icon);
    *width = gr_fb_width();
    *height = gr_fb_height();
    *bpp = sizeof(gr_pixel) * 8;
    int size = *width * *height * sizeof(gr_pixel);
    char *ret = malloc(size);
    if (ret == NULL) {
        LOGE("Can't allocate %d bytes for image\n", size);
    } else {
        memcpy(ret, gr_fb_data(), size);
    }
    pthread_mutex_unlock(&gUpdateMutex);
    return ret;
}

void ui_set_background(int icon)
{
    pthread_mutex_lock(&gUpdateMutex);
    gCurrentIcon = icon;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_show_indeterminate_progress()
{
    pthread_mutex_lock(&gUpdateMutex);
    if (gProgressBarType != PROGRESSBAR_TYPE_INDETERMINATE) {
        gProgressBarType = PROGRESSBAR_TYPE_INDETERMINATE;
        update_progress_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_show_progress(float portion, int seconds)
{
    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NORMAL;
    gProgressScopeStart += gProgressScopeSize;
    gProgressScopeSize = portion;
    gProgressScopeTime = now();
    gProgressScopeDuration = seconds;
    gProgress = 0;
    update_progress_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_set_progress(float fraction)
{
    pthread_mutex_lock(&gUpdateMutex);
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && fraction > gProgress) {
        // Skip updates that aren't visibly different.
        int width = gr_get_width(gProgressBarIndeterminate[0]);
        float scale = width * gProgressScopeSize;
        if ((int) (gProgress * scale) != (int) (fraction * scale)) {
            gProgress = fraction;
            update_progress_locked();
        }
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_reset_progress()
{
    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NONE;
    gProgressScopeStart = gProgressScopeSize = 0;
    gProgressScopeTime = gProgressScopeDuration = 0;
    gProgress = 0;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

static long delta_milliseconds(struct timeval from, struct timeval to) {
    long delta_sec = (to.tv_sec - from.tv_sec)*1000;
    long delta_usec = (to.tv_usec - from.tv_usec)/1000;
    return (delta_sec + delta_usec);
}

static struct timeval lastupdate = (struct timeval) {0};
static int ui_nice = 0;
static int ui_niced = 0;
void ui_set_nice(int enabled) {
    ui_nice = enabled;
}
#define NICE_INTERVAL 100
int ui_was_niced() {
    return ui_niced;
}
int ui_get_text_cols() {
    return text_cols;
}

void ui_print(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, 256, fmt, ap);
    va_end(ap);

    if (ui_log_stdout)
        fputs(buf, stdout);

    // if we are running 'ui nice' mode, we do not want to force a screen update
    // for this line if not necessary.
    ui_niced = 0;
    if (ui_nice) {
        struct timeval curtime;
        gettimeofday(&curtime, NULL);
        long ms = delta_milliseconds(lastupdate, curtime);
        if (ms < NICE_INTERVAL && ms >= 0) {
            ui_niced = 1;
            return;
        }
    }

    // This can get called before ui_init(), so be careful.
    pthread_mutex_lock(&gUpdateMutex);
    gettimeofday(&lastupdate, NULL);
    if (text_rows > 0 && text_cols > 0) {
        char *ptr;
        for (ptr = buf; *ptr != '\0'; ++ptr) {
            if (*ptr == '\n' || text_col >= text_cols) {
                text[text_row][text_col] = '\0';
                text_col = 0;
                text_row = (text_row + 1) % text_rows;
                if (text_row == text_top) text_top = (text_top + 1) % text_rows;
            }
            if (*ptr != '\n') text[text_row][text_col++] = *ptr;
        }
        text[text_row][text_col] = '\0';
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_printlogtail(int nb_lines) {
    char * log_data;
    char tmp[PATH_MAX];
    FILE * f;
    int line=0;
    //don't log output to recovery.log
    ui_log_stdout=0;
    sprintf(tmp, "tail -n %d /tmp/recovery.log > /tmp/tail.log", nb_lines);
    __system(tmp);
    f = fopen("/tmp/tail.log", "rb");
    if (f != NULL) {
        while (line < nb_lines) {
            log_data = fgets(tmp, PATH_MAX, f);
            if (log_data == NULL) break;
            ui_print("%s", tmp);
            line++;
        }
        fclose(f);
    }
    ui_log_stdout=1;
}

#define MENU_ITEM_HEADER " "
#define MENU_ITEM_HEADER_LENGTH strlen(MENU_ITEM_HEADER)

int ui_start_menu(const char** headers, char** items, int initial_selection) {
    int i;
    pthread_mutex_lock(&gUpdateMutex);
    if (text_rows > 0 && text_cols > 0) {
        for (i = 0; i < text_rows; ++i) {
            if (headers[i] == NULL) break;
            strncpy(menu[i], headers[i], text_cols-1);
            menu[i][text_cols-1] = '\0';
        }
        menu_top = i;
        for (; i < MENU_MAX_ROWS; ++i) {
            if (items[i-menu_top] == NULL) break;
            strcpy(menu[i], MENU_ITEM_HEADER);
            strncpy(menu[i] + MENU_ITEM_HEADER_LENGTH, items[i-menu_top], MENU_MAX_COLS - 1 - MENU_ITEM_HEADER_LENGTH);
            menu[i][MENU_MAX_COLS-1] = '\0';
        }
/*
        if (gShowBackButton && !ui_root_menu) {
            strcpy(menu[i], " - +++++Go Back+++++");
            ++i;
        }
*/
        menu_items = i - menu_top;
        show_menu = 1;
        menu_sel = menu_show_start = initial_selection;
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
/*
    if (gShowBackButton && !ui_root_menu) {
        return menu_items - 1;
    }
*/
    return menu_items;
}

int ui_menu_select(int sel) {
    int old_sel;
    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0) {
        old_sel = menu_sel;
        menu_sel = sel;

        if (menu_sel < 0) menu_sel = menu_items + menu_sel;
        if (menu_sel >= menu_items) menu_sel = menu_sel - menu_items;


        if (menu_sel < menu_show_start && menu_show_start > 0) {
            menu_show_start = menu_sel;
        }

        if (menu_sel - menu_show_start + menu_top >= max_menu_rows) {
            menu_show_start = menu_sel + menu_top - max_menu_rows + 1;
        }

        sel = menu_sel;

        if (menu_sel != old_sel) update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
    return sel;
}

void ui_end_menu() {
    int i;
    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0 && text_rows > 0 && text_cols > 0) {
        show_menu = 0;
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

int ui_text_visible()
{
    pthread_mutex_lock(&gUpdateMutex);
    int visible = show_text;
    pthread_mutex_unlock(&gUpdateMutex);
    return visible;
}

int ui_text_ever_visible()
{
    pthread_mutex_lock(&gUpdateMutex);
    int ever_visible = show_text_ever;
    pthread_mutex_unlock(&gUpdateMutex);
    return ever_visible;
}

void ui_show_text(int visible)
{
    pthread_mutex_lock(&gUpdateMutex);
    show_text = visible;
    if (show_text) show_text_ever = 1;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

// Return true if USB is connected.
static int usb_connected() {
    int fd = open("/sys/class/android_usb/android0/state", O_RDONLY);
    if (fd < 0) {
        printf("failed to open /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
        return 0;
    }

    char buf;
    /* USB is connected if android_usb state is CONNECTED or CONFIGURED */
    int connected = (read(fd, &buf, 1) == 1) && (buf == 'C');
    if (close(fd) < 0) {
        printf("failed to close /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
    }
    return connected;
}

void ui_cancel_wait_key() {
    pthread_mutex_lock(&key_queue_mutex);
    key_queue[key_queue_len] = -2;
    key_queue_len++;
    pthread_cond_signal(&key_queue_cond);
    pthread_mutex_unlock(&key_queue_mutex);
}

extern int volumes_changed();

// delay in seconds to refresh clock and USB plugged volumes
#define REFRESH_TIME_USB_INTERVAL 5
int ui_wait_key()
{
    if (boardEnableKeyRepeat) return ui_wait_key_with_repeat();
    pthread_mutex_lock(&key_queue_mutex);
    int timeouts = UI_WAIT_KEY_TIMEOUT_SEC;

    // Time out after REFRESH_TIME_USB_INTERVAL seconds to catch volume changes, and loop for
    // UI_WAIT_KEY_TIMEOUT_SEC to restart a device not connected to USB
    do {
        struct timeval now;
        struct timespec timeout;
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec;
        timeout.tv_nsec = now.tv_usec * 1000;
        timeout.tv_sec += REFRESH_TIME_USB_INTERVAL;

        int rc = 0;
        while (key_queue_len == 0 && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex,
                                        &timeout);
            if (volumes_changed()) {
                pthread_mutex_unlock(&key_queue_mutex);
                return REFRESH;
            }
        }
        timeouts -= REFRESH_TIME_USB_INTERVAL;
    } while ((timeouts > 0 || usb_connected()) && key_queue_len == 0);

    int key = -1;
    if (key_queue_len > 0) {
        key = key_queue[0];
        memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
    }
    pthread_mutex_unlock(&key_queue_mutex);
    return key;
}

// util for ui_wait_key_with_repeat
int key_can_repeat(int key)
{
    int k = 0;
    for (;k < boardNumRepeatableKeys; ++k) {
        if (boardRepeatableKeys[k] == key) {
            break;
        }
    }
    if (k < boardNumRepeatableKeys) return 1;
    return 0;
}

int ui_wait_key_with_repeat()
{
    int key = -1;

    // Loop to wait for more keys.
    do {
        int timeouts = UI_WAIT_KEY_TIMEOUT_SEC;
        int rc = 0;
        struct timeval now;
        struct timespec timeout;
        pthread_mutex_lock(&key_queue_mutex);
        while (key_queue_len == 0 && timeouts > 0) {
            gettimeofday(&now, NULL);
            timeout.tv_sec = now.tv_sec;
            timeout.tv_nsec = now.tv_usec * 1000;
            timeout.tv_sec += REFRESH_TIME_USB_INTERVAL;

            rc = 0;
            while (key_queue_len == 0 && rc != ETIMEDOUT) {
                rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex,
                                            &timeout);
                if (volumes_changed()) {
                    pthread_mutex_unlock(&key_queue_mutex);
                    return REFRESH;
                }
            }
            timeouts -= REFRESH_TIME_USB_INTERVAL;
        }
        pthread_mutex_unlock(&key_queue_mutex);

        if (rc == ETIMEDOUT && !usb_connected()) {
            return -1;
        }

        // Loop to wait wait for more keys, or repeated keys to be ready.
        while (1) {
            unsigned long now_msec;

            gettimeofday(&now, NULL);
            now_msec = (now.tv_sec * 1000) + (now.tv_usec / 1000);

            pthread_mutex_lock(&key_queue_mutex);

            // Replacement for the while conditional, so we don't have to lock the entire
            // loop, because that prevents the input system from touching the variables while
            // the loop is running which causes problems.
            if (key_queue_len == 0) {
                pthread_mutex_unlock(&key_queue_mutex);
                break;
            }

            key = key_queue[0];
            memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);

            // sanity check the returned key.
            if (key < 0) {
                pthread_mutex_unlock(&key_queue_mutex);
                return key;
            }

            // Check for already released keys and drop them if they've repeated.
            if (!key_pressed[key] && key_last_repeat[key] > 0) {
                pthread_mutex_unlock(&key_queue_mutex);
                continue;
            }

            if (key_can_repeat(key)) {
                // Re-add the key if a repeat is expected, since we just popped it. The
                // if below will determine when the key is actually repeated (returned)
                // in the mean time, the key will be passed through the queue over and
                // over and re-evaluated each time.
                if (key_pressed[key]) {
                    key_queue[key_queue_len] = key;
                    key_queue_len++;
                }
                if ((now_msec > key_press_time[key] + UI_KEY_WAIT_REPEAT && now_msec > key_last_repeat[key] + UI_KEY_REPEAT_INTERVAL) ||
                        key_last_repeat[key] == 0) {
                    key_last_repeat[key] = now_msec;
                } else {
                    // Not ready
                    pthread_mutex_unlock(&key_queue_mutex);
                    continue;
                }
            }
            pthread_mutex_unlock(&key_queue_mutex);
            return key;
        }
    } while (1);

    return key;
}

int ui_key_pressed(int key)
{
    // This is a volatile static array, don't bother locking
    return key_pressed[key];
}

void ui_clear_key_queue() {
    pthread_mutex_lock(&key_queue_mutex);
    key_queue_len = 0;
    pthread_mutex_unlock(&key_queue_mutex);
}

void ui_set_log_stdout(int enabled) {
    ui_log_stdout = enabled;
}

int ui_should_log_stdout()
{
    return ui_log_stdout;
}

void ui_set_show_text(int value) {
    show_text = value;
}

void ui_set_showing_back_button(int showBackButton) {
    gShowBackButton = showBackButton;
}

int ui_get_showing_back_button() {
    return gShowBackButton;
}

int ui_is_showing_back_button() {
    return gShowBackButton && !ui_root_menu;
}

int ui_get_selected_item() {
  return menu_sel;
}

int ui_handle_key(int key, int visible) {
#ifdef BOARD_TOUCH_RECOVERY
    return touch_handle_key(key, visible);
#else
    return device_handle_key(key, visible);
#endif
}

void ui_delete_line() {
    pthread_mutex_lock(&gUpdateMutex);
    text[text_row][0] = '\0';
    text_row = (text_row - 1 + text_rows) % text_rows;
    text_col = 0;
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_increment_frame() {
    gInstallingFrame =
        (gInstallingFrame + 1) % ui_parameters.installing_frames;
}

int ui_get_rainbow_mode() {
    return gRainbowMode;
}

void ui_rainbow_mode() {
    static int colors[] = { 255, 0, 0,        // red
                            255, 127, 0,      // orange
                            255, 255, 0,      // yellow
                            0, 255, 0,        // green
                            60, 80, 255,      // blue
                            143, 0, 255 };    // violet

    gr_color(colors[cur_rainbow_color], colors[cur_rainbow_color+1], colors[cur_rainbow_color+2], 255);
    cur_rainbow_color += 3;
    if (cur_rainbow_color >= (sizeof(colors) / sizeof(colors[0]))) cur_rainbow_color = 0;
}

void ui_set_rainbow_mode(int rainbowMode) {
    gRainbowMode = rainbowMode;

    pthread_mutex_lock(&gUpdateMutex);
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

