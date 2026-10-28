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
			/* Single click: Cycle Brightness 100% -> 60% -> 30% -> 100% */
			if (g_backlight_level == 255) g_backlight_level = 160;      /* 60% */
			else if (g_backlight_level == 160) g_backlight_level = 80;  /* 30% */
			else g_backlight_level = 255;                               /* 100% */
			
			if (display) {
				GDS_DisplayOn(display);
				GDS_SetContrast(display, g_backlight_level);
			}
			int pct = (g_backlight_level == 255) ? 100 : (g_backlight_level == 160 ? 60 : 30);
			snprintf(g_osd_text, sizeof(g_osd_text), "BRT: %d%%", pct);
			g_osd_color = HIFI_AMBER;
			g_osd_expire_tick = now + pdMS_TO_TICKS(1200);
			g_tactile_r = 120; g_tactile_g = 80; g_tactile_b = 0;
			g_tactile_led_expire = now + pdMS_TO_TICKS(180);
			if (led_display) led_vu_color_all(g_tactile_r, g_tactile_g, g_tactile_b);
			return true;
		}
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
