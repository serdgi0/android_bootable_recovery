/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (C) 2013-2015 Project Open Cannibal
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
#include "recovery_ui.h"

// Include extendedcommands.h in order to get our custom ui colors
#include "extendedcommands.h"

//these are included in the original kernel's linux/input.h but are missing from AOSP

#ifndef SYN_MT_REPORT
#define SYN_MT_REPORT 2
#define ABS_MT_TOUCH_MAJOR  0x30  /* Major axis of touching ellipse */
#define ABS_MT_WIDTH_MAJOR  0x32  /* Major axis of approaching ellipse */
#define ABS_MT_POSITION_X 0x35  /* Center X ellipse position */
#define ABS_MT_POSITION_Y 0x36  /* Center Y ellipse position */
#define ABS_MT_TRACKING_ID 0x39  /* Center Y ellipse position */
#endif

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
#include "settings.h"
#include "settingshandler.h"
#include "iniparse/ini.h"

extern int __system(const char *command);

#if defined(BOARD_HAS_NO_SELECT_BUTTON) || defined(BOARD_TOUCH_RECOVERY)
static int gShowBackButton = 1;
#else
static int gShowBackButton = 0;
#endif

#define MAX_COLS 96
#ifdef BOARD_TS_MAX_ROWS
#define MAX_ROWS BOARD_TS_MAX_ROWS
#else
#define MAX_ROWS 30
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
static gr_surface gMenuIcon[NUM_MENU_ICON];
static gr_surface *gInstallationOverlay;
static gr_surface *gProgressBarIndeterminate;
static gr_surface gProgressBarEmpty;
static gr_surface gProgressBarFill;
static gr_surface gBackground;
static int ui_has_initialized = 0;
static int ui_log_stdout = 1;
static int selMenuIcon = 0;

static int boardEnableKeyRepeat = 0;
static int boardRepeatableKeys[64], boardNumRepeatableKeys = 0;

static const struct { gr_surface* surface; const char *name; } BITMAPS[] = {
    { &gBackgroundIcon[BACKGROUND_ICON_INSTALLING], "icon_installing" },
    { &gBackgroundIcon[BACKGROUND_ICON_ERROR],      "icon_error" },
    { &gBackgroundIcon[BACKGROUND_ICON_CLOCKWORK],  "icon_background" },
    { &gBackgroundIcon[BACKGROUND_ICON_TSCAL],	"tscal_step" },
    { &gBackgroundIcon[BACKGROUND_ICON_CID],  "icon_cid" },
    { &gBackgroundIcon[BACKGROUND_ICON_FIRMWARE_INSTALLING], "icon_firmware_install" },
    { &gBackgroundIcon[BACKGROUND_ICON_FIRMWARE_ERROR], "icon_firmware_error" },
    { &gMenuIcon[MENU_BACK],      "icon_back" },
    { &gMenuIcon[MENU_DOWN],  	  "icon_down" },
    { &gMenuIcon[MENU_UP], 		  "icon_up" },
    { &gMenuIcon[MENU_SELECT],    "icon_select" },
	{ &gMenuIcon[MENU_BACK_M],    "icon_backM" },
    { &gMenuIcon[MENU_DOWN_M],    "icon_downM" },
    { &gMenuIcon[MENU_UP_M], 	  "icon_upM" },
    { &gMenuIcon[MENU_SELECT_M],  "icon_selectM" },
    { &gProgressBarEmpty,               "progress_empty" },
    { &gProgressBarFill,                "progress_fill" },
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

static int cur_rainbow_color = 0;
static int gRainbowMode = 0;

// Key event input queue
static pthread_mutex_t key_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t key_queue_cond = PTHREAD_COND_INITIALIZER;
static int key_queue[256], key_queue_len = 0, key_queue_len_back = 0;
static unsigned long key_last_repeat[KEY_MAX + 1], key_press_time[KEY_MAX + 1];
static volatile char key_pressed[KEY_MAX + 1];

// Threads
static pthread_t pt_ui_thread;
static pthread_t pt_input_thread;
static volatile int pt_ui_thread_active = 1;
static volatile int pt_input_thread_active = 1;

// Desire/Nexus and similar have 2, SGS has 5, SGT has 10, we take the max as it's cool. We'll only use 1 however
#define MAX_MT_POINTS 10

// Struct to store mouse events
static struct mousePosStruct {
  int x;
  int y;
  int pressure; // 0:up or 255:down
  int size;
  int num;
  int length; // length of the line drawn while in touch state
} actPos, grabPos, oldMousePos[MAX_MT_POINTS], mousePos[MAX_MT_POINTS];
//Struct to return key events to recovery.c through ui_wait_key()
volatile struct keyStruct key;

void update_screen_locked(void);

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
    gr_color(0, 0, 0, 255);
    gr_fill(0, 0, gr_fb_width(), gr_fb_height());

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

// Draw the currently selected icon (if any) at given location.
// Should only be called with gUpdateMutex locked.
static void draw_icon_locked(gr_surface icon,int locX, int locY)
{
    gPagesIdentical = 0;

    if (icon) {
        int iconWidth = gr_get_width(icon);
        int iconHeight = gr_get_height(icon);
        int iconX = locX - iconWidth / 2;
        int iconY = locY - iconHeight / 2;
        gr_blit(icon, 0, 0, iconWidth, iconHeight, iconX, iconY);
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
        int dy = (3*gr_fb_height() + iconHeight - 2*height)/4;

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

static void draw_text_line(int row, const char* t, int align) {
	int col = 0; 
	if (t[0] != '\0') {
		int length = strnlen(t, MENU_MAX_COLS) * CHAR_WIDTH;
		switch(align)
		{
			case LEFT_ALIGN:
				col = 1;
				break;
			case CENTER_ALIGN:
				col = ((gr_fb_width() - length) / 2);
				break;
			case RIGHT_ALIGN:
				col = gr_fb_width() - length - 1;
				break;
		}
		if (ui_get_rainbow_mode()) ui_rainbow_mode();
		gr_text(col, (row+1)*CHAR_HEIGHT-1, t);
	}
}

//#define MENU_TEXT_COLOR 255, 0, 0, 255
#define NORMAL_TEXT_COLOR 200, 200, 200, 255
#define HEADER_TEXT_COLOR NORMAL_TEXT_COLOR

int BATT_LINE, TIME_LINE, BATT_POS, TIME_POS;

// Redraw everything on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
void draw_screen_locked(void)
{
	if (!ui_has_initialized) return;
#ifdef BUILD_IN_LANDSCAPE
		//In this case MENU_SELECT icon has maximum possible height.
		int menu_max_height = gr_get_height(gMenuIcon[MENU_SELECT]);
		struct { int x; int y; } MENU_ICON[] = {
			{  gr_fb_width() - menu_max_height, 7*gr_fb_height()/8 },
			{  gr_fb_width() - menu_max_height,	5*gr_fb_height()/8 },
			{  gr_fb_width() - menu_max_height,	3*gr_fb_height()/8 },
			{  gr_fb_width() - menu_max_height,	1*gr_fb_height()/8 }, 
#else
//ToDo: Following structure should be global
		struct { int x; int y; int xL; int xR; } MENU_ICON[] = {
			{  get_menu_icon_info(MENU_BACK,MENU_ICON_X),	get_menu_icon_info(MENU_BACK,MENU_ICON_Y), get_menu_icon_info(MENU_BACK,MENU_ICON_XL), get_menu_icon_info(MENU_BACK,MENU_ICON_XR) },
			{  get_menu_icon_info(MENU_DOWN,MENU_ICON_X),	get_menu_icon_info(MENU_DOWN,MENU_ICON_Y), get_menu_icon_info(MENU_DOWN,MENU_ICON_XL), get_menu_icon_info(MENU_DOWN,MENU_ICON_XR) },
			{  get_menu_icon_info(MENU_UP,MENU_ICON_X),	get_menu_icon_info(MENU_UP,MENU_ICON_Y), get_menu_icon_info(MENU_UP,MENU_ICON_XL), get_menu_icon_info(MENU_UP,MENU_ICON_XR) },
			{  get_menu_icon_info(MENU_SELECT,MENU_ICON_X),	get_menu_icon_info(MENU_SELECT,MENU_ICON_Y), get_menu_icon_info(MENU_SELECT,MENU_ICON_XL), get_menu_icon_info(MENU_SELECT,MENU_ICON_XR) },
#endif
		};

    		draw_background_locked(gCurrentIcon);
    		draw_progress_locked();

		if (show_text) {
	        	gr_color(0, 0, 0, 160);
        		gr_fill(0, 0, gr_fb_width(), gr_fb_height());

        		int i = 0;
        		int j = 0;
        		int row = 0;            // current row that we are drawing on
        		if (show_menu) {
				draw_icon_locked(gMenuIcon[MENU_BACK], MENU_ICON[MENU_BACK].x, MENU_ICON[MENU_BACK].y );
				draw_icon_locked(gMenuIcon[MENU_DOWN], MENU_ICON[MENU_DOWN].x, MENU_ICON[MENU_DOWN].y);
				draw_icon_locked(gMenuIcon[MENU_UP], MENU_ICON[MENU_UP].x, MENU_ICON[MENU_UP].y );
				draw_icon_locked(gMenuIcon[MENU_SELECT], MENU_ICON[MENU_SELECT].x, MENU_ICON[MENU_SELECT].y );
            			// Setup our text colors
            			gr_color(UICOLOR0, UICOLOR1, UICOLOR2, 255);
            
            			// Show battery level
            			int batt_level = 0;
            			batt_level = get_batt_stats();
            			if(batt_level < 21) {
					gr_color(255, 0, 0, 255);
				}
				char batt_text[40];
				char time_gmt[40];
			
				// Get a usable time
				struct tm *current;
				time_t now;
				now = time(0);
				current = localtime(&now);
				sprintf(batt_text, "[%d%%]", batt_level);
				sprintf(time_gmt, "[%02D:%02D GMT]", current->tm_hour, current->tm_min);

       			draw_text_line(BATT_LINE, batt_text, BATT_POS);
       			draw_text_line(TIME_LINE, time_gmt, TIME_POS);

				gr_color(UICOLOR0, UICOLOR1, UICOLOR2, 255);

            		gr_fill(0, (menu_top + menu_sel - menu_show_start) * CHAR_HEIGHT,
#ifdef BUILD_IN_LANDSCAPE
					gr_fb_width()-menu_max_height*2, (menu_top + menu_sel - menu_show_start + 1)*CHAR_HEIGHT+1);
#else
					gr_fb_width(), (menu_top + menu_sel - menu_show_start + 1)*CHAR_HEIGHT+1);
#endif


            			gr_color(HEADER_TEXT_COLOR);
            			for (i = 0; i < menu_top; ++i) {
					draw_text_line(i, menu[i], LEFT_ALIGN);
                			row++;
            			}

	            		if (menu_items - menu_show_start + menu_top >= MAX_ROWS)
                			j = MAX_ROWS - menu_top;
            			else
                			j = menu_items - menu_show_start;

            			gr_color(UICOLOR0, UICOLOR1, UICOLOR2, 255);
            			for (i = menu_show_start + menu_top; i < (menu_show_start + menu_top + j); ++i) {
                			if (i == menu_top + menu_sel) {
                    				gr_color(255, 255, 255, 255);
                    				draw_text_line(i - menu_show_start , menu[i], LEFT_ALIGN);
                    				gr_color(UICOLOR0, UICOLOR1, UICOLOR2, 255);
                		} else {
                    			gr_color(UICOLOR0, UICOLOR1, UICOLOR2, 255);
                    			draw_text_line(i - menu_show_start, menu[i], LEFT_ALIGN);
                		}
                		row++;
                		if (row >= max_menu_rows)
		                    break;
            		}
            		gr_fill(0, row*CHAR_HEIGHT+CHAR_HEIGHT/2-1,
#ifdef BUILD_IN_LANDSCAPE
				gr_fb_width()-menu_max_height*2, row*CHAR_HEIGHT+CHAR_HEIGHT/2+1);
#else
				gr_fb_width(), row*CHAR_HEIGHT+CHAR_HEIGHT/2+1);
#endif
        	}

        	gr_color(NORMAL_TEXT_COLOR);
        	for (; row < text_rows; ++row) {
        		draw_text_line(row + 1, text[(row+text_top) % text_rows], LEFT_ALIGN);
        	}
    	}
}

// Redraw everything on the screen and flip the screen (make it visible).
// Should only be called with gUpdateMutex locked.
void update_screen_locked(void)
{
    if (!ui_has_initialized) return;
    draw_screen_locked();
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
            ui_parameters.installing_frames > 0 &&
            !show_text) {
            gInstallingFrame =
                (gInstallingFrame + 1) % ui_parameters.installing_frames;
            redraw = 1;
        }

        // update the progress bar animation, if active
        // skip this if we have a text overlay (too expensive to update)
        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE && !show_text) {
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

static int input_callback(int fd, short revents, void *data)
{
    struct input_event ev;
    int rel_sum_x = 0;
    int rel_sum_y = 0;
    int ret;
    int fake_key = 0;
    int got_data = 0;

    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;

    if (ev.type == EV_SYN) {
        // end of a multitouch point
        if (ev.code == SYN_MT_REPORT) {
			if (touchY > 0 && actPos.y < touchY) {
				actPos.num = 0;
				actPos.x = 0;
				actPos.y = 0;
				actPos.pressure = 0;
				actPos.size = 0;
			}
			if (actPos.num>=0 && actPos.num<MAX_MT_POINTS) {
				// create a fake keyboard event. We will use BTN_WHEEL, BTN_GEAR_DOWN and BTN_GEAR_UP key events to fake
				// TOUCH_MOVE, TOUCH_DOWN and TOUCH_UP in this order
				int type = BTN_WHEEL;
				// new and old pressure state are not consistent --> we have touch down or up event
				if ((mousePos[actPos.num].pressure!=0) != (actPos.pressure!=0)) {
					if (actPos.pressure == 0) {
						type = BTN_GEAR_UP;
						if (actPos.num==0) {
							if (mousePos[0].length<15) {
								// consider this a mouse click
								type = BTN_MOUSE;
							}
							memset(&grabPos,0,sizeof(grabPos));
						}
					} else if (actPos.pressure != 0) {
						type == BTN_GEAR_DOWN;
						if (actPos.num==0) {
							grabPos = actPos;
						}
					}
				}
				
				fake_key = 1;
				ev.type = EV_KEY;
				ev.code = type;
				ev.value = actPos.num+1;
				
				// this should be locked, but that causes ui events to get dropped, as the screen drawing takes too much time
				// this should be solved by making the critical section inside the drawing much much smaller
				if (actPos.pressure) {
					if (mousePos[actPos.num].pressure) {
						actPos.length = mousePos[actPos.num].length + abs(mousePos[actPos.num].x-actPos.x) + abs(mousePos[actPos.num].y-actPos.y);
					} else {
						actPos.length = 0;
					}
				} else {
					actPos.length = 0;
				}
				oldMousePos[actPos.num] = mousePos[actPos.num];
				mousePos[actPos.num] = actPos;
				int curPos[] = {actPos.pressure, actPos.x, actPos.y};
				ui_handle_mouse_input(curPos);
			}
			
			memset(&actPos,0,sizeof(actPos));
		} else {
			return 0;
		}
	} else if (ev.type == EV_ABS) {
		// multitouch records are sent as ABS events. Well at least on the SGS-i9000
		if (ev.code == ABS_MT_POSITION_X) {
			actPos.x = MT_X(ev.value);
		} else if (ev.code == ABS_MT_POSITION_Y) {
			actPos.y = MT_Y(ev.value);
			if (touchY > 0 && actPos.y < touchY) { actPos.y = 0; }
		} else if (ev.code == ABS_MT_TOUCH_MAJOR) {
			actPos.pressure = ev.value; // on SGS-i9000 this is 0 for not-pressed and 40 for pressed
		} else if (ev.code == ABS_MT_WIDTH_MAJOR) {
			// num is stored inside the high byte of width. Well at least on SGS-i9000
			if (actPos.num==0) {
				// only update if it was not already set. On a normal device MT_TRACKING_ID is sent
				actPos.num = ev.value >> 8;
			}
			actPos.size = ev.value & 0xFF;
		} else if (ev.code == ABS_MT_TRACKING_ID) {
			// on a normal device, the num is got from this value
			actPos.num = ev.value;
		}
	} else if (ev.type == EV_REL) {
        if (ev.code == REL_Y) {
			// accumulate the up or down motion reported by
			// the trackball.  When it exceeds a threshol
			// (positive or negative), fake an up/down
			// key event.
			rel_sum_y += ev.value;
			if (rel_sum_y > 3) { 
				fake_key = 1;
				ev.type = EV_KEY;
				ev.code = KEY_DOWN;
				ev.value = 1;
				rel_sum_y = 0;
            } else if (rel_sum_y < -3) {
				fake_key = 1;
				ev.type = EV_KEY;
				ev.code = KEY_UP;
				ev.value = 1;
				rel_sum_y = 0;
            }
		}
		// do the same for the X axis
		if (ev.code == REL_X) {
			rel_sum_x += ev.value;
			if (rel_sum_x > 3) {
				fake_key = 1;
				ev.type = EV_KEY;
				ev.code = KEY_RIGHT;
				ev.value = 1;
				rel_sum_x = 0;
            } else if (rel_sum_x < -3) {
				fake_key = 1;
				ev.type = EV_KEY;
				ev.code = KEY_LEFT;
				ev.value = 1;
				rel_sum_x = 0;
            }
        }
    } else {
        rel_sum = 0;
        rel_sum_y = 0;
        rel_sum_x = 0;
    }

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
		// we don't want to pollute the queue with mouse move events
		if (ev.code!=BTN_WHEEL || key_queue_len==0 || key_queue[key_queue_len-1]!=BTN_WHEEL) {
			key_queue[key_queue_len++] = ev.code;
		}

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
        pass_normal_reboot();
    }

    return 0;
}

// handle the action associated with user input touch events inside the ui handler
int device_handle_mouse(struct keyStruct *key, int visible)
{
// ToDo: Following structures should be global
#ifdef BUILD_IN_LANDSCAPE
	struct { int xL; int xR; } MENU_ICON[] = {
		{  3*gr_fb_height()/4, 4*gr_fb_height()/4 },
		{  2*gr_fb_height()/4, 3*gr_fb_height()/4 },
		{  1*gr_fb_height()/4, 2*gr_fb_height()/4 },
		{  0*gr_fb_height()/4, 1*gr_fb_height()/4 },
#else
	struct { int x; int y; int xL; int xR; } MENU_ICON[] = {
		{  get_menu_icon_info(MENU_BACK,MENU_ICON_X),	get_menu_icon_info(MENU_BACK,MENU_ICON_Y), get_menu_icon_info(MENU_BACK,MENU_ICON_XL), get_menu_icon_info(MENU_BACK,MENU_ICON_XR) },
		{  get_menu_icon_info(MENU_DOWN,MENU_ICON_X),	get_menu_icon_info(MENU_DOWN,MENU_ICON_Y), get_menu_icon_info(MENU_DOWN,MENU_ICON_XL), get_menu_icon_info(MENU_DOWN,MENU_ICON_XR) },
		{  get_menu_icon_info(MENU_UP,MENU_ICON_X),	get_menu_icon_info(MENU_UP,MENU_ICON_Y), get_menu_icon_info(MENU_UP,MENU_ICON_XL), get_menu_icon_info(MENU_UP,MENU_ICON_XR) },
		{  get_menu_icon_info(MENU_SELECT,MENU_ICON_X),	get_menu_icon_info(MENU_SELECT,MENU_ICON_Y), get_menu_icon_info(MENU_SELECT,MENU_ICON_XL), get_menu_icon_info(MENU_SELECT,MENU_ICON_XR) },
#endif
	};

	if (visible) {
	int position;

#ifdef BUILD_IN_LANDSCAPE
		position = gr_fb_height() - key->x;
#else
		position = key->x;
#endif

		if(position > MENU_ICON[MENU_BACK].xL && position < MENU_ICON[MENU_BACK].xR)
			return GO_BACK;
		else if(position > MENU_ICON[MENU_DOWN].xL && position < MENU_ICON[MENU_DOWN].xR)
			return HIGHLIGHT_DOWN;
		else if(position > MENU_ICON[MENU_UP].xL && position < MENU_ICON[MENU_UP].xR)
			return HIGHLIGHT_UP;
		else if(position > MENU_ICON[MENU_SELECT].xL && position < MENU_ICON[MENU_SELECT].xR)
			return SELECT_ITEM;
    }
	return NO_ACTION;
}

// handle the user input events (mainly the touch events) inside the ui handler
void ui_handle_mouse_input(int* curPos)
{
	pthread_mutex_lock(&key_queue_mutex);

//ToDo: Following structure should be global
#ifdef BUILD_IN_LANDSCAPE
	//In this case MENU_SELECT icon has maximum possible height.
	int menu_max_height = gr_get_height(gMenuIcon[MENU_SELECT]);
	struct { int x; int y; int xL; int xR; } MENU_ICON[] = {
		{  gr_fb_width() - menu_max_height, 7*gr_fb_height()/8, 3*gr_fb_height()/4, 4*gr_fb_height()/4  },
		{  gr_fb_width() - menu_max_height,	5*gr_fb_height()/8, 2*gr_fb_height()/4, 3*gr_fb_height()/4  },
		{  gr_fb_width() - menu_max_height,	3*gr_fb_height()/8, 1*gr_fb_height()/4, 2*gr_fb_height()/4  },
		{  gr_fb_width() - menu_max_height,	1*gr_fb_height()/8, 0*gr_fb_height()/4, 1*gr_fb_height()/4  },
#else
	struct { int x; int y; int xL; int xR; } MENU_ICON[] = {
		{  get_menu_icon_info(MENU_BACK,MENU_ICON_X),	get_menu_icon_info(MENU_BACK,MENU_ICON_Y), get_menu_icon_info(MENU_BACK,MENU_ICON_XL), get_menu_icon_info(MENU_BACK,MENU_ICON_XR) },
		{  get_menu_icon_info(MENU_DOWN,MENU_ICON_X),	get_menu_icon_info(MENU_DOWN,MENU_ICON_Y), get_menu_icon_info(MENU_DOWN,MENU_ICON_XL), get_menu_icon_info(MENU_DOWN,MENU_ICON_XR) },
		{  get_menu_icon_info(MENU_UP,MENU_ICON_X),	get_menu_icon_info(MENU_UP,MENU_ICON_Y), get_menu_icon_info(MENU_UP,MENU_ICON_XL), get_menu_icon_info(MENU_UP,MENU_ICON_XR) },
		{  get_menu_icon_info(MENU_SELECT,MENU_ICON_X),	get_menu_icon_info(MENU_SELECT,MENU_ICON_Y), get_menu_icon_info(MENU_SELECT,MENU_ICON_XL), get_menu_icon_info(MENU_SELECT,MENU_ICON_XR) },
#endif
	};

if(TOUCH_CONTROL_DEBUG)
{
	ui_print("Touch gr_fb_width:\t%d,\tgr_fb_height:\t%d\n",gr_fb_width(),gr_fb_height());
	ui_print("Touch X:\t%d,\tY:\t%d\n",curPos[1],curPos[2]);
}

  if (show_menu) {
    if (curPos[0] > 0) {
		int position;
#ifdef BUILD_IN_LANDSCAPE
		position = gr_fb_height() - curPos[1];
#else
		position = curPos[1];
#endif

		pthread_mutex_lock(&gUpdateMutex);
#ifdef BUILD_IN_LANDSCAPE
		if(position > MENU_ICON[MENU_BACK].xL && position < MENU_ICON[MENU_BACK].xR && selMenuIcon != MENU_BACK) {
#else
		if(position > MENU_ICON[MENU_BACK].xL && position < MENU_ICON[MENU_BACK].xR ) {
#endif
			draw_icon_locked(gMenuIcon[selMenuIcon], MENU_ICON[selMenuIcon].x, MENU_ICON[selMenuIcon].y );
			draw_icon_locked(gMenuIcon[MENU_BACK_M], MENU_ICON[MENU_BACK].x, MENU_ICON[MENU_BACK].y );
			selMenuIcon = MENU_BACK;
			gr_flip();
		}
#ifdef BUILD_IN_LANDSCAPE
		else if(position > MENU_ICON[MENU_DOWN].xL && position < MENU_ICON[MENU_DOWN].xR && selMenuIcon != MENU_DOWN) {			
#else
		else if(position > MENU_ICON[MENU_DOWN].xL && position < MENU_ICON[MENU_DOWN].xR ) {
#endif
			draw_icon_locked(gMenuIcon[selMenuIcon], MENU_ICON[selMenuIcon].x, MENU_ICON[selMenuIcon].y );
			draw_icon_locked(gMenuIcon[MENU_DOWN_M], MENU_ICON[MENU_DOWN].x, MENU_ICON[MENU_DOWN].y);
			selMenuIcon = MENU_DOWN;
			gr_flip();
		}
#ifdef BUILD_IN_LANDSCAPE
		else if(position > MENU_ICON[MENU_UP].xL && position < MENU_ICON[MENU_UP].xR && selMenuIcon != MENU_UP) {
#else
		else if(position > MENU_ICON[MENU_UP].xL && position < MENU_ICON[MENU_UP].xR ) {
#endif
			draw_icon_locked(gMenuIcon[selMenuIcon], MENU_ICON[selMenuIcon].x, MENU_ICON[selMenuIcon].y );			
			draw_icon_locked(gMenuIcon[MENU_UP_M], MENU_ICON[MENU_UP].x, MENU_ICON[MENU_UP].y );
			selMenuIcon = MENU_UP;
			gr_flip();
		}
#ifdef BUILD_IN_LANDSCAPE
		else if(position > MENU_ICON[MENU_SELECT].xL && position < MENU_ICON[MENU_SELECT].xR && selMenuIcon != MENU_SELECT) {
#else
		else if(position > MENU_ICON[MENU_SELECT].xL && position < MENU_ICON[MENU_SELECT].xR ) {
#endif
			draw_icon_locked(gMenuIcon[selMenuIcon], MENU_ICON[selMenuIcon].x, MENU_ICON[selMenuIcon].y );			
			draw_icon_locked(gMenuIcon[MENU_SELECT_M], MENU_ICON[MENU_SELECT].x, MENU_ICON[MENU_SELECT].y );
			selMenuIcon = MENU_SELECT;
			gr_flip();
		}
		key_queue_len_back = key_queue_len;
		pthread_mutex_unlock(&gUpdateMutex);
     }
  }
  pthread_mutex_unlock(&key_queue_mutex);
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

int i;

void ui_init_icons(void) {
    int result;
    switch(is_sd_theme) {
		case 0:
			for (i = 0; BITMAPS[i].name != NULL; ++i) {
				result = res_create_recovery_surface(BITMAPS[i].name, BITMAPS[i].surface, 0, themename);
			}
			break;
		case 1:
			for (i = 0; BITMAPS[i].name != NULL; ++i) {
				result = res_create_recovery_surface(BITMAPS[i].name, BITMAPS[i].surface, 1, themename);
			}
			break;
		default:
			for (i = 0; BITMAPS[i].name != NULL; ++i) {
				result = res_create_recovery_surface(BITMAPS[i].name, BITMAPS[i].surface, 0, "hydro");
			}
			break;
    }
    if (result < 0) {
		LOGE("Missing bitmap %s\n(Code %d)\n", BITMAPS[i].name, result);
    }
}

void ui_init(void)
{
    ui_has_initialized = 1;
    gr_init();
    ev_init(input_callback, NULL);

    text_col = text_row = 0;
    text_rows = gr_fb_height() / CHAR_HEIGHT;
    max_menu_rows = text_rows - MIN_LOG_ROWS;
    if (max_menu_rows > MENU_MAX_ROWS)
        max_menu_rows = MENU_MAX_ROWS;
    if (text_rows > MAX_ROWS) text_rows = MAX_ROWS;
    text_top = 1;

    text_cols = gr_fb_width() / CHAR_WIDTH;
    if (text_cols > MAX_COLS - 1) text_cols = MAX_COLS - 1;

    ui_init_icons();

    gProgressBarIndeterminate = malloc(ui_parameters.indeterminate_frames *
                                       sizeof(gr_surface));
    int frame_result;
    for (i = 0; i < ui_parameters.indeterminate_frames; ++i) {
        char filename[40];
        // "indeterminate01.png", "indeterminate02.png", ...
        sprintf(filename, "indeterminate%02d", i+1);
        switch(is_sd_theme) {
			case 0:
				frame_result = res_create_recovery_surface(filename, gProgressBarIndeterminate+i, 0, themename);
				break;
			case 1:
				frame_result = res_create_recovery_surface(filename, gProgressBarIndeterminate+i, 1, themename);
				break;
			default:
				frame_result = res_create_recovery_surface(filename, gProgressBarIndeterminate+i, 0, "hydro");
				break;
		}
        if (frame_result < 0) {
            LOGE("Missing bitmap %s\n(Code %d)\n", filename, frame_result);
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
            switch(is_sd_theme) {
				case 0:
					frame_result = res_create_recovery_surface(filename, gInstallationOverlay+i, 0, themename);
					break;
				case 1:
					frame_result = res_create_recovery_surface(filename, gInstallationOverlay+i, 1, themename);
					break;
				default:
					frame_result = res_create_recovery_surface(filename, gInstallationOverlay+i, 0, "hydro");
					break;
			}
            if (frame_result < 0) {
                LOGE("Missing bitmap %s\n(Code %d)\n", filename, frame_result);
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

    memset(&actPos, 0, sizeof(actPos));
    memset(&grabPos, 0, sizeof(grabPos));
    memset(mousePos, 0, sizeof(mousePos));
    memset(oldMousePos, 0, sizeof(oldMousePos));

    pt_ui_thread_active = 1;
    pt_input_thread_active = 1;

    pthread_create(&pt_ui_thread, NULL, progress_thread, NULL);
    pthread_create(&pt_input_thread, NULL, input_thread, NULL);
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
	if(TOUCH_NOSHOW_LOG);
	else {
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

#define MENU_ITEM_HEADER " - "
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

        if (gShowBackButton && !ui_root_menu) {
            strcpy(menu[i], " - +++++Go Back+++++");
            ++i;
        }

        menu_items = i - menu_top;
        show_menu = 1;
        menu_sel = menu_show_start = initial_selection;
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
    if (gShowBackButton && !ui_root_menu) {
        return menu_items - 1;
    }
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

void ui_reset_icons()
{
	pthread_mutex_lock(&gUpdateMutex);
	ui_init_icons();
	update_screen_locked();
	pthread_mutex_unlock(&gUpdateMutex);
}

void ui_cancel_wait_key() {
    pthread_mutex_lock(&key_queue_mutex);
    key_queue[key_queue_len] = -2;
    key_queue_len++;
    pthread_cond_signal(&key_queue_cond);
    pthread_mutex_unlock(&key_queue_mutex);
}

struct keyStruct *ui_wait_key()
{
    if (boardEnableKeyRepeat) return ui_wait_key_with_repeat();
    pthread_mutex_lock(&key_queue_mutex);

    // Time out after UI_WAIT_KEY_TIMEOUT_SEC, unless a USB cable is
    // plugged in.
    do {
        struct timeval now;
        struct timespec timeout;
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec;
        timeout.tv_nsec = now.tv_usec * 1000;
        timeout.tv_sec += UI_WAIT_KEY_TIMEOUT_SEC;

        int rc = 0;
        while (key_queue_len == 0 && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex,
                                        &timeout);
        }
    } while (usb_connected() && key_queue_len == 0);

    key.code = key_queue[0];
    memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
    if(TOUCH_CONTROL_DEBUG)
		ui_print("[UI_WAIT_KEY] key code:\t%d\n",key.code);

	if((key.code == BTN_GEAR_UP || key.code == BTN_MOUSE) && !actPos.pressure && oldMousePos[actPos.num].pressure && key_queue_len_back != (key_queue_len -1))
	{	
		key.code = ABS_MT_POSITION_X;
		key.x = oldMousePos[actPos.num].x;
		key.y = oldMousePos[actPos.num].y;
	}
    pthread_mutex_unlock(&key_queue_mutex);
    return &key;
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
        struct timeval now;
        struct timespec timeout;
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec;
        timeout.tv_nsec = now.tv_usec * 1000;
        timeout.tv_sec += UI_WAIT_KEY_TIMEOUT_SEC;

        int rc = 0;
        pthread_mutex_lock(&key_queue_mutex);
        while (key_queue_len == 0 && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex,
                                        &timeout);
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

int get_batt_stats(void) {
	static int level = -1;
	
	char value[4];
	FILE * capacity = fopen("/sys/class/power_supply/battery/capacity","rt");
	if (capacity) {
		fgets(value, 4, capacity);
		fclose(capacity);
		level = atoi(value);
		
		if (level > 100)
			level = 100;
		if (level < 0)
			level = 0;
		
	}
	return level;
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
    if (cur_rainbow_color >= sizeof(colors)/sizeof(colors[0])) cur_rainbow_color = 0;
}

void ui_set_rainbow_mode(int rainbowMode) {
    gRainbowMode = rainbowMode;

    pthread_mutex_lock(&gUpdateMutex);
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}
