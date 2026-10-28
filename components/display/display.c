/* 
 *  (c) Philippe G. 2019, philippe_44@outlook.com
 *
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "globdefs.h"
#include "platform_config.h"
#include "tools.h"
#include "display.h"
#include "services.h"
#include "gds.h"
#include "gds_private.h"
#include "gds_default_if.h"
#include "gds_draw.h"
#include "gds_text.h"
#include "gds_font.h"
#include "gds_image.h"

static const char *TAG = "display";

#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))

#define DISPLAYER_STACK_SIZE 	(6*1024)
#define SCROLLABLE_SIZE			384
#define HEADER_SIZE				64
#define	DEFAULT_SLEEP			3600
#define ARTWORK_BORDER			1

/* UI/UX Pro Max Sleek Dark OLED Audio Streamer Tokens (True RGB565) */
#define HIFI_BG			0x0022  /* Deep OLED Pitch Black-Navy #050714 */
#define HIFI_CARD		0x10C5  /* Dark Slate-Indigo Glass Card #11182B */
#define HIFI_PILL		0x1948  /* Elevated Pill / Glass Rim #1A2644 */
#define HIFI_TRACK		0x1926  /* Dim LED / Scrubber Track #1E2538 */
#define HIFI_VIOLET		0x633E  /* Royal Indigo-Violet #6366F1 */
#define HIFI_CYAN		0x073F  /* Electric Cyan #00E5FF */
#define HIFI_EMERALD	0x16EA  /* Neon Emerald #10DD55 */
#define HIFI_ROSE		0xF9EC  /* Coral Rose Peak #F43F63 */
#define HIFI_GOLD		0xFD60  /* Warm Hi-Res Gold #F5AC00 */
#define HIFI_SLATE		0x9536  /* Muted Slate Label #94A3B8 */
#define HIFI_WHITE		0xFFFF  /* Crisp White Text #FFFFFF */

/* Real-time audio peak level (0..100) updated by output_i2s.c */
volatile uint8_t g_audio_peak_level = 0;
static bool g_has_custom_artwork = false;

extern struct led_strip_t* led_display;
extern void led_vu_color_all(uint8_t r, uint8_t g, uint8_t b);

extern const uint8_t default_artwork[]   asm("_binary_note_jpg_start");
extern bool is_recovery_running;

static EXT_RAM_ATTR struct {
	TaskHandle_t task;
	SemaphoreHandle_t mutex;
	int pause, speed, by;
	enum { DISPLAYER_DOWN, DISPLAYER_IDLE, DISPLAYER_ACTIVE } state;
	char header[HEADER_SIZE + 1];
	char string[SCROLLABLE_SIZE + 1];
	int offset, boundary;
	char *metadata_config;
	bool timer, refresh;
	uint32_t elapsed;
	struct {
		uint32_t value;
		char string[8]; // H:MM:SS
		bool visible;
	} duration;
	struct {
		bool enable, active;
		bool fit;
		bool updated;
		int tick;
		int offset;
	}  artwork;
	TickType_t tick;
	TickType_t progress_tick;
} displayer;

/* =========================================================================
 * PERFECT 4-BUTTON MEDIA CONTROLLER & TACTILE OSD / DISPLAY STATE
 * K1 (GPIO 13): Click = Prev Track, Hold = Volume Down
 * K2 (GPIO 14): Click = Next Track, Hold = Volume Up
 * K3 (GPIO 15): Click = Play / Pause, Hold = Seek Rewind (-10s)
 * K4 (GPIO 16): Click = Brightness (100%->60%->30%), Dbl-click = Sleep, Hold = Seek Forward (+10s)
 * Wake on ANY button when sleeping!
 * ========================================================================= */
typedef enum {
	LED_MODE_BEAT = 0,     /* Default: Live audio beat visualizer */
	LED_MODE_RAINBOW = 1,  /* Smooth rainbow color cycle */
	LED_MODE_STATIC = 2,   /* Soft warm amber glow */
	LED_MODE_OFF = 3       /* Completely off */
} led_mode_t;

static volatile led_mode_t g_led_mode = LED_MODE_BEAT;
static volatile uint8_t g_backlight_level = 255; /* 255 (100%), 160 (60%), 80 (30%) */
static volatile bool g_display_sleep_forced = false;
static volatile TickType_t g_last_user_activity = 0;

/* On-Screen Display (OSD) feedback overlay */
static char g_osd_text[32] = "";
static TickType_t g_osd_expire_tick = 0;
static uint16_t g_osd_color = HIFI_CYAN;
static TickType_t g_tactile_led_expire = 0;
static uint8_t g_tactile_r = 0, g_tactile_g = 0, g_tactile_b = 0;

volatile int g_system_volume_pct = 50; /* 0..100% */

void display_set_volume(int pct) {
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	g_system_volume_pct = pct;
	snprintf(g_osd_text, sizeof(g_osd_text), "VOL: %d%%", g_system_volume_pct);
	g_osd_color = HIFI_CYAN;
	g_osd_expire_tick = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
}

int display_get_volume(void) {
	return g_system_volume_pct;
}

int display_get_elapsed(void) {
	return (int)displayer.elapsed;
}

int display_get_duration(void) {
	return (int)displayer.duration.value;
}

bool display_button_hook(int gpio, actrls_action_e action, button_event_e event, button_press_e press, bool long_press) {
	if (event != BUTTON_PRESSED) return false;
	
	TickType_t now = xTaskGetTickCount();
	g_last_user_activity = now;
	ESP_LOGI(TAG, "BUTTON EVENT: GPIO=%d, action=%d, long=%d", gpio, action, (int)long_press);
	
	/* 1. Wake screen if it was sleeping or dimmed */
	if (g_display_sleep_forced) {
		g_display_sleep_forced = false;
		if (display) {
			GDS_DisplayOn(display);
			GDS_SetContrast(display, g_backlight_level);
		}
		snprintf(g_osd_text, sizeof(g_osd_text), "WAKE");
		g_osd_color = HIFI_EMERALD;
		g_osd_expire_tick = now + pdMS_TO_TICKS(1200);
		if (led_display) led_vu_color_all(0, 100, 40);
		return true; /* Woke up device, don't execute action on wake button press */
	}
	
	/* 2. Process according to User's Standardized Layout: */
	switch (gpio) {
	case 13: /* K1 - Prev Track (Click) / Volume Down (Hold) */
		if (long_press) {
			/* Vol Down (Continuous Hold Auto-Repeat) */
			if (g_system_volume_pct > 0) {
				if (g_system_volume_pct >= 5) g_system_volume_pct -= 5;
				else g_system_volume_pct = 0;
			}
			snprintf(g_osd_text, sizeof(g_osd_text), "VOL: %d%%", g_system_volume_pct);
			g_osd_color = HIFI_CYAN;
			g_osd_expire_tick = now + pdMS_TO_TICKS(1500);
			g_tactile_r = 0; g_tactile_g = 60; g_tactile_b = 100;
			g_tactile_led_expire = now + pdMS_TO_TICKS(140);
			if (led_display) led_vu_color_all(g_tactile_r, g_tactile_g, g_tactile_b);
		} else {
			/* Prev */
			snprintf(g_osd_text, sizeof(g_osd_text), "|<< PREV");
			g_osd_color = HIFI_CYAN;
			g_osd_expire_tick = now + pdMS_TO_TICKS(1000);
			g_tactile_r = 0; g_tactile_g = 40; g_tactile_b = 120;
			g_tactile_led_expire = now + pdMS_TO_TICKS(180);
			if (led_display) led_vu_color_all(g_tactile_r, g_tactile_g, g_tactile_b);
		}
		return false; /* Pass to audio_controls to trigger ACTRLS_PREV / ACTRLS_VOLDOWN */
		
	case 14: /* K2 - Next Track (Click) / Volume Up (Hold) */
		if (long_press) {
			/* Vol Up (Continuous Hold Auto-Repeat) */
			if (g_system_volume_pct < 100) {
				if (g_system_volume_pct <= 95) g_system_volume_pct += 5;
				else g_system_volume_pct = 100;
			}
			snprintf(g_osd_text, sizeof(g_osd_text), "VOL: %d%%", g_system_volume_pct);
			g_osd_color = HIFI_VIOLET;
			g_osd_expire_tick = now + pdMS_TO_TICKS(1500);
			g_tactile_r = 80; g_tactile_g = 0; g_tactile_b = 120;
			g_tactile_led_expire = now + pdMS_TO_TICKS(140);
			if (led_display) led_vu_color_all(g_tactile_r, g_tactile_g, g_tactile_b);
		} else {
			/* Next */
			snprintf(g_osd_text, sizeof(g_osd_text), "NEXT >>|");
			g_osd_color = HIFI_VIOLET;
			g_osd_expire_tick = now + pdMS_TO_TICKS(1000);
			g_tactile_r = 100; g_tactile_g = 0; g_tactile_b = 80;
			g_tactile_led_expire = now + pdMS_TO_TICKS(180);
			if (led_display) led_vu_color_all(g_tactile_r, g_tactile_g, g_tactile_b);
		}
		return false; /* Pass to audio_controls to trigger ACTRLS_NEXT / ACTRLS_VOLUP */
		
	case 15: /* K3 - Play/Pause (Click) / Mute/Unmute (Hold) */
		if (long_press) {
			static TickType_t s_last_k3_hold = 0;
			if (now - s_last_k3_hold < pdMS_TO_TICKS(600)) return true; /* Throttle repeat */
			s_last_k3_hold = now;

			static bool s_is_muted = false;
			static int s_pre_mute_vol = 50;

			s_is_muted = !s_is_muted;
			if (s_is_muted) {
				s_pre_mute_vol = (g_system_volume_pct > 0) ? g_system_volume_pct : 50;
				g_system_volume_pct = 0;
				display_set_volume(0);
				snprintf(g_osd_text, sizeof(g_osd_text), "[X] MUTED");
				g_osd_color = HIFI_ROSE;
				g_tactile_r = 150; g_tactile_g = 0; g_tactile_b = 0;
			} else {
				g_system_volume_pct = s_pre_mute_vol;
				display_set_volume(s_pre_mute_vol);
				snprintf(g_osd_text, sizeof(g_osd_text), "[>] UNMUTE");
				g_osd_color = HIFI_EMERALD;
				g_tactile_r = 0; g_tactile_g = 150; g_tactile_b = 30;
			}
			g_osd_expire_tick = now + pdMS_TO_TICKS(1500);
			g_tactile_led_expire = now + pdMS_TO_TICKS(250);
			if (led_display) led_vu_color_all(g_tactile_r, g_tactile_g, g_tactile_b);
			return false; /* Pass to audio_controls to trigger ACTRLS_REW (mutetoggle) */
		} else {
			/* Play / Pause Toggle */
			bool is_currently_playing = (displayer.state == DISPLAYER_ACTIVE && (displayer.timer || g_audio_peak_level > 1));
			if (is_currently_playing) {
				snprintf(g_osd_text, sizeof(g_osd_text), "|| PAUSE");
				g_osd_color = HIFI_GOLD;
			} else {
				snprintf(g_osd_text, sizeof(g_osd_text), "> PLAY");
				g_osd_color = HIFI_EMERALD;
			}
			g_osd_expire_tick = now + pdMS_TO_TICKS(1200);
			g_tactile_r = 0; g_tactile_g = 120; g_tactile_b = 30;
			g_tactile_led_expire = now + pdMS_TO_TICKS(180);
			if (led_display) led_vu_color_all(g_tactile_r, g_tactile_g, g_tactile_b);
		}
		return false; /* Pass to audio_controls to trigger ACTRLS_TOGGLE */
		
	case 16: /* K4 - Brightness (Click) / Sleep (Hold) */
		if (long_press) {
			static TickType_t s_last_k4_hold = 0;
			if (now - s_last_k4_hold < pdMS_TO_TICKS(600)) return true; /* Throttle repeat */
			s_last_k4_hold = now;

			g_display_sleep_forced = true;
			snprintf(g_osd_text, sizeof(g_osd_text), "SLEEP");
			g_osd_color = HIFI_ROSE;
			g_osd_expire_tick = now + pdMS_TO_TICKS(1200);
			if (led_display) led_vu_color_all(80, 0, 0);
			if (display) {
				GDS_SetContrast(display, 0);
				GDS_DisplayOff(display);
			}
			return true; /* Sleep activated */
		} else {
			/* Single click: Cycle Brightness 100% -> 60% -> 30% -> 100% (NEVER black) */
			if (g_backlight_level == 255) g_backlight_level = 160;      /* 60% */
			else if (g_backlight_level == 160) g_backlight_level = 80;  /* 30% */
			else g_backlight_level = 255;                               /* 100% */
			
			if (display) {
				GDS_DisplayOn(display);
				GDS_SetContrast(display, g_backlight_level);
			}
			
			const char *bl_str = (g_backlight_level == 255) ? "LCD: 100%" :
			                     (g_backlight_level == 160) ? "LCD: 60%" : "LCD: 30%";
			snprintf(g_osd_text, sizeof(g_osd_text), "%s", bl_str);
			g_osd_color = HIFI_GOLD;
			g_osd_expire_tick = now + pdMS_TO_TICKS(1200);
			g_tactile_r = 100; g_tactile_g = 70; g_tactile_b = 0;
			g_tactile_led_expire = now + pdMS_TO_TICKS(180);
			if (led_display) led_vu_color_all(g_tactile_r, g_tactile_g, g_tactile_b);
			return true; /* Handled completely by display! */
		}
	}
	
	return false;
}

static const char *known_drivers[] = {"SH1106",
        "SH1122",
		"SSD1306",
		"SSD1322",
		"SSD1326",
		"SSD1327",
		"SSD1675",
		"SSD1351",
		"ST7735",
		"ST7789",
		"ILI9341",
		NULL
	};
    
static void displayer_task(void *args);
static void display_sleep(void);
static void hifi_draw_album_box(const uint8_t *jpeg_data);

struct GDS_Device *display;   
extern GDS_DetectFunc SSD1306_Detect, SSD132x_Detect, SH1106_Detect, SH1122_Detect, SSD1675_Detect, SSD1322_Detect, SSD1351_Detect, ST77xx_Detect, ILI9341_Detect;
GDS_DetectFunc *drivers[] = { SH1106_Detect, SH1122_Detect, SSD1306_Detect, SSD132x_Detect, SSD1675_Detect, SSD1322_Detect, SSD1351_Detect, ST77xx_Detect, ILI9341_Detect, NULL };

/****************************************************************************************
 * Draw a smooth rounded-corner glass card (radius = 3px) with subtle top specular rim
 */
static void hifi_fill_round_rect(int x, int y, int w, int h, uint16_t fill_col, uint16_t rim_col) {
	if (!display || w < 6 || h < 6) return;
	for (int row = 0; row < h; row++) {
		int inset = 0;
		if (row == 0 || row == h - 1) inset = 3;
		else if (row == 1 || row == h - 2) inset = 1;
		else if (row == 2 || row == h - 3) inset = 1;
		uint16_t col = (row == 0 && rim_col) ? rim_col : fill_col;
		GDS_DrawHLine(display, x + inset, y + row, w - inset * 2, col);
	}
}

/****************************************************************************************
 * Draw a true circular Hi-Fi Vinyl Record with rotating light reflection & micro-grooves
 */
static void hifi_draw_vinyl_disc(int cx, int cy, int anim_tick, bool spinning) {
	int phase = spinning ? (anim_tick & 7) : 1;
	for (int dy = -22; dy <= 22; dy++) {
		for (int dx = -22; dx <= 22; dx++) {
			int d2 = dx * dx + dy * dy;
			if (d2 > 484) continue;
			uint16_t col = 0x0862; /* Deep graphite vinyl body */
			if (d2 >= 435) {
				col = HIFI_PILL;   /* Smooth outer metallic rim */
			} else if (d2 > 72) {
				/* Rotating diagonal light sheen across vinyl grooves */
				bool sheen = false;
				switch (phase & 3) {
					case 0: sheen = (dx * dy > 16); break;
					case 1: sheen = ((dx + dy) * (dx - dy) > 28); break;
					case 2: sheen = (dx * dy < -16); break;
					case 3: sheen = ((dx - dy) * (dx + dy) > 28); break;
				}
				if (d2 >= 265 && d2 <= 285) {
					col = sheen ? HIFI_CYAN : HIFI_TRACK;
				} else if (d2 >= 155 && d2 <= 172) {
					col = sheen ? HIFI_VIOLET : HIFI_TRACK;
				} else if (sheen) {
					col = HIFI_CARD;
				}
			} else if (d2 >= 52) {
				col = HIFI_CYAN;   /* Outer label ring */
			} else if (d2 >= 9) {
				col = (dx + dy > 0) ? HIFI_VIOLET : HIFI_ROSE; /* Vibrant dual-tone label */
			} else {
				col = HIFI_BG;     /* Center spindle hole */
			}
			GDS_DrawPixelFast(display, cx + dx, cy + dy, col);
		}
	}
}

/****************************************************************************************
 * Render Album Cover JPEG or Hi-Fi Vinyl Graphic into Right Deck Card (X:102..155, Y:49..107)
 */
static void hifi_draw_album_box(const uint8_t *jpeg_data) {
	if (!display || GDS_GetDepth(display) != 16) return;

	bool is_portrait = (GDS_GetWidth(display) < GDS_GetHeight(display));

	if (is_portrait) {
		/* Centered Album Deck in Portrait: X: 36..91, Y: 20..75 (W:56, H:56, center 64, 48) */
		GDS_ClearWindow(display, 36, 20, 91, 75, HIFI_BG);
		hifi_fill_round_rect(36, 20, 56, 56, HIFI_CARD, HIFI_PILL);

		if (jpeg_data && jpeg_data != default_artwork) {
			g_has_custom_artwork = true;
			uint16_t orig_h = display->Height;
			display->Height = 73;
			GDS_DrawJPEG(display, (uint8_t*)jpeg_data, 38, 22, GDS_IMAGE_CENTER | GDS_IMAGE_FIT);
			display->Height = orig_h;
		} else {
			g_has_custom_artwork = false;
			hifi_draw_vinyl_disc(64, 48, 0, false);
		}
	} else {
		/* Right Deck Box in Landscape: X: 102..155, Y: 49..107 */
		GDS_ClearWindow(display, 102, 49, 159, 107, HIFI_BG);
		hifi_fill_round_rect(102, 49, 54, 59, HIFI_CARD, HIFI_PILL);

		if (jpeg_data && jpeg_data != default_artwork) {
			g_has_custom_artwork = true;
			uint16_t orig_h = display->Height;
			display->Height = 105;
			GDS_DrawJPEG(display, (uint8_t*)jpeg_data, 104, 51, GDS_IMAGE_CENTER | GDS_IMAGE_FIT);
			display->Height = orig_h;
			GDS_ClearWindow(display, 156, 49, 159, 107, HIFI_BG);
		} else {
			g_has_custom_artwork = false;
			hifi_draw_vinyl_disc(129, 78, 0, false);
		}
	}
}
void display_init(char *welcome) {
	bool init = false;
	char *config = config_alloc_get_str("display_config", NULL, CONFIG_DISPLAY_CONFIG);
	
	int width = -1, height = -1, backlight_pin = -1;
	char *drivername = strstr(config, "driver");

	PARSE_PARAM(config, "width", '=', width);
	PARSE_PARAM(config, "height", '=', height);
	PARSE_PARAM(config, "back", '=', backlight_pin);
		
	// query drivers to see if we have a match
	ESP_LOGI(TAG, "Trying to configure display with %s", config);
	if (backlight_pin >= 0) {
		struct GDS_BacklightPWM PWMConfig = { .Channel = pwm_system.base_channel++, .Timer = pwm_system.timer, .Max = pwm_system.max, .Init = false	};
		display = GDS_AutoDetect(drivername, drivers, &PWMConfig);
	} else {
		display = GDS_AutoDetect(drivername, drivers, NULL);
	}

	// so far so good
	if (display && width > 0 && height > 0) {
		int RST_pin = -1;
		PARSE_PARAM(config, "reset", '=', RST_pin);
		
		// Detect driver interface
		if (strcasestr(config, "I2C") && i2c_system_port != -1) {
			int address = 0x3C;
				
			PARSE_PARAM(config, "address", '=', address);
				
			init = true;
			GDS_I2CInit( i2c_system_port, -1, -1, i2c_system_speed ) ;
			GDS_I2CAttachDevice( display, width, height, address, RST_pin, backlight_pin );
		
			ESP_LOGI(TAG, "Display is I2C on port %u", address);
		} else if (strcasestr(config, "SPI") && spi_system_host != -1) {
			int CS_pin = -1, speed = 0, mode = 0;
		
			PARSE_PARAM(config, "cs", '=', CS_pin);
			PARSE_PARAM(config, "speed", '=', speed);
			PARSE_PARAM(config, "mode", '=', mode);
		
			init = true;
			GDS_SPIInit( spi_system_host, spi_system_dc_gpio );
			GDS_SPIAttachDevice( display, width, height, CS_pin, RST_pin, backlight_pin, speed, mode );
				
			ESP_LOGI(TAG, "Display is SPI host %u with cs:%d", spi_system_host, CS_pin);
		} else {
			display = NULL;
			ESP_LOGI(TAG, "Unsupported display interface or serial link not configured");
		}
	} else {
		display = NULL;
		ESP_LOGW(TAG, "No display driver");
	}	
	
	if (init) {
		static DRAM_ATTR StaticTask_t xTaskBuffer __attribute__ ((aligned (4)));
		static DRAM_ATTR StackType_t xStack[DISPLAYER_STACK_SIZE] __attribute__ ((aligned (4)));
		struct GDS_Layout Layout = {
			.HFlip = strcasestr(config, "HFlip"), 
			.VFlip = strcasestr(config, "VFlip"), 
			.Rotate = strcasestr(config, "rotate"), 
			.Invert = strcasestr(config, "invert"),
			.ColorSwap = strcasestr(config, "cswap"),
		};	

		GDS_SetLayout(display, &Layout);
		GDS_SetFont(display, &Font_line_2);
		GDS_TextPos(display, GDS_FONT_DEFAULT, GDS_TEXT_CENTERED, GDS_TEXT_CLEAR | GDS_TEXT_UPDATE, welcome);

		// start the task that will handle scrolling & counting
		displayer.mutex = xSemaphoreCreateMutex();
		displayer.by = 2;
		displayer.pause = 0;
		displayer.speed = 33;
		strcpy(displayer.header, "SQUEEZELITE S3");
		strcpy(displayer.string, welcome ? welcome : "SQUEEZELITE ESP32-S3");

		// set lines for "fixed" text mode
		GDS_TextSetFontAuto(display, 1, GDS_FONT_LINE_1, height >= 64 ? 2 : -3);
		GDS_TextSetFontAuto(display, 2, GDS_FONT_LINE_2, height >= 64 ? 2 : -3);
		
		displayer.metadata_config = config_alloc_get(NVS_TYPE_STR, "metadata_config");
		
		// leave room for artwork if display is horizontal-style
		displayer.artwork.enable = true;
		displayer.artwork.fit = true;
		if (displayer.metadata_config && strcasestr(displayer.metadata_config, "artwork")) {
			if (height <= 64 && width > height * 2) displayer.artwork.offset = width - height - ARTWORK_BORDER;
			PARSE_PARAM(displayer.metadata_config, "artwork", ':', displayer.artwork.fit);
		}	

		displayer.task = xTaskCreateStatic( (TaskFunction_t) displayer_task, "common_displayer", DISPLAYER_STACK_SIZE, NULL, ESP_TASK_PRIO_MIN + 1, xStack, &xTaskBuffer);
        
        // and finally register ourselves to power off upon deep sleep
        services_sleep_setsuspend(display_sleep);
	}
	
	free(config);
}

/****************************************************************************************
 * 
 */
static void display_sleep(void) {
    GDS_DisplayOff(display);
}

/****************************************************************************************
 * Professional Hi-Fi Audio Streamer UI & Continuous Smooth Marquee Task
 */
static void displayer_task(void *args) {
	vTaskDelay(pdMS_TO_TICKS(400));
	if (is_recovery_running) {
		vTaskSuspend(NULL);
	}
		/* =========================================================================
	 * DEDICATED 20 FPS SLEEK DARK OLED DASHBOARD FOR 128x160 PORTRAIT SCREENS
	 * ========================================================================= */
	if (GDS_GetDepth(display) == 16 && GDS_GetWidth(display) < GDS_GetHeight(display)) {
		int marquee_x = 0;
		int anim_tick = 0;
		int bar_segs[14]  = {2, 3, 5, 4, 6, 7, 5, 4, 6, 5, 4, 3, 2, 2};
		int bar_peaks[14] = {3, 4, 6, 5, 7, 8, 6, 5, 7, 6, 5, 4, 3, 3};
		char last_track[SCROLLABLE_SIZE + 1] = "";
		bool artwork_initialized = false;
		TickType_t last_active_tick = xTaskGetTickCount();
		int pwr_state = 0;

		GDS_SetTextWidth(display, GDS_GetWidth(display));
		GDS_ClearWindow(display, 0, 0, 127, 159, HIFI_BG);
		hifi_draw_album_box(NULL);
		artwork_initialized = true;

		while (1) {
			char header_buf[HEADER_SIZE + 1];
			char track_buf[SCROLLABLE_SIZE + 64];
			char time_buf[24] = "";
			bool is_active = (displayer.state == DISPLAYER_ACTIVE);
			bool is_playing = is_active && (displayer.timer || g_audio_peak_level > 1);
			uint32_t cur_elapsed = 0, cur_duration = 0;
			TickType_t now_tick = xTaskGetTickCount();

			if (g_last_user_activity > last_active_tick) {
				last_active_tick = g_last_user_activity;
			}

			if (g_display_sleep_forced) {
				if (pwr_state != 2) {
					GDS_SetContrast(display, 0);
					GDS_DisplayOff(display);
					pwr_state = 2;
					if (led_display) led_vu_color_all(0, 0, 0);
				}
				vTaskDelay(pdMS_TO_TICKS(200));
				continue;
			}

			bool user_active = (is_active || g_audio_peak_level > 2 || (now_tick - last_active_tick) < pdMS_TO_TICKS(60000));

			if (user_active) {
				if (pwr_state != 0 && g_backlight_level > 0) {
					if (pwr_state == 2) GDS_DisplayOn(display);
					GDS_SetContrast(display, g_backlight_level);
					pwr_state = 0;
				}
			} else {
				if (pwr_state == 0 && g_backlight_level > 0) {
					GDS_SetContrast(display, min(g_backlight_level, (uint8_t)140));
					pwr_state = 1;
				}
			}

			xSemaphoreTake(displayer.mutex, portMAX_DELAY);

			if (is_playing) {
				TickType_t now = xTaskGetTickCount();
				uint32_t dt_ms = (now - displayer.tick) * portTICK_PERIOD_MS;
				if (dt_ms >= 1000) {
					uint32_t inc_sec = dt_ms / 1000;
					if (inc_sec > 5) inc_sec = 1;
					displayer.tick = now;
					displayer.elapsed += inc_sec;
				}
			} else {
				displayer.tick = xTaskGetTickCount();
			}
			if (displayer.duration.value > 0 && displayer.elapsed > displayer.duration.value + 3) {
				displayer.elapsed = displayer.elapsed - displayer.duration.value;
				displayer.duration.value = 0;
				displayer.duration.visible = false;
			}
			cur_elapsed = displayer.elapsed;
			cur_duration = displayer.duration.value;
			if (cur_duration > 0 && cur_elapsed > cur_duration) {
				cur_elapsed = cur_duration;
			}

			if (is_active && displayer.header[0]) {
				strncpy(header_buf, displayer.header, HEADER_SIZE);
				header_buf[HEADER_SIZE] = '\0';
			} else {
				strcpy(header_buf, "HI-FI S3");
			}

			if (is_active) {
				if (displayer.string[0]) {
					snprintf(track_buf, sizeof(track_buf), "%s     -     ", displayer.string);
				} else {
					strcpy(track_buf, "AirPlay Lossless  -  PCM5102A DAC     -     ");
				}
			} else {
				if (displayer.string[0] && strcasecmp(displayer.string, "SqueezeESP32") != 0) {
					snprintf(track_buf, sizeof(track_buf), "%s     -     ", displayer.string);
				} else {
					strcpy(track_buf, "Squeezelite S3  -  Hi-Fi Ready     -     ");
				}
			}

			if (strcmp(last_track, track_buf) != 0) {
				strncpy(last_track, track_buf, SCROLLABLE_SIZE);
				last_track[SCROLLABLE_SIZE] = '\0';
				marquee_x = 0;
			}

			if (is_active && !displayer.artwork.updated && !artwork_initialized) {
				hifi_draw_album_box(NULL);
				artwork_initialized = true;
			}

			xSemaphoreGive(displayer.mutex);

			/* Clear background around cards */
			GDS_ClearWindow(display, 0, 0, 127, 19, HIFI_BG);
			GDS_ClearWindow(display, 0, 20, 35, 76, HIFI_BG);
			GDS_ClearWindow(display, 92, 20, 127, 76, HIFI_BG);
			GDS_ClearWindow(display, 0, 77, 127, 78, HIFI_BG);
			GDS_ClearWindow(display, 0, 104, 127, 105, HIFI_BG);
			GDS_ClearWindow(display, 0, 124, 127, 125, HIFI_BG);
			GDS_ClearWindow(display, 0, 158, 127, 159, HIFI_BG);

			/* -------------------------------------------------------------
			 * ZONE 1: HEADER PILL (X:4..123, Y:2..17)
			 * ------------------------------------------------------------- */
			hifi_fill_round_rect(4, 2, 120, 16, HIFI_CARD, HIFI_PILL);
			uint16_t dot_col = is_active ? ((anim_tick & 8) ? HIFI_EMERALD : HIFI_CYAN) : HIFI_CYAN;
			GDS_DrawBox(display, 10, 8, 13, 11, dot_col, true);

			GDS_SetFont(display, &Font_droid_sans_fallback_11x13);
			GDS_SetTextWidth(display, 64);
			GDS_FontDrawString(display, 17, 4, header_buf, HIFI_WHITE);
			GDS_SetTextWidth(display, 128);

			if (is_active && (is_playing || cur_elapsed > 0)) {
				snprintf(time_buf, sizeof(time_buf), "%u:%02u", (unsigned)(cur_elapsed / 60), (unsigned)(cur_elapsed % 60));
				GDS_SetFont(display, &Font_droid_sans_mono_7x13);
				int tw = GDS_FontMeasureString(display, time_buf);
				GDS_FontDrawString(display, max(75, 118 - tw), 4, time_buf, HIFI_CYAN);
			} else {
				GDS_SetFont(display, &Font_droid_sans_mono_7x13);
				GDS_FontDrawString(display, 80, 4, "32-BIT", HIFI_GOLD);
			}

			/* -------------------------------------------------------------
			 * ZONE 2: VINYL RECORD (X:36..91, Y:20..76)
			 * ------------------------------------------------------------- */
			uint8_t peak = g_audio_peak_level;
			if (!g_has_custom_artwork) {
				hifi_draw_vinyl_disc(64, 48, anim_tick, is_active && (is_playing || peak > 2));
			}

			/* -------------------------------------------------------------
			 * ZONE 3: TRACK MARQUEE / OSD (X:4..123, Y:79..103)
			 * ------------------------------------------------------------- */
			bool show_osd = (xTaskGetTickCount() < g_osd_expire_tick && g_osd_text[0] != '\0');
			if (show_osd) {
				hifi_fill_round_rect(4, 79, 120, 25, HIFI_CARD, g_osd_color);
				GDS_SetFont(display, &Font_droid_sans_fallback_11x13);
				int otw = GDS_FontMeasureString(display, g_osd_text);
				int ox = max(8, 64 - otw / 2);
				int oy = (strncmp(g_osd_text, "VOL", 3) == 0) ? 81 : 85;
				GDS_FontDrawString(display, ox, oy, g_osd_text, g_osd_color);

				if (strncmp(g_osd_text, "VOL", 3) == 0) {
					int bar_x = 14;
					int bar_w = 100;
					int fill_w = (bar_w * g_system_volume_pct) / 100;
					if (fill_w > bar_w) fill_w = bar_w;
					GDS_DrawHLine(display, bar_x, 96, bar_w, HIFI_TRACK);
					GDS_DrawHLine(display, bar_x, 97, bar_w, HIFI_TRACK);
					if (fill_w > 0) {
						GDS_DrawHLine(display, bar_x, 96, fill_w, g_osd_color);
						GDS_DrawHLine(display, bar_x, 97, fill_w, g_osd_color);
					}
				}
			} else {
				hifi_fill_round_rect(4, 79, 120, 25, HIFI_CARD, HIFI_PILL);
				GDS_SetFont(display, &Font_droid_sans_fallback_11x13);
				int text_w = GDS_FontMeasureString(display, track_buf);
				if (text_w < 112) text_w = 112;

				int text_color = is_active ? HIFI_WHITE : HIFI_CYAN;
				GDS_SetTextWidth(display, 119);
				GDS_FontDrawString(display, 8 - marquee_x, 85, track_buf, text_color);
				if (8 + text_w - marquee_x < 119) {
					GDS_FontDrawString(display, 8 + text_w - marquee_x, 85, track_buf, text_color);
				}
				GDS_SetTextWidth(display, 128);

				/* Clip edges */
				GDS_ClearWindow(display, 0, 79, 3, 103, HIFI_BG);
				GDS_ClearWindow(display, 124, 79, 127, 103, HIFI_BG);
				marquee_x = (marquee_x + 2) % text_w;
			}

			/* -------------------------------------------------------------
			 * ZONE 4: SCRUBBER TIMELINE (X:4..123, Y:106..123)
			 * ------------------------------------------------------------- */
			hifi_fill_round_rect(4, 106, 120, 18, HIFI_CARD, HIFI_PILL);
			if (cur_duration > 0 && cur_duration < 3600) {
				snprintf(time_buf, sizeof(time_buf), "%u:%02u / %u:%02u",
					(unsigned)(cur_elapsed / 60), (unsigned)(cur_elapsed % 60),
					(unsigned)(cur_duration / 60), (unsigned)(cur_duration % 60));
			} else {
				snprintf(time_buf, sizeof(time_buf), "%u:%02u", (unsigned)(cur_elapsed / 60), (unsigned)(cur_elapsed % 60));
			}
			GDS_SetFont(display, &Font_droid_sans_mono_7x13);
			int timew = GDS_FontMeasureString(display, time_buf);
			GDS_FontDrawString(display, max(8, 64 - timew / 2), 108, time_buf, HIFI_CYAN);

			/* Progress bar line at Y: 121 */
			GDS_DrawHLine(display, 10, 121, 108, HIFI_TRACK);
			int fill_w = 0;
			if (is_active && cur_duration > 0) {
				fill_w = (int)((cur_elapsed * 108) / cur_duration);
				if (fill_w > 108) fill_w = 108;
				if (fill_w > 0) GDS_DrawHLine(display, 10, 121, fill_w, HIFI_CYAN);
			}

			/* -------------------------------------------------------------
			 * ZONE 5: 14-BAND EQUALIZER SPECTRUM (X:4..123, Y:126..157)
			 * ------------------------------------------------------------- */
			hifi_fill_round_rect(4, 126, 120, 31, HIFI_CARD, HIFI_PILL);

			/* Micro header */
			uint16_t ico_col = is_active ? HIFI_EMERALD : HIFI_VIOLET;
			GDS_DrawVLine(display, 9, 129, 6, ico_col);
			GDS_DrawVLine(display, 10, 130, 4, ico_col);
			GDS_DrawVLine(display, 11, 131, 2, ico_col);

			GDS_SetFont(display, &Font_droid_sans_mono_7x13);
			GDS_FontDrawString(display, 16, 127, is_active ? "44.1kHz PCM" : "DAC READY", HIFI_SLATE);

			/* 14 spectrum bars */
			for (int i = 0; i < 14; i++) {
				int target = 1;
				if (is_active && peak > 2) {
					int dist = (i < 6) ? (6 - i) : (i - 6);
					int weight = 105 - dist * 9;
					int jitter = ((anim_tick * 5 + i * 17) % 5) - 2;
					target = ((int)peak * weight) / 1000 + jitter;
				} else if (is_playing) {
					target = 2 + ((anim_tick * 2 + i * 7) % 5);
				} else {
					int wave = ((anim_tick + i * 2) % 14);
					if (wave > 7) wave = 14 - wave;
					target = 1 + (wave / 3);
				}

				if (target < 1) target = 1;
				if (target > 7) target = 7;

				if (target > bar_segs[i]) {
					bar_segs[i] = (bar_segs[i] + target * 2) / 3;
					if (bar_segs[i] < target) bar_segs[i]++;
				} else if (bar_segs[i] > target) {
					bar_segs[i]--;
				}

				if (bar_segs[i] >= bar_peaks[i]) {
					bar_peaks[i] = bar_segs[i];
				} else if ((anim_tick & 1) == 0 && bar_peaks[i] > 1) {
					bar_peaks[i]--;
				}

				int bx = 9 + i * 8; /* 14 bars * 8px = 112px */
				for (int s = 0; s < 7; s++) {
					int sy = 153 - s * 3;
					uint16_t seg_col = HIFI_TRACK;
					if (s < bar_segs[i]) {
						if (s < 2) seg_col = HIFI_VIOLET;
						else if (s < 4) seg_col = HIFI_CYAN;
						else if (s < 6) seg_col = HIFI_EMERALD;
						else seg_col = HIFI_ROSE;
					} else if (s == bar_peaks[i] && s > 0) {
						seg_col = HIFI_WHITE;
					}
					GDS_DrawHLine(display, bx, sy, 5, seg_col);
					GDS_DrawHLine(display, bx, sy + 1, 5, seg_col);
				}
			}

			/* Onboard WS2812 LED synchronization */
			if (led_display) {
				if (now_tick < g_tactile_led_expire) {
					led_vu_color_all(g_tactile_r, g_tactile_g, g_tactile_b);
				} else if (is_active && is_playing && peak > 1) {
					int bri = (peak * 3) / 2;
					if (bri > 120) bri = 120;
					if (bri < 15)  bri = 15;
					if (peak > 55) {
						led_vu_color_all(bri, bri / 5, bri / 2);
					} else if (peak > 30) {
						led_vu_color_all(0, bri, bri);
					} else {
						led_vu_color_all(bri / 3, 0, bri);
					}
				} else if (is_active) {
					int wave = (anim_tick % 40);
					if (wave > 20) wave = 40 - wave;
					uint8_t b = 4 + wave / 2;
					led_vu_color_all(0, b / 2, b);
				} else {
					int wave = (anim_tick % 60);
					if (wave > 30) wave = 60 - wave;
					uint8_t b = 3 + wave / 3;
					led_vu_color_all(b, b / 2, 0);
				}
			}

			GDS_Update(display);
			anim_tick++;
			vTaskDelay(pdMS_TO_TICKS(pwr_state == 1 ? 500 : 50));
		}
	} else /* Dedicated 20 FPS Sleek Dark OLED Dashboard for 16-bit color 160x128 ST7735 screens */
	if (GDS_GetDepth(display) == 16 && GDS_GetWidth(display) >= 120 && GDS_GetHeight(display) >= 120) {
		int marquee_x = 0;
		int anim_tick = 0;
		int bar_segs[14]  = {2, 3, 5, 4, 6, 7, 5, 4, 6, 5, 4, 3, 2, 2};
		int bar_peaks[14] = {3, 4, 6, 5, 7, 8, 6, 5, 7, 6, 5, 4, 3, 3};
		char last_track[SCROLLABLE_SIZE + 1] = "";
		bool artwork_initialized = false;
		TickType_t last_active_tick = xTaskGetTickCount();
		int pwr_state = 0; /* 0 = Active (100% BL, 20FPS), 1 = Eco Dim (25% BL, 2FPS), 2 = Standby (Screen Off) */

		GDS_SetTextWidth(display, GDS_GetWidth(display));
		GDS_ClearWindow(display, 0, 0, 159, 127, HIFI_BG);
		hifi_draw_album_box(NULL);
		artwork_initialized = true;

		while (1) {
			char header_buf[HEADER_SIZE + 1];
			char track_buf[SCROLLABLE_SIZE + 64];
			char time_buf[24] = "";
			bool is_active = (displayer.state == DISPLAYER_ACTIVE);
			bool is_playing = is_active && (displayer.timer || g_audio_peak_level > 1);
			uint32_t cur_elapsed = 0, cur_duration = 0;
			TickType_t now_tick = xTaskGetTickCount();

			if (g_last_user_activity > last_active_tick) {
				last_active_tick = g_last_user_activity;
			}

			if (g_display_sleep_forced) {
				if (pwr_state != 2) {
					GDS_SetContrast(display, 0);
					GDS_DisplayOff(display);
					pwr_state = 2;
					if (led_display) led_vu_color_all(0, 0, 0);
				}
				vTaskDelay(pdMS_TO_TICKS(200));
				continue;
			}

			bool user_active = (is_active || g_audio_peak_level > 2 || (now_tick - last_active_tick) < pdMS_TO_TICKS(60000));

			if (user_active) {
				if (pwr_state != 0 && g_backlight_level > 0) {
					if (pwr_state == 2) {
						GDS_DisplayOn(display);
					}
					GDS_SetContrast(display, g_backlight_level);
					pwr_state = 0;
					ESP_LOGI(TAG, "Power Manager: Display Active (Backlight %d, 20 FPS)", (int)g_backlight_level);
				}
			} else {
				/* After 60s idle with no music: gently dim to 40% (Eco Dim) - NEVER turn black */
				if (pwr_state == 0 && g_backlight_level > 0) {
					GDS_SetContrast(display, min(g_backlight_level, (uint8_t)140));
					pwr_state = 1;
					ESP_LOGI(TAG, "Power Manager: Eco Dim Mode (40%% Backlight, 2 FPS)");
				}
			}

			xSemaphoreTake(displayer.mutex, portMAX_DELAY);

			if (is_playing) {
				TickType_t now = xTaskGetTickCount();
				uint32_t dt_ms = (now - displayer.tick) * portTICK_PERIOD_MS;
				if (dt_ms >= 1000) {
					uint32_t inc_sec = dt_ms / 1000;
					if (inc_sec > 5) inc_sec = 1;
					displayer.tick = now;
					displayer.elapsed += inc_sec;
				}
			} else {
				displayer.tick = xTaskGetTickCount();
			}
			if (displayer.duration.value > 0 && displayer.elapsed > displayer.duration.value + 3) {
				displayer.elapsed = displayer.elapsed - displayer.duration.value;
				displayer.duration.value = 0;
				displayer.duration.visible = false;
			}
			cur_elapsed = displayer.elapsed;
			cur_duration = displayer.duration.value;
			if (cur_duration > 0 && cur_elapsed > cur_duration) {
				cur_elapsed = cur_duration;
			}

			if (is_active && displayer.header[0]) {
				strncpy(header_buf, displayer.header, HEADER_SIZE);
				header_buf[HEADER_SIZE] = '\0';
			} else {
				strcpy(header_buf, "HI-FI S3");
			}

			if (is_active) {
				if (displayer.string[0]) {
					snprintf(track_buf, sizeof(track_buf), "%s     -     ", displayer.string);
				} else {
					strcpy(track_buf, "AirPlay Lossless Stream  -  PCM5102A 32-Bit DAC     -     ");
				}
			} else {
				if (displayer.string[0] && strcasecmp(displayer.string, "SqueezeESP32") != 0) {
					snprintf(track_buf, sizeof(track_buf), "%s  -  AirPlay Hi-Fi Ready     -     ", displayer.string);
				} else {
					strcpy(track_buf, "Squeezelite ESP32-S3  -  AirPlay Hi-Fi Streamer     -     ");
				}
			}

			if (strcmp(last_track, track_buf) != 0) {
				strncpy(last_track, track_buf, SCROLLABLE_SIZE);
				last_track[SCROLLABLE_SIZE] = '\0';
				marquee_x = 0;
			}

			if (is_active && !displayer.artwork.updated && !artwork_initialized) {
				hifi_draw_album_box(NULL);
				artwork_initialized = true;
			}

			xSemaphoreGive(displayer.mutex);

			/* Clear all non-album background areas so cards float cleanly on OLED pitch black */
			GDS_ClearWindow(display, 0, 0, 159, 19, HIFI_BG);
			GDS_ClearWindow(display, 0, 20, 3, 48, HIFI_BG);
			GDS_ClearWindow(display, 156, 20, 159, 48, HIFI_BG);
			GDS_ClearWindow(display, 0, 46, 159, 48, HIFI_BG);
			GDS_ClearWindow(display, 0, 49, 101, 107, HIFI_BG);
			GDS_ClearWindow(display, 156, 49, 159, 107, HIFI_BG);
			GDS_ClearWindow(display, 0, 108, 159, 127, HIFI_BG);

			/* =========================================================================
			 * ZONE 1: FLOATING STATUS PILL & TIME / HI-RES BADGE (Y: 2..17)
			 * ========================================================================= */
			hifi_fill_round_rect(4, 2, 76, 16, HIFI_CARD, HIFI_PILL);

			/* Glowing Status Dot (pulsing emerald when active, cyan when ready) */
			uint16_t dot_col = is_active ? ((anim_tick & 8) ? HIFI_EMERALD : HIFI_CYAN) : HIFI_CYAN;
			GDS_DrawBox(display, 10, 8, 13, 11, dot_col, true);

			GDS_SetFont(display, &Font_droid_sans_fallback_11x13);
			GDS_SetTextWidth(display, 78);
			GDS_FontDrawString(display, 18, 4, header_buf, HIFI_WHITE);
			GDS_SetTextWidth(display, 160);

			if (is_active && (is_playing || cur_elapsed > 0)) {
				if (cur_duration > 0 && cur_duration < 3600) {
					snprintf(time_buf, sizeof(time_buf), "%u:%02u / %u:%02u",
						(unsigned)(cur_elapsed / 60), (unsigned)(cur_elapsed % 60),
						(unsigned)(cur_duration / 60), (unsigned)(cur_duration % 60));
				} else if (cur_duration >= 3600) {
					snprintf(time_buf, sizeof(time_buf), "%u:%02u:%02u",
						(unsigned)(cur_elapsed / 3600), (unsigned)((cur_elapsed % 3600) / 60), (unsigned)(cur_elapsed % 60));
				} else if (cur_elapsed >= 3600) {
					snprintf(time_buf, sizeof(time_buf), "LIVE %u:%02u:%02u",
						(unsigned)(cur_elapsed / 3600), (unsigned)((cur_elapsed % 3600) / 60), (unsigned)(cur_elapsed % 60));
				} else {
					snprintf(time_buf, sizeof(time_buf), "LIVE  %u:%02u",
						(unsigned)(cur_elapsed / 60), (unsigned)(cur_elapsed % 60));
				}
				GDS_SetFont(display, &Font_droid_sans_mono_7x13);
				int tw = GDS_FontMeasureString(display, time_buf);
				GDS_FontDrawString(display, max(84, 155 - tw), 4, time_buf, HIFI_CYAN);
			} else {
				hifi_fill_round_rect(108, 2, 48, 16, HIFI_CARD, HIFI_PILL);
				GDS_SetFont(display, &Font_droid_sans_mono_7x13);
				GDS_FontDrawString(display, 114, 4, "32-BIT", HIFI_GOLD);
			}

			/* =========================================================================
			 * ZONE 2: SLEEK ROUNDED GLASS PILL MARQUEE (X: 4..155, Y: 20..45) OR OSD BADGE
			 * ========================================================================= */
			bool show_osd = (xTaskGetTickCount() < g_osd_expire_tick && g_osd_text[0] != '\0');
			if (show_osd) {
				hifi_fill_round_rect(10, 20, 140, 26, HIFI_CARD, g_osd_color);
				GDS_SetFont(display, &Font_droid_sans_fallback_15x17);
				int otw = GDS_FontMeasureString(display, g_osd_text);
				int ox = max(14, 80 - otw / 2);
				int oy = (strncmp(g_osd_text, "VOL", 3) == 0) ? 23 : 25;
				GDS_FontDrawString(display, ox, oy, g_osd_text, g_osd_color);

				/* If volume OSD, draw a sleek high-precision volume meter bar underneath */
				if (strncmp(g_osd_text, "VOL", 3) == 0) {
					int bar_x = 24;
					int bar_w = 112;
					int fill_w = (bar_w * g_system_volume_pct) / 100;
					if (fill_w > bar_w) fill_w = bar_w;
					GDS_DrawHLine(display, bar_x, 41, bar_w, HIFI_TRACK);
					GDS_DrawHLine(display, bar_x, 42, bar_w, HIFI_TRACK);
					if (fill_w > 0) {
						GDS_DrawHLine(display, bar_x, 41, fill_w, g_osd_color);
						GDS_DrawHLine(display, bar_x, 42, fill_w, g_osd_color);
					}
				}
			} else {
				hifi_fill_round_rect(4, 20, 152, 26, HIFI_CARD, HIFI_PILL);

				GDS_SetFont(display, &Font_droid_sans_fallback_15x17);
				int text_w = GDS_FontMeasureString(display, track_buf);
				if (text_w < 144) text_w = 144;

				int text_color = is_active ? HIFI_WHITE : HIFI_CYAN;
				GDS_SetTextWidth(display, 151);
				GDS_FontDrawString(display, 9 - marquee_x, 25, track_buf, text_color);
				if (9 + text_w - marquee_x < 151) {
					GDS_FontDrawString(display, 9 + text_w - marquee_x, 25, track_buf, text_color);
				}
				GDS_SetTextWidth(display, 160);

				/* Restore left rounded pill edge so scrolling text clips cleanly with 5px padding */
				GDS_ClearWindow(display, 0, 20, 3, 45, HIFI_BG);
				for (int row = 0; row < 26; row++) {
					int inset = (row == 0 || row == 25) ? 3 : ((row <= 2 || row >= 23) ? 1 : 0);
					if (inset > 0) GDS_DrawHLine(display, 4, 20 + row, inset, HIFI_BG);
					uint16_t col = (row == 0) ? HIFI_PILL : HIFI_CARD;
					if (5 - inset > 0) GDS_DrawHLine(display, 4 + inset, 20 + row, 5 - inset, col);
				}

				marquee_x = (marquee_x + 2) % text_w;
			}

			/* =========================================================================
			 * ZONE 3 LEFT: 14-BAND SEGMENTED LED SPECTRUM CARD (X: 4..98, Y: 49..107)
			 * ========================================================================= */
			hifi_fill_round_rect(4, 49, 95, 59, HIFI_CARD, HIFI_PILL);

			/* Vector Play/Ready micro-icon + sample rate label */
			uint16_t ico_col = is_active ? HIFI_EMERALD : HIFI_VIOLET;
			GDS_DrawVLine(display, 10, 54, 7, ico_col);
			GDS_DrawVLine(display, 11, 55, 5, ico_col);
			GDS_DrawVLine(display, 12, 56, 3, ico_col);
			GDS_DrawPixelFast(display, 13, 57, ico_col);

			GDS_SetFont(display, &Font_droid_sans_mono_7x13);
			GDS_FontDrawString(display, 18, 52, is_active ? "44.1kHz PCM" : "DAC READY", HIFI_SLATE);

			uint8_t peak = g_audio_peak_level;
			for (int i = 0; i < 14; i++) {
				int target = 1;
				if (is_active && peak > 2) {
					/* Bell-curve frequency weighting across 14 bands + live organic motion */
					int dist = (i < 6) ? (6 - i) : (i - 6);
					int weight = 105 - dist * 9;
					int jitter = ((anim_tick * 5 + i * 17) % 5) - 2;
					target = ((int)peak * weight) / 850 + jitter;
				} else if (is_playing) {
					target = 2 + ((anim_tick * 2 + i * 7) % 6);
				} else {
					/* Gentle breathing wave in standby */
					int wave = ((anim_tick + i * 2) % 14);
					if (wave > 7) wave = 14 - wave;
					target = 1 + (wave / 3);
				}

				if (target < 1) target = 1;
				if (target > 12) target = 12;

				if (target > bar_segs[i]) {
					bar_segs[i] = (bar_segs[i] + target * 2) / 3;
					if (bar_segs[i] < target) bar_segs[i]++;
				} else if (bar_segs[i] > target) {
					bar_segs[i]--;
				}

				if (bar_segs[i] >= bar_peaks[i]) {
					bar_peaks[i] = bar_segs[i];
				} else if ((anim_tick & 1) == 0 && bar_peaks[i] > 1) {
					bar_peaks[i]--;
				}

				int bx = 9 + i * 6;
				for (int s = 0; s < 12; s++) {
					int sy = 102 - s * 3;
					uint16_t seg_col = HIFI_TRACK; /* Subtle unlit LED matrix segment */
					if (s < bar_segs[i]) {
						if (s < 4) seg_col = HIFI_VIOLET;
						else if (s < 8) seg_col = HIFI_CYAN;
						else if (s < 10) seg_col = HIFI_EMERALD;
						else seg_col = HIFI_ROSE;
					} else if (s == bar_peaks[i] && s > 0) {
						seg_col = HIFI_WHITE; /* Crisp floating peak-hold LED */
					}
					GDS_DrawHLine(display, bx, sy, 4, seg_col);
					GDS_DrawHLine(display, bx, sy + 1, 4, seg_col);
				}
			}

			/* =========================================================================
			 * ZONE 3 RIGHT: SPINNING VINYL DISC WHEN NO CUSTOM JPEG ARTWORK
			 * ========================================================================= */
			if (!g_has_custom_artwork) {
				hifi_draw_vinyl_disc(129, 78, anim_tick, is_active && (is_playing || peak > 2));
			}

			/* =========================================================================
			 * ZONE 4: SLIM SPOTIFY/APPLE-MUSIC SCRUBBER TIMELINE (Y: 114..122)
			 * ========================================================================= */
			GDS_DrawHLine(display, 9, 117, 142, HIFI_TRACK);
			GDS_DrawHLine(display, 8, 118, 144, HIFI_TRACK);
			GDS_DrawHLine(display, 9, 119, 142, HIFI_TRACK);

			int knob_x = 12;
			if (is_active && cur_duration > 0) {
				int fill_w = (int)((cur_elapsed * 140) / cur_duration);
				if (fill_w > 140) fill_w = 140;
				if (fill_w > 2) {
					GDS_DrawHLine(display, 9, 117, fill_w, HIFI_VIOLET);
					GDS_DrawHLine(display, 8, 118, fill_w + 1, HIFI_CYAN);
					GDS_DrawHLine(display, 9, 119, fill_w, HIFI_CYAN);
				}
				knob_x = 9 + fill_w;
			} else {
				/* Smooth ping-pong glowing comet along the slim track */
				int cycle = (anim_tick * 3) % 260;
				int pos = (cycle <= 130) ? cycle : (260 - cycle);
				int trail = 22;
				int start_x = max(9, 9 + pos - trail / 2);
				int end_x   = min(149, 9 + pos + trail / 2);
				if (end_x > start_x) {
					GDS_DrawHLine(display, start_x, 117, end_x - start_x, HIFI_VIOLET);
					GDS_DrawHLine(display, start_x, 118, end_x - start_x, is_active ? HIFI_CYAN : HIFI_VIOLET);
					GDS_DrawHLine(display, start_x, 119, end_x - start_x, HIFI_VIOLET);
				}
				knob_x = 9 + pos;
			}

			/* Glowing circular playhead knob */
			GDS_DrawHLine(display, knob_x - 1, 116, 3, HIFI_CYAN);
			GDS_DrawHLine(display, knob_x - 2, 117, 5, HIFI_WHITE);
			GDS_DrawHLine(display, knob_x - 2, 118, 5, HIFI_WHITE);
			GDS_DrawHLine(display, knob_x - 2, 119, 5, HIFI_WHITE);
			GDS_DrawHLine(display, knob_x - 1, 120, 3, HIFI_CYAN);

			/* =========================================================================
			 * ZONE 5: ONBOARD WS2812 RGB LED (GPIO 21) DANCING TO LIVE AIRPLAY AUDIO
			 * ========================================================================= */
			if (led_display) {
				if (pwr_state == 2) {
					led_vu_color_all(0, 0, 0);
				} else if (is_active && is_playing && peak > 2) {
					/* Dynamic beat pulse: Coral Red on heavy bass, Emerald on mids, Violet on highs */
					uint8_t bri = (peak > 80) ? 75 : (uint8_t)((peak * 60) / 80 + 15);
					if (peak > 55) {
						led_vu_color_all(bri, 0, bri / 4);
					} else if (peak > 25) {
						led_vu_color_all(0, bri, bri / 3);
					} else {
						led_vu_color_all(bri / 3, 0, bri);
					}
				} else if (is_active) {
					/* Connected / Paused: Gentle breathing cyan */
					int wave = (anim_tick % 40);
					if (wave > 20) wave = 40 - wave;
					uint8_t b = 4 + wave / 2;
					led_vu_color_all(0, b / 2, b);
				} else {
					/* Ready / Standby: Warm subtle amber pulse */
					int wave = (anim_tick % 60);
					if (wave > 30) wave = 60 - wave;
					uint8_t b = 3 + wave / 3;
					led_vu_color_all(b, b / 2, 0);
				}
			}

			GDS_Update(display);
			anim_tick++;
			vTaskDelay(pdMS_TO_TICKS(pwr_state == 1 ? 500 : 50));
		}
	}

	/* Legacy monochrome/grayscale display loop */
	int scroll_sleep = 0, timer_sleep;
		
	while (1) {
		if (displayer.state < DISPLAYER_ACTIVE) {
			if (displayer.state == DISPLAYER_IDLE) GDS_TextLine(display, 2, 0, GDS_TEXT_CLEAR | GDS_TEXT_UPDATE, displayer.string);
			vTaskSuspend(NULL);
			scroll_sleep = 0;
			GDS_ClearExt(display, true);
			GDS_TextLine(display, 1, GDS_TEXT_LEFT, GDS_TEXT_UPDATE, displayer.header);
		} else if (displayer.refresh) {
			GDS_TextLine(display, 1, GDS_TEXT_LEFT, GDS_TEXT_CLEAR | GDS_TEXT_UPDATE, displayer.header);	
			scroll_sleep = 0;
			displayer.refresh = false;			
		}
		
		if (scroll_sleep <= 10) {
			if (*displayer.string && displayer.state == DISPLAYER_ACTIVE) {
				xSemaphoreTake(displayer.mutex, portMAX_DELAY);
				int offset = -displayer.offset;
				char *string = strdup(displayer.string);
				scroll_sleep = displayer.offset ? displayer.speed : displayer.pause;
				displayer.offset = displayer.offset >= displayer.boundary ? 0 : (displayer.offset + min(displayer.by, displayer.boundary - displayer.offset));			
				xSemaphoreGive(displayer.mutex);				
				GDS_TextLine(display, 2, offset, GDS_TEXT_CLEAR | GDS_TEXT_UPDATE, string);
				free(string);
			} else {
				scroll_sleep = DEFAULT_SLEEP;
			}	
		}	
		
		if (displayer.timer && displayer.state == DISPLAYER_ACTIVE) {
			char line[19] = "-", *_line = line + 1;
			TickType_t tick = xTaskGetTickCount();
			uint32_t elapsed = (tick - displayer.tick) * portTICK_PERIOD_MS;

			if (elapsed >= 1000) {
				xSemaphoreTake(displayer.mutex, portMAX_DELAY);
				displayer.tick = tick;
				elapsed = displayer.elapsed += elapsed / 1000;
				xSemaphoreGive(displayer.mutex);

				if (displayer.duration.value && !displayer.duration.visible) elapsed = displayer.duration.value - elapsed;

				if (elapsed < 3600) sprintf(_line, "%u:%02u", elapsed / 60, elapsed % 60);
				else sprintf(_line, "%u:%02u:%02u", (elapsed / 3600) % 100, (elapsed % 3600) / 60, elapsed % 60);

				if (displayer.duration.visible) {
					strcat(_line, "/");
					strcat(_line, displayer.duration.string);
				} else if (displayer.duration.value) {
					_line--;
				}

				GDS_TextLine(display, 1, GDS_TEXT_LEFT, GDS_TEXT_CLEAR, displayer.header);	
				GDS_TextLine(display, 1, GDS_TEXT_RIGHT, GDS_TEXT_UPDATE, _line);
				
				if (displayer.artwork.active && !displayer.artwork.updated && tick - displayer.artwork.tick > pdMS_TO_TICKS(5000)) {
					displayer_artwork((uint8_t*) default_artwork);
				}	
				timer_sleep = 1000;
			} else timer_sleep = max(1000 - elapsed, 0);	
		} else timer_sleep = DEFAULT_SLEEP;
		
		int sleep = min(scroll_sleep, timer_sleep);
		scroll_sleep -= sleep;
		vTaskDelay(sleep / portTICK_PERIOD_MS);
	}
}	

/****************************************************************************************
 * 
 */
static void displayer_artwork_unlocked(uint8_t *data) {
	if (!display) return;

	if (GDS_GetDepth(display) == 16 && GDS_GetWidth(display) >= 120 && GDS_GetHeight(display) >= 120) {
		if (data) {
			displayer.artwork.updated = true;
			hifi_draw_album_box(data);
		} else {
			displayer.artwork.updated = false;
			displayer.artwork.tick = xTaskGetTickCount();
			hifi_draw_album_box(default_artwork);
		}
		return;
	}

	if (!displayer.artwork.active) return;
	
	int x = displayer.artwork.offset ? displayer.artwork.offset + ARTWORK_BORDER : 0;
	int y = x ? 0 : (GDS_GetHeight(display) >= 128 ? 44 : 32);
	GDS_ClearWindow(display, x, y, -1, -1, GDS_COLOR_BLACK);
	if (data) {
		displayer.artwork.updated = true;
		GDS_DrawJPEG(display, data, x, y, GDS_IMAGE_CENTER | (displayer.artwork.fit ? GDS_IMAGE_FIT : 0));
		GDS_Update(display);
	} else {
		displayer.artwork.updated = false;
		displayer.artwork.tick = xTaskGetTickCount();
	}	
}

void displayer_artwork(uint8_t *data) {
	if (!display) return;
	xSemaphoreTake(displayer.mutex, portMAX_DELAY);
	displayer_artwork_unlocked(data);
	xSemaphoreGive(displayer.mutex);
}

/****************************************************************************************
 * 
 */
bool displayer_can_artwork(void) {
	return displayer.artwork.active || (display && GDS_GetDepth(display) == 16);
}

/****************************************************************************************
 * Sanitize decoded string so all characters are printable 7-bit ASCII (32..126)
 */
static void sanitize_ascii(char *s) {
	for (; s && *s; s++) {
		uint8_t c = (uint8_t)*s;
		if (c < 32 || c > 126) *s = ' ';
	}
}

/****************************************************************************************
 * 
 */
void displayer_metadata(char *artist, char *album, char *title) {
	if (!display) return;
	
	xSemaphoreTake(displayer.mutex, portMAX_DELAY);
	
	char prev_string[SCROLLABLE_SIZE + 1];
	strncpy(prev_string, displayer.string, SCROLLABLE_SIZE);
	prev_string[SCROLLABLE_SIZE] = '\0';

	bool has_artist = (artist && *artist);
	bool has_title  = (title && *title);
	bool has_album  = (album && *album);

	if (has_artist && has_title && has_album) {
		snprintf(displayer.string, SCROLLABLE_SIZE, "%s - %s (%s)", artist, title, album);
	} else if (has_artist && has_title) {
		snprintf(displayer.string, SCROLLABLE_SIZE, "%s - %s", artist, title);
	} else if (has_title && has_album) {
		snprintf(displayer.string, SCROLLABLE_SIZE, "%s (%s)", title, album);
	} else if (has_title) {
		strncpy(displayer.string, title, SCROLLABLE_SIZE);
	} else if (has_artist) {
		strncpy(displayer.string, artist, SCROLLABLE_SIZE);
	} else if (has_album) {
		strncpy(displayer.string, album, SCROLLABLE_SIZE);
	} else {
		strcpy(displayer.string, "AIRPLAY HI-FI STREAM");
	}
	displayer.string[SCROLLABLE_SIZE] = '\0';
	
	// get optional scroll speed & pause
	if (displayer.metadata_config) {
		PARSE_PARAM(displayer.metadata_config, "speed", '=', displayer.speed);
		PARSE_PARAM(displayer.metadata_config, "pause", '=', displayer.pause);
	}
	
	displayer.offset = 0;	
	utf8_decode(displayer.string);
	sanitize_ascii(displayer.string);

	TickType_t now = xTaskGetTickCount();
	if (!has_artist && !has_title && !has_album) {
		/* App (e.g. YouTube) cleared NowPlayingInfo; discard stale duration from previous app */
		displayer.elapsed = 0;
		displayer.duration.value = 0;
		displayer.duration.visible = false;
		displayer.tick = now;
		displayer.timer = true;
	} else if (strcmp(prev_string, displayer.string) != 0) {
		/* New track/video started; if no fresh progress packet arrived in the last 1.5s, reset timer */
		if ((now - displayer.progress_tick) > pdMS_TO_TICKS(1500)) {
			displayer.elapsed = 0;
			displayer.duration.value = 0;
			displayer.duration.visible = false;
		}
		displayer.tick = now;
		displayer.timer = true;
	}

	ESP_LOGI(TAG, "playing %s", displayer.string);
	displayer.boundary = GDS_TextStretch(display, 2, displayer.string, SCROLLABLE_SIZE);
	displayer.refresh = true;
		
	xSemaphoreGive(displayer.mutex);
	if (displayer.task) vTaskResume(displayer.task);
}	

/****************************************************************************************
 *
 */
void displayer_scroll(char *string, int speed, int pause) {
	if (!display) return;
	
	xSemaphoreTake(displayer.mutex, portMAX_DELAY);

	if (speed) displayer.speed = speed;
	if (pause) displayer.pause = pause;
	displayer.offset = 0;	
	strncpy(displayer.string, string ? string : "", SCROLLABLE_SIZE);
	displayer.string[SCROLLABLE_SIZE] = '\0';
	utf8_decode(displayer.string);
	sanitize_ascii(displayer.string);
	displayer.boundary = GDS_TextStretch(display, 2, displayer.string, SCROLLABLE_SIZE);
	displayer.refresh = true;
		
	xSemaphoreGive(displayer.mutex);
	if (displayer.task) vTaskResume(displayer.task);
}

/****************************************************************************************
 * 
 */
void displayer_timer(enum displayer_time_e mode, int elapsed, int duration) {
	if (!display) return;
	
	xSemaphoreTake(displayer.mutex, portMAX_DELAY);

	TickType_t now = xTaskGetTickCount();
	displayer.tick = now;
	displayer.progress_tick = now;
	displayer.timer = true;

	if (elapsed >= 0) displayer.elapsed = elapsed / 1000;	
	if (duration > 0) {
		displayer.duration.visible = true;
		displayer.duration.value = duration / 1000;
		if (displayer.elapsed > displayer.duration.value) {
			displayer.elapsed = displayer.duration.value;
		}

		if (displayer.duration.value > 3600) sprintf(displayer.duration.string, "%u:%02u:%02u", (displayer.duration.value / 3600) % 10,
													(displayer.duration.value % 3600) / 60, displayer.duration.value % 60);
		else sprintf(displayer.duration.string, "%u:%02u", displayer.duration.value / 60, displayer.duration.value % 60);

		char *buf;
		asprintf(&buf, "%s %s/%s", displayer.header, displayer.duration.string, displayer.duration.string);
		if (GDS_GetTextWidth(display, 1, 0, buf) > GDS_GetWidth(display)) {
			displayer.duration.visible = false;
		}
		free(buf);
	} else {
		displayer.duration.visible = false;
		displayer.duration.value = 0;
	}
		
	xSemaphoreGive(displayer.mutex);
}	

/****************************************************************************************
 * 
 */
void displayer_control(enum displayer_cmd_e cmd, ...) {
	va_list args;
	
	if (!display) return;
	
	va_start(args, cmd);
	xSemaphoreTake(displayer.mutex, portMAX_DELAY);
		
	switch(cmd) {
	case DISPLAYER_ACTIVATE: {	
		char *header = va_arg(args, char*);
		displayer.artwork.active = displayer.artwork.enable && va_arg(args, int);
		strncpy(displayer.header, header ? header : "AIRPLAY", HEADER_SIZE);
		displayer.header[HEADER_SIZE] = '\0';
		displayer.state = DISPLAYER_ACTIVE;
		displayer.timer = false;
		displayer.refresh = true;
		displayer.string[0] = '\0';
		displayer.elapsed = displayer.duration.value = 0;
		displayer.duration.visible = false;
		displayer.offset = displayer.boundary = 0;
		display_bus(&displayer, DISPLAY_BUS_TAKE);
		if (displayer.artwork.active && GDS_GetDepth(display) < 16) GDS_SetTextWidth(display, displayer.artwork.offset);
		vTaskResume(displayer.task);
		break;
	}	
	case DISPLAYER_SUSPEND:		
		displayer.state = DISPLAYER_IDLE;
		displayer_artwork_unlocked(NULL);
		display_bus(&displayer, DISPLAY_BUS_GIVE);
		break;		
	case DISPLAYER_SHUTDOWN:
		GDS_SetTextWidth(display, 0);
		displayer_artwork_unlocked(NULL);
		displayer.state = DISPLAYER_DOWN;
		display_bus(&displayer, DISPLAY_BUS_GIVE);
		break;
	case DISPLAYER_TIMER_RUN:
		if (!displayer.timer) {
			display_bus(&displayer, DISPLAY_BUS_TAKE);
			displayer.timer = true;		
			displayer.tick = xTaskGetTickCount();		
		}	
		break;
	case DISPLAYER_TIMER_PAUSE:
		displayer.timer = false;
		break;
	default:
		break;
	}	
	
	xSemaphoreGive(displayer.mutex);
	va_end(args);
}

/****************************************************************************************
 *
 */
bool display_is_valid_driver(const char * driver){
	return display_conf_get_driver_name(driver)!=NULL;
}

/****************************************************************************************
 *
 */
const char *display_conf_get_driver_name(const char * driver){
	for(uint8_t i=0;known_drivers[i]!=NULL && strlen(known_drivers[i])>0;i++ ){
		if(strcasestr(driver,known_drivers[i])){
			return known_drivers[i];
		}
	}
	return NULL;
}

/****************************************************************************************
 *
 */
char * display_get_supported_drivers(void){
	int total_size = 1;
	char * supported_drivers=NULL;
	const char * separator = "|";
	int separator_len = strlen(separator);

	for(uint8_t i=0;known_drivers[i]!=NULL && strlen(known_drivers[i])>0;i++ ){
		total_size += strlen(known_drivers[i])+separator_len;
	}
	total_size+=2;
	supported_drivers = malloc(total_size);
	memset(supported_drivers,0x00,total_size);
	strcat(supported_drivers,"<");
	for(uint8_t i=0;known_drivers[i]!=NULL && strlen(known_drivers[i])>0;i++ ){
		supported_drivers = strcat(supported_drivers,known_drivers[i]);
		supported_drivers = strcat(supported_drivers,separator);
	}
	strcat(supported_drivers,">");
	return supported_drivers;
}
