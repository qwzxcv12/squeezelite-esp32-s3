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
static volatile bool g_display_hw_sync_req = false;
static volatile TickType_t g_last_user_activity = 0;

/* On-Screen Display (OSD) feedback overlay */
static char g_osd_text[32] = "";
static TickType_t g_osd_expire_tick = 0;
static uint16_t g_osd_color = HIFI_CYAN;
static TickType_t g_tactile_led_expire = 0;
static uint8_t g_tactile_r = 0, g_tactile_g = 0, g_tactile_b = 0;

volatile int g_system_volume_pct = 50; /* 0..100% */
extern void set_volume(unsigned left, unsigned right) __attribute__((weak));

void display_apply_volume(int pct) {
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	g_system_volume_pct = pct;
	snprintf(g_osd_text, sizeof(g_osd_text), "VOL: %d%%", g_system_volume_pct);
	g_osd_color = HIFI_GOLD;
	g_osd_expire_tick = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
	if (set_volume) {
		float f = pct / 100.0f;
		uint32_t vol = (uint32_t)(65536.0f * (f * f * f));
		set_volume(vol, vol);
	}
}

void display_set_volume(int pct) {
	display_apply_volume(pct);
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
	bool is_active = (displayer.state == DISPLAYER_ACTIVE);
	g_last_user_activity = now;
	ESP_LOGI(TAG, "BUTTON EVENT: GPIO=%d, action=%d, long=%d, active=%d", gpio, action, (int)long_press, (int)is_active);
	
	/* 1. Wake screen if it was sleeping (defer SPI command to displayer_task to prevent SPI collision) */
	if (g_display_sleep_forced) {
		g_display_sleep_forced = false;
		g_display_hw_sync_req = true;
		snprintf(g_osd_text, sizeof(g_osd_text), "WAKE");
		g_osd_color = HIFI_EMERALD;
		g_osd_expire_tick = now + pdMS_TO_TICKS(1200);
		g_tactile_r = 0; g_tactile_g = 100; g_tactile_b = 40;
		g_tactile_led_expire = now + pdMS_TO_TICKS(200);
		return true; /* Woke up device, don't execute action on wake button press */
	}
	
	/* 2. Process according to User's Standardized Layout: */
	switch (gpio) {
	case 13: /* K1 - Volume Up (Click) / Next Track (Hold >= 350ms) */
		if (long_press) {
			/* Next Track on Long Press (Hold >= 350ms) */
			static TickType_t s_last_k1_hold = 0;
			if (now - s_last_k1_hold < pdMS_TO_TICKS(800)) return true; /* Throttle to single skip per hold */
			s_last_k1_hold = now;
			snprintf(g_osd_text, sizeof(g_osd_text), is_active ? "NEXT >>|" : "NEXT (IDLE)");
			g_osd_color = HIFI_VIOLET;
			g_osd_expire_tick = now + pdMS_TO_TICKS(1000);
			g_tactile_r = 100; g_tactile_g = 0; g_tactile_b = 80;
			g_tactile_led_expire = now + pdMS_TO_TICKS(180);
		} else {
			/* Vol Up on Click (Instant tap-tap-tap volume up) */
			int new_vol = g_system_volume_pct;
			if (new_vol <= 95) new_vol += 5; else new_vol = 100;
			display_apply_volume(new_vol);
			g_osd_color = HIFI_VIOLET;
			g_tactile_r = 80; g_tactile_g = 0; g_tactile_b = 120;
			g_tactile_led_expire = now + pdMS_TO_TICKS(140);
		}
		/* In Standby (!is_active), handle locally & immediately without calling disconnected network controls */
		return !is_active;
		
	case 14: /* K2 - Volume Down (Click) / Prev Track (Hold >= 350ms) */
		if (long_press) {
			/* Prev Track on Long Press (Hold >= 350ms) */
			static TickType_t s_last_k2_hold = 0;
			if (now - s_last_k2_hold < pdMS_TO_TICKS(800)) return true; /* Throttle to single skip per hold */
			s_last_k2_hold = now;
			snprintf(g_osd_text, sizeof(g_osd_text), is_active ? "|<< PREV" : "PREV (IDLE)");
			g_osd_color = HIFI_CYAN;
			g_osd_expire_tick = now + pdMS_TO_TICKS(1000);
			g_tactile_r = 0; g_tactile_g = 40; g_tactile_b = 120;
			g_tactile_led_expire = now + pdMS_TO_TICKS(180);
		} else {
			/* Vol Down on Click (Instant tap-tap-tap volume down) */
			int new_vol = g_system_volume_pct;
			if (new_vol >= 5) new_vol -= 5; else new_vol = 0;
			display_apply_volume(new_vol);
			g_osd_color = HIFI_GOLD;
			g_tactile_r = 0; g_tactile_g = 60; g_tactile_b = 100;
			g_tactile_led_expire = now + pdMS_TO_TICKS(140);
		}
		return !is_active;
		
	case 15: /* K3 - Play/Pause (Click) / Mute/Unmute (Hold >= 350ms) */
		if (long_press) {
			static TickType_t s_last_k3_hold = 0;
			if (now - s_last_k3_hold < pdMS_TO_TICKS(400)) return true; /* Throttle repeat */
			s_last_k3_hold = now;

			static bool s_is_muted = false;
			static int s_pre_mute_vol = 50;

			s_is_muted = !s_is_muted;
			if (s_is_muted) {
				s_pre_mute_vol = (g_system_volume_pct > 0) ? g_system_volume_pct : 50;
				g_system_volume_pct = 0;
				display_apply_volume(0);
				snprintf(g_osd_text, sizeof(g_osd_text), "[X] MUTED");
				g_osd_color = HIFI_ROSE;
				g_tactile_r = 150; g_tactile_g = 0; g_tactile_b = 0;
			} else {
				g_system_volume_pct = s_pre_mute_vol;
				display_apply_volume(s_pre_mute_vol);
				snprintf(g_osd_text, sizeof(g_osd_text), "[>] UNMUTE");
				g_osd_color = HIFI_EMERALD;
				g_tactile_r = 0; g_tactile_g = 150; g_tactile_b = 30;
			}
			g_osd_expire_tick = now + pdMS_TO_TICKS(1500);
			g_tactile_led_expire = now + pdMS_TO_TICKS(250);
			return !is_active; /* Pass to audio_controls only when active */
		} else {
			/* Play / Pause Toggle (or Standby Ready status) */
			if (!is_active) {
				snprintf(g_osd_text, sizeof(g_osd_text), "READY AIRPLAY");
				g_osd_color = HIFI_EMERALD;
			} else {
				bool is_currently_playing = (displayer.timer || g_audio_peak_level > 1);
				if (is_currently_playing) {
					snprintf(g_osd_text, sizeof(g_osd_text), "|| PAUSE");
					g_osd_color = HIFI_GOLD;
				} else {
					snprintf(g_osd_text, sizeof(g_osd_text), "> PLAY");
					g_osd_color = HIFI_EMERALD;
				}
			}
			g_osd_expire_tick = now + pdMS_TO_TICKS(1200);
			g_tactile_r = 0; g_tactile_g = 120; g_tactile_b = 30;
			g_tactile_led_expire = now + pdMS_TO_TICKS(180);
		}
		return !is_active;
		
	case 16: /* K4 - Brightness (Click) / Sleep (Hold) */
		if (long_press) {
			static TickType_t s_last_k4_hold = 0;
			if (now - s_last_k4_hold < pdMS_TO_TICKS(600)) return true; /* Throttle repeat */
			s_last_k4_hold = now;

			g_display_sleep_forced = true;
			g_display_hw_sync_req = true;
			snprintf(g_osd_text, sizeof(g_osd_text), "SLEEP");
			g_osd_color = HIFI_ROSE;
			g_osd_expire_tick = now + pdMS_TO_TICKS(1200);
			return true; /* Sleep activated safely via displayer_task */
		} else {
			/* Single click: Cycle Brightness 100% -> 60% -> 30% -> 100% (NEVER black) */
			if (g_backlight_level == 255) g_backlight_level = 160;      /* 60% */
			else if (g_backlight_level == 160) g_backlight_level = 80;  /* 30% */
			else g_backlight_level = 255;                               /* 100% */
			
			g_display_hw_sync_req = true;
			
			const char *bl_str = (g_backlight_level == 255) ? "LCD: 100%" :
			                     (g_backlight_level == 160) ? "LCD: 60%" : "LCD: 30%";
			snprintf(g_osd_text, sizeof(g_osd_text), "%s", bl_str);
			g_osd_color = HIFI_GOLD;
			g_osd_expire_tick = now + pdMS_TO_TICKS(1200);
			g_tactile_r = 100; g_tactile_g = 70; g_tactile_b = 0;
			g_tactile_led_expire = now + pdMS_TO_TICKS(180);
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
static void hifi_draw_vinyl_disc(int cx, int cy, int radius, int anim_tick, bool spinning) {
	int phase = spinning ? (anim_tick & 7) : 1;
	int r2 = radius * radius;
	int r_rim2 = (radius * 94) * (radius * 94) / 10000;
	int r_label2 = (radius * 36) * (radius * 36) / 10000;
	int r_label_in2 = (radius * 32) * (radius * 32) / 10000;
	int r_hole2 = (radius * 10) * (radius * 10) / 10000;
	if (r_hole2 < 4) r_hole2 = 4;

	for (int dy = -radius; dy <= radius; dy++) {
		for (int dx = -radius; dx <= radius; dx++) {
			int d2 = dx * dx + dy * dy;
			if (d2 > r2) continue;
			uint16_t col = 0x0862; /* Deep graphite vinyl body */

			if (d2 >= r_rim2) {
				col = HIFI_PILL;   /* Smooth outer metallic rim */
			} else if (d2 > r_label2) {
				/* Rotating diagonal light sheen across vinyl grooves */
				bool sheen = false;
				int threshold = r2 / 30;
				int threshold2 = r2 / 18;
				switch (phase & 3) {
					case 0: sheen = (dx * dy > threshold); break;
					case 1: sheen = ((dx + dy) * (dx - dy) > threshold2); break;
					case 2: sheen = (dx * dy < -threshold); break;
					case 3: sheen = ((dx - dy) * (dx + dy) > threshold2); break;
				}
				/* Concentric groove rings */
				int g1 = (radius * 75) / 100;
				int g2 = (radius * 55) / 100;
				if (abs(d2 - g1 * g1) < radius * 2) {
					col = sheen ? HIFI_CYAN : HIFI_TRACK;
				} else if (abs(d2 - g2 * g2) < radius * 2) {
					col = sheen ? HIFI_VIOLET : HIFI_TRACK;
				} else if (sheen) {
					col = HIFI_CARD;
				}
			} else if (d2 >= r_label_in2) {
				col = HIFI_CYAN;   /* Outer label ring */
			} else if (d2 >= r_hole2) {
				col = (dx + dy > 0) ? HIFI_EMERALD : HIFI_VIOLET; /* Vibrant dual-tone label */
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
		/* Full-width Turntable Deck Card in Portrait: X: 4..123, Y: 20..81 (W:120, H:62) */
		GDS_ClearWindow(display, 0, 20, 127, 81, HIFI_BG);
		hifi_fill_round_rect(4, 20, 120, 62, HIFI_CARD, HIFI_PILL);

		g_has_custom_artwork = false;
		/* 4-tier mini vertical volume meter ticks (Y: 37, 44, 51, 58) */
		int v_thresholds[4] = { 90, 60, 30, 1 };
		for (int s = 0; s < 4; s++) {
			int sy = 37 + s * 7;
			bool tier_lit = (g_system_volume_pct >= v_thresholds[s]);
			uint16_t sc = tier_lit ? HIFI_GOLD : HIFI_TRACK;
			GDS_DrawBox(display, 13, sy, 15, sy + 2, sc, true);
		}
		/* Volume level display on left of turntable (replacing legacy 33 RPM indicator) */
		char vol_str[8];
		if (g_system_volume_pct == 0) {
			strcpy(vol_str, "MUT");
		} else {
			snprintf(vol_str, sizeof(vol_str), "%d%%", g_system_volume_pct);
		}
		int vx = 4 + (32 - (int)strlen(vol_str) * 7) / 2;
		if (vx < 5) vx = 5;
		GDS_DrawBox(display, 5, 66, 35, 80, HIFI_CARD, true);
		GDS_SetFont(display, &Font_droid_sans_mono_7x13);
		GDS_FontDrawString(display, vx, 67, vol_str, (g_system_volume_pct == 0) ? HIFI_ROSE : HIFI_SLATE);

		/* Centered Large Vinyl Disc (Radius 27, cx 64, cy 51) - always smooth turntable */
		hifi_draw_vinyl_disc(64, 51, 27, 0, false);

		/* Tonearm on right parked */
		GDS_DrawBox(display, 111, 28, 115, 32, HIFI_PILL, true);
		GDS_DrawPixelFast(display, 113, 30, HIFI_WHITE);
		GDS_DrawVLine(display, 113, 24, 4, HIFI_SLATE);
		GDS_DrawVLine(display, 113, 30, 20, HIFI_PILL);
		GDS_DrawBox(display, 112, 50, 114, 53, HIFI_GOLD, true);
		GDS_DrawHLine(display, 110, 42, 6, HIFI_SLATE);
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
			hifi_draw_vinyl_disc(129, 78, 22, 0, false);
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
		int bar_segs[14]  = {4, 7, 11, 9, 13, 15, 12, 10, 14, 12, 10, 8, 6, 4};
		int bar_peaks[14] = {5, 8, 12, 10, 14, 16, 13, 11, 15, 13, 11, 9, 7, 5};
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

			if (g_display_hw_sync_req) {
				g_display_hw_sync_req = false;
				if (!g_display_sleep_forced) {
					if (pwr_state == 2) GDS_DisplayOn(display);
					GDS_SetContrast(display, g_backlight_level);
					pwr_state = 0;
				}
			}

			if (g_display_sleep_forced) {
				if (pwr_state != 2) {
					GDS_SetContrast(display, 0);
					GDS_DisplayOff(display);
					pwr_state = 2;
				}
				for (int w = 0; w < 8 && g_display_sleep_forced && !g_display_hw_sync_req; w++) {
					vTaskDelay(pdMS_TO_TICKS(25));
				}
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
			GDS_ClearWindow(display, 0, 0, 127, 1, HIFI_BG);
			GDS_ClearWindow(display, 0, 2, 3, 18, HIFI_BG);
			GDS_ClearWindow(display, 124, 2, 127, 18, HIFI_BG);
			GDS_ClearWindow(display, 0, 19, 127, 19, HIFI_BG);
			GDS_ClearWindow(display, 0, 20, 3, 81, HIFI_BG);
			GDS_ClearWindow(display, 124, 20, 127, 81, HIFI_BG);
			GDS_ClearWindow(display, 0, 82, 127, 82, HIFI_BG);
			GDS_ClearWindow(display, 0, 83, 3, 103, HIFI_BG);
			GDS_ClearWindow(display, 124, 83, 127, 103, HIFI_BG);
			GDS_ClearWindow(display, 0, 104, 127, 104, HIFI_BG);
			GDS_ClearWindow(display, 0, 105, 3, 158, HIFI_BG);
			GDS_ClearWindow(display, 124, 105, 127, 158, HIFI_BG);
			GDS_ClearWindow(display, 0, 159, 127, 159, HIFI_BG);

			/* -------------------------------------------------------------
			 * ZONE 1: HEADER & TIMELINE SCRUBBER (X:4..123, Y:2..18)
			 * ------------------------------------------------------------- */
			hifi_fill_round_rect(4, 2, 120, 17, HIFI_CARD, HIFI_PILL);

			/* Row 1: Status Dot + Source Badge (AIRPLAY) + Time (01:23 / 03:45) */
			uint16_t dot_col = is_active ? ((anim_tick & 8) ? HIFI_EMERALD : HIFI_SLATE) : HIFI_SLATE;
			GDS_DrawBox(display, 8, 6, 11, 9, dot_col, true);

			GDS_SetFont(display, &Font_droid_sans_fallback_11x13);
			GDS_FontDrawString(display, 15, 3, header_buf, HIFI_WHITE);

			if (is_active && cur_duration > 0 && cur_duration < 3600) {
				snprintf(time_buf, sizeof(time_buf), "%u:%02u/%u:%02u",
					(unsigned)(cur_elapsed / 60), (unsigned)(cur_elapsed % 60),
					(unsigned)(cur_duration / 60), (unsigned)(cur_duration % 60));
			} else if (is_active && cur_elapsed > 0) {
				snprintf(time_buf, sizeof(time_buf), "%u:%02u", (unsigned)(cur_elapsed / 60), (unsigned)(cur_elapsed % 60));
			} else {
				strcpy(time_buf, "32-BIT");
			}

			GDS_SetFont(display, &Font_droid_sans_mono_7x13);
			int timew = GDS_FontMeasureString(display, time_buf);
			uint16_t time_col = (strcmp(time_buf, "32-BIT") == 0) ? HIFI_GOLD : HIFI_SLATE;
			GDS_FontDrawString(display, max(58, 120 - timew), 3, time_buf, time_col);

			/* Row 2: Scrubber timeline bar at Y: 15..16 in Warm Studio Gold (Width 112px, X: 8..119) */
			GDS_DrawHLine(display, 8, 15, 112, HIFI_TRACK);
			GDS_DrawHLine(display, 8, 16, 112, HIFI_TRACK);
			if (is_active && cur_duration > 0) {
				int fill_w = (int)((cur_elapsed * 112) / cur_duration);
				if (fill_w > 112) fill_w = 112;
				if (fill_w > 0) {
					GDS_DrawHLine(display, 8, 15, fill_w, HIFI_GOLD);
					GDS_DrawHLine(display, 8, 16, fill_w, HIFI_GOLD);
				}
			} else if (is_active && is_playing) {
				int pulse_x = 8 + ((anim_tick * 4) % 96);
				GDS_DrawHLine(display, pulse_x, 15, 16, HIFI_GOLD);
				GDS_DrawHLine(display, pulse_x, 16, 16, HIFI_GOLD);
			}

			/* -------------------------------------------------------------
			 * ZONE 2: TURNTABLE DECK & VINYL RECORD (X:4..123, Y:20..81)
			 * ------------------------------------------------------------- */
			uint8_t peak = g_audio_peak_level;
			bool record_spinning = is_active && (is_playing || peak > 2);

			if (!g_has_custom_artwork) {
				/* 4-tier mini vertical volume meter ticks (Y: 37, 44, 51, 58) */
				int v_thresholds[4] = { 90, 60, 30, 1 };
				for (int s = 0; s < 4; s++) {
					int sy = 37 + s * 7;
					bool tier_lit = (g_system_volume_pct >= v_thresholds[s]);
					uint16_t sc = HIFI_TRACK;
					if (tier_lit) {
						sc = (record_spinning && ((anim_tick + s) & 2)) ? HIFI_WHITE : HIFI_GOLD;
					}
					GDS_DrawBox(display, 13, sy, 15, sy + 2, sc, true);
				}
				/* Volume level display on left of turntable (replacing legacy 33 RPM indicator) */
				char vol_str[8];
				uint16_t v_col;
				if (g_system_volume_pct == 0) {
					strcpy(vol_str, "MUT");
					v_col = HIFI_ROSE;
				} else {
					snprintf(vol_str, sizeof(vol_str), "%d%%", g_system_volume_pct);
					v_col = is_active ? HIFI_GOLD : HIFI_SLATE;
				}
				int vx = 4 + (32 - (int)strlen(vol_str) * 7) / 2;
				if (vx < 5) vx = 5;
				GDS_DrawBox(display, 5, 66, 35, 80, HIFI_CARD, true);
				GDS_SetFont(display, &Font_droid_sans_mono_7x13);
				GDS_FontDrawString(display, vx, 67, vol_str, v_col);

				/* Large Centered Vinyl Disc (Radius 27, cx 64, cy 51) */
				hifi_draw_vinyl_disc(64, 51, 27, anim_tick, record_spinning);

				/* Tonearm Assembly on right */
				GDS_DrawBox(display, 111, 28, 115, 32, HIFI_PILL, true);
				GDS_DrawPixelFast(display, 113, 30, HIFI_WHITE);
				GDS_DrawVLine(display, 113, 24, 4, HIFI_SLATE); /* Counterweight */

				if (record_spinning) {
					/* Tonearm playing on record */
					GDS_DrawLine(display, 113, 30, 105, 40, HIFI_PILL);
					GDS_DrawLine(display, 105, 40, 93, 46, HIFI_PILL);
					GDS_DrawLine(display, 93, 46, 85, 51, HIFI_WHITE);
					GDS_DrawBox(display, 83, 50, 85, 52, HIFI_GOLD, true); /* Cartridge */
				} else {
					/* Tonearm parked on rest */
					GDS_DrawVLine(display, 113, 30, 20, HIFI_PILL);
					GDS_DrawBox(display, 112, 50, 114, 53, HIFI_GOLD, true);
					GDS_DrawHLine(display, 110, 42, 6, HIFI_SLATE);
				}
			}

			/* -------------------------------------------------------------
			 * ZONE 3: TRACK MARQUEE / POPUP OSD (X:4..123, Y:83..103)
			 * ------------------------------------------------------------- */
			bool show_osd = (xTaskGetTickCount() < g_osd_expire_tick && g_osd_text[0] != '\0');
			if (show_osd) {
				hifi_fill_round_rect(4, 83, 120, 21, HIFI_CARD, g_osd_color);
				GDS_SetFont(display, &Font_droid_sans_fallback_11x13);
				int otw = GDS_FontMeasureString(display, g_osd_text);
				int ox = max(8, 64 - otw / 2);
				int oy = (strncmp(g_osd_text, "VOL", 3) == 0) ? 84 : 87;
				GDS_FontDrawString(display, ox, oy, g_osd_text, g_osd_color);

				if (strncmp(g_osd_text, "VOL", 3) == 0) {
					int bar_x = 14;
					int bar_w = 100;
					int fill_w = (bar_w * g_system_volume_pct) / 100;
					if (fill_w > bar_w) fill_w = bar_w;
					GDS_DrawHLine(display, bar_x, 99, bar_w, HIFI_TRACK);
					GDS_DrawHLine(display, bar_x, 100, bar_w, HIFI_TRACK);
					if (fill_w > 0) {
						GDS_DrawHLine(display, bar_x, 99, fill_w, g_osd_color);
						GDS_DrawHLine(display, bar_x, 100, fill_w, g_osd_color);
					}
				}
			} else {
				hifi_fill_round_rect(4, 83, 120, 21, HIFI_CARD, HIFI_PILL);
				GDS_SetFont(display, &Font_droid_sans_fallback_11x13);
				int text_w = GDS_FontMeasureString(display, track_buf);
				if (text_w < 112) text_w = 112;

				int text_color = is_active ? HIFI_WHITE : HIFI_SLATE;
				GDS_SetTextWidth(display, 119);
				GDS_FontDrawString(display, 8 - marquee_x, 87, track_buf, text_color);
				if (8 + text_w - marquee_x < 119) {
					GDS_FontDrawString(display, 8 + text_w - marquee_x, 87, track_buf, text_color);
				}
				GDS_SetTextWidth(display, 128);

				/* Clip edges */
				GDS_ClearWindow(display, 0, 83, 3, 103, HIFI_BG);
				GDS_ClearWindow(display, 124, 83, 127, 103, HIFI_BG);
				marquee_x = (marquee_x + 2) % text_w;
			}

			/* -------------------------------------------------------------
			 * ZONE 4: TALL 14-BAND EQUALIZER SPECTRUM (X:4..123, Y:105..158, H:54)
			 * (No subtitle text - Full 54px dedicated to 14 high-density LED bars!)
			 * ------------------------------------------------------------- */
			hifi_fill_round_rect(4, 105, 120, 54, HIFI_CARD, HIFI_PILL);

			/* 14 spectrum bars (6px width, 16 tall LED levels each, perfectly filling 47px inside 54px card!) */
			for (int i = 0; i < 14; i++) {
				int target = 1;
				if (is_active && peak > 2) {
					int dist = (i < 6) ? (6 - i) : (i - 6);
					int weight = 135 - dist * 10;
					int jitter = ((anim_tick * 5 + i * 17) % 5) - 2;
					/* Boosted dynamic gain: lively response even at normal listening volume */
					target = ((int)peak * weight * 16) / 5800 + jitter;
				} else if (is_playing) {
					target = 4 + ((anim_tick * 2 + i * 7) % 11);
				} else {
					int wave = ((anim_tick + i * 2) % 24);
					if (wave > 12) wave = 24 - wave;
					target = 2 + (wave / 3);
				}

				if (target < 1) target = 1;
				if (target > 16) target = 16;

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

				int bx = 8 + i * 8; /* 14 bars * 8px = 112px (X: 8..119) */
				for (int s = 0; s < 16; s++) {
					int sy = 154 - s * 3; /* Levels 0..15 span Y: 154 down to Y: 109 (47px height, 4px top padding, 3px bottom padding!) */
					uint16_t seg_col = HIFI_TRACK;
					if (s < bar_segs[i]) {
						if (s < 3) seg_col = HIFI_VIOLET;        /* Sub-Bass: Deep Violet (3 levels) */
						else if (s < 6) seg_col = HIFI_CYAN;     /* Bass: Electric Cyan (3 levels) */
						else if (s < 9) seg_col = HIFI_EMERALD;  /* Mid: Neon Emerald (3 levels) */
						else if (s < 13) seg_col = HIFI_GOLD;    /* Highs/Treble: Studio Warm Gold (4 levels!) */
						else seg_col = HIFI_ROSE;                 /* Peak/Redline: Rose Punch (3 levels!) */
					} else if (s == bar_peaks[i] && s > 0) {
						seg_col = HIFI_WHITE;                     /* Floating Peak Hold */
					}
					GDS_DrawHLine(display, bx, sy, 6, seg_col);
					GDS_DrawHLine(display, bx, sy + 1, 6, seg_col);
				}
			}

			/* Onboard WS2812 RGB LED disabled per user request */
			static bool s_rgb_off_done = false;
			if (led_display && !s_rgb_off_done) {
				led_vu_color_all(0, 0, 0);
				s_rgb_off_done = true;
			}

			GDS_Update(display);
			anim_tick++;
			if (pwr_state == 1) {
				for (int w = 0; w < 20 && !g_display_hw_sync_req && g_last_user_activity <= last_active_tick && displayer.state != DISPLAYER_ACTIVE; w++) {
					vTaskDelay(pdMS_TO_TICKS(25));
				}
			} else {
				vTaskDelay(pdMS_TO_TICKS(50));
			}
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

			if (g_display_hw_sync_req) {
				g_display_hw_sync_req = false;
				if (!g_display_sleep_forced) {
					if (pwr_state == 2) GDS_DisplayOn(display);
					GDS_SetContrast(display, g_backlight_level);
					pwr_state = 0;
				}
			}

			if (g_display_sleep_forced) {
				if (pwr_state != 2) {
					GDS_SetContrast(display, 0);
					GDS_DisplayOff(display);
					pwr_state = 2;
				}
				for (int w = 0; w < 8 && g_display_sleep_forced && !g_display_hw_sync_req; w++) {
					vTaskDelay(pdMS_TO_TICKS(25));
				}
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
				hifi_draw_vinyl_disc(129, 78, 22, anim_tick, is_active && (is_playing || peak > 2));
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

			/* Onboard WS2812 RGB LED disabled per user request */
			if (led_display) {
				// Disabled
			}

			GDS_Update(display);
			anim_tick++;
			if (pwr_state == 1) {
				for (int w = 0; w < 20 && !g_display_hw_sync_req && g_last_user_activity <= last_active_tick && displayer.state != DISPLAYER_ACTIVE; w++) {
					vTaskDelay(pdMS_TO_TICKS(25));
				}
			} else {
				vTaskDelay(pdMS_TO_TICKS(50));
			}
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
	/* Avoid blocking RTSP audio thread with heavy external artwork decoding */
	if (!display || data != default_artwork) return;
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
