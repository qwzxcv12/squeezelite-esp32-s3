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

/* UI/UX Pro Max Dark Hi-Fi Audio Streamer Color Tokens (RGB565) */
#define HIFI_BG			0x0844  /* Deep Navy #080920 */
#define HIFI_HEADER		0x18F9  /* Royal Indigo Header #1E1B4B */
#define HIFI_CARD		0x10E5  /* Elevated Slate Card #111827 */
#define HIFI_BORDER		0x3190  /* Slate Border #313380 */
#define HIFI_INDIGO		0x633E  /* Indigo Accent #6366F1 */
#define HIFI_GREEN		0x262A  /* Hi-Res Emerald #22C55E */
#define HIFI_CYAN		0x07FF  /* Cyan Spectrum Low #00FFFF */
#define HIFI_AMBER		0xFDC0  /* Amber Peak/Clock #FBBF24 */
#define HIFI_SLATE		0x9536  /* Muted Slate Label #94A3B8 */
#define HIFI_WHITE		0xFFFF  /* Crisp White Text #FFFFFF */

/* Real-time audio peak level (0..100) updated by output_i2s.c */
volatile uint8_t g_audio_peak_level = 0;

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
} displayer;

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
 * Render Album Cover JPEG or Hi-Fi Vinyl Graphic into Right Deck Box (X:94..157, Y:48..113)
 */
static void hifi_draw_album_box(const uint8_t *jpeg_data) {
	if (!display || GDS_GetDepth(display) != 16) return;

	GDS_ClearWindow(display, 94, 48, 159, 113, HIFI_CARD);

	if (jpeg_data) {
		uint16_t orig_h = display->Height;
		display->Height = 113;
		GDS_DrawJPEG(display, (uint8_t*)jpeg_data, 95, 49, GDS_IMAGE_CENTER | GDS_IMAGE_FIT);
		display->Height = orig_h;
		GDS_ClearWindow(display, 157, 48, 159, 113, HIFI_BG);
	} else {
		/* Stylish vector Hi-Fi Vinyl / Disc icon when no JPEG is provided */
		GDS_DrawBox(display, 108, 62, 143, 99, HIFI_BORDER, true);
		GDS_DrawBox(display, 114, 68, 137, 93, HIFI_HEADER, true);
		GDS_DrawBox(display, 122, 76, 129, 85, HIFI_AMBER, true);
	}

	/* Crisp Indigo border around album box */
	GDS_DrawBox(display, 94, 48, 157, 113, HIFI_INDIGO, false);
}

/****************************************************************************************
 * 
 */
void display_init(char *welcome) {
	bool init = false;
	char *config = config_alloc_get_str("display_config", CONFIG_DISPLAY_CONFIG, "N/A");
	
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
		static EXT_RAM_ATTR StackType_t xStack[DISPLAYER_STACK_SIZE] __attribute__ ((aligned (4)));
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
	/* Dedicated 20 FPS Hi-Fi Dashboard for 16-bit color 160x128 ST7735 screens */
	if (GDS_GetDepth(display) == 16 && GDS_GetWidth(display) >= 160 && GDS_GetHeight(display) >= 128) {
		int marquee_x = 0;
		int anim_tick = 0;
		int bar_heights[10] = {4, 6, 8, 5, 7, 9, 6, 4, 5, 3};
		int bar_peaks[10]   = {6, 8, 10, 7, 9, 11, 8, 6, 7, 5};
		char last_track[SCROLLABLE_SIZE + 1] = "";
		bool artwork_initialized = false;

		GDS_SetTextWidth(display, GDS_GetWidth(display));
		GDS_ClearWindow(display, 0, 0, 159, 127, HIFI_BG);
		hifi_draw_album_box(default_artwork);
		artwork_initialized = true;

		while (1) {
			char header_buf[HEADER_SIZE + 1];
			char track_buf[SCROLLABLE_SIZE + 32];
			char time_buf[24] = "";
			bool is_active = (displayer.state == DISPLAYER_ACTIVE);
			bool is_playing = (displayer.timer && is_active);
			uint32_t cur_elapsed = 0, cur_duration = 0;

			xSemaphoreTake(displayer.mutex, portMAX_DELAY);

			if (is_playing) {
				TickType_t now = xTaskGetTickCount();
				uint32_t dt_ms = (now - displayer.tick) * portTICK_PERIOD_MS;
				if (dt_ms >= 1000) {
					displayer.tick = now;
					displayer.elapsed += dt_ms / 1000;
				}
			}
			cur_elapsed = displayer.elapsed;
			cur_duration = displayer.duration.value;

			if (is_active && displayer.header[0]) {
				strncpy(header_buf, displayer.header, HEADER_SIZE);
				header_buf[HEADER_SIZE] = '\0';
			} else {
				strcpy(header_buf, "SQUEEZELITE S3");
			}

			if (is_active) {
				if (displayer.string[0]) {
					snprintf(track_buf, sizeof(track_buf), "%s   *   ", displayer.string);
				} else {
					strcpy(track_buf, "AIRPLAY HI-FI STREAMING - PCM5102A 32-BIT DAC   *   ");
				}
			} else {
				if (displayer.string[0] && strcasecmp(displayer.string, "SqueezeESP32") != 0) {
					snprintf(track_buf, sizeof(track_buf), "%s - AIRPLAY HI-FI READY   *   ", displayer.string);
				} else {
					strcpy(track_buf, "SQUEEZELITE ESP32-S3 HI-FI STREAMER - AIRPLAY READY   *   ");
				}
			}

			if (strcmp(last_track, track_buf) != 0) {
				strncpy(last_track, track_buf, SCROLLABLE_SIZE);
				last_track[SCROLLABLE_SIZE] = '\0';
				marquee_x = 0;
			}

			if (is_active && !displayer.artwork.updated && !artwork_initialized) {
				hifi_draw_album_box(default_artwork);
				artwork_initialized = true;
			}

			xSemaphoreGive(displayer.mutex);

			/* Clear all non-album background margins so no stray boot pixels remain */
			GDS_DrawHLine(display, 0, 17, 160, HIFI_BG);
			GDS_ClearWindow(display, 0, 46, 159, 47, HIFI_BG);
			GDS_ClearWindow(display, 0, 48, 1, 113, HIFI_BG);
			GDS_ClearWindow(display, 92, 48, 93, 113, HIFI_BG);
			GDS_ClearWindow(display, 158, 48, 159, 113, HIFI_BG);
			GDS_ClearWindow(display, 0, 114, 159, 115, HIFI_BG);
			GDS_ClearWindow(display, 0, 126, 159, 127, HIFI_BG);
			GDS_DrawBox(display, 94, 48, 157, 113, HIFI_INDIGO, false);

			/* =========================================================================
			 * ZONE 1: TOP HEADER BAR (Y: 0..16)
			 * ========================================================================= */
			GDS_ClearWindow(display, 0, 0, 159, 15, HIFI_HEADER);
			GDS_DrawHLine(display, 0, 16, 160, HIFI_INDIGO);

			/* Status Indicator Dot */
			GDS_DrawBox(display, 4, 5, 9, 10, is_active ? HIFI_GREEN : HIFI_CYAN, true);

			GDS_SetFont(display, &Font_droid_sans_fallback_11x13);
			GDS_FontDrawString(display, 13, 2, header_buf, is_active ? HIFI_GREEN : HIFI_CYAN);

			if (is_active && (is_playing || cur_elapsed > 0)) {
				if (cur_duration > 0 && cur_duration < 36000) {
					snprintf(time_buf, sizeof(time_buf), "%u:%02u/%u:%02u",
						(unsigned)(cur_elapsed / 60), (unsigned)(cur_elapsed % 60),
						(unsigned)(cur_duration / 60), (unsigned)(cur_duration % 60));
				} else {
					snprintf(time_buf, sizeof(time_buf), "%u:%02u",
						(unsigned)(cur_elapsed / 60), (unsigned)(cur_elapsed % 60));
				}
				GDS_SetFont(display, &Font_droid_sans_mono_7x13);
				int tw = GDS_FontMeasureString(display, time_buf);
				GDS_FontDrawString(display, max(82, 157 - tw), 2, time_buf, HIFI_AMBER);
			} else {
				GDS_SetFont(display, &Font_droid_sans_mono_7x13);
				GDS_FontDrawString(display, 120, 2, "HI-FI", HIFI_AMBER);
			}

			/* =========================================================================
			 * ZONE 2: LINE 2 CONTINUOUS SMOOTH MARQUEE CARD (Y: 18..45)
			 * ========================================================================= */
			GDS_ClearWindow(display, 0, 18, 159, 45, HIFI_CARD);
			GDS_DrawHLine(display, 0, 18, 160, HIFI_BORDER);
			GDS_DrawHLine(display, 0, 45, 160, HIFI_BORDER);

			GDS_SetFont(display, &Font_line_2);
			int text_w = GDS_FontMeasureString(display, track_buf);
			if (text_w < 160) text_w = 160;

			int text_color = is_active ? HIFI_WHITE : HIFI_GREEN;
			GDS_FontDrawString(display, -marquee_x, 21, track_buf, text_color);
			if (text_w - marquee_x < 160) {
				GDS_FontDrawString(display, text_w - marquee_x, 21, track_buf, text_color);
			}
			marquee_x = (marquee_x + 2) % text_w;

			/* =========================================================================
			 * ZONE 3 LEFT: REAL-TIME 10-BAR SPECTRUM DECK (X: 2..91, Y: 48..113)
			 * ========================================================================= */
			GDS_ClearWindow(display, 2, 48, 91, 113, HIFI_CARD);
			GDS_DrawBox(display, 2, 48, 91, 113, HIFI_BORDER, false);

			GDS_SetFont(display, &Font_droid_sans_mono_7x13);
			GDS_FontDrawString(display, 6, 50, is_active ? "PCM 44.1kHz" : "DAC READY", HIFI_SLATE);

			uint8_t peak = g_audio_peak_level;
			for (int i = 0; i < 10; i++) {
				int target = 3;
				if (is_active && peak > 2) {
					/* Organic frequency band weighting + live jitter */
					int weight = (i == 0 || i == 9) ? 55 : ((i >= 2 && i <= 6) ? 100 : 80);
					int jitter = ((anim_tick * 7 + i * 19) % 15) - 7;
					target = ((int)peak * weight) / 230 + jitter;
				} else if (is_playing) {
					/* Fallback animated spectrum if stream volume is very soft */
					target = 6 + ((anim_tick * 3 + i * 11) % 18);
				} else {
					/* Gentle idle wave in standby */
					int wave = ((anim_tick + i * 3) % 16);
					if (wave > 8) wave = 16 - wave;
					target = 2 + wave / 2;
				}

				if (target < 2) target = 2;
				if (target > 42) target = 42;

				if (target > bar_heights[i]) {
					bar_heights[i] = (bar_heights[i] + target * 2) / 3;
				} else {
					bar_heights[i] = max(target, bar_heights[i] - 3);
				}

				if (bar_heights[i] >= bar_peaks[i]) {
					bar_peaks[i] = bar_heights[i];
				} else if ((anim_tick & 1) == 0 && bar_peaks[i] > 2) {
					bar_peaks[i]--;
				}

				int bx = 6 + i * 8;
				int by_bottom = 110;
				int h = bar_heights[i];

				/* Segmented color bar: Cyan bottom -> Green mid -> Amber top */
				int h_cyan = min(h, 18);
				if (h_cyan > 0) {
					GDS_DrawBox(display, bx, by_bottom - h_cyan, bx + 5, by_bottom, HIFI_CYAN, true);
				}
				if (h > 18) {
					int h_green = min(h, 32);
					GDS_DrawBox(display, bx, by_bottom - h_green, bx + 5, by_bottom - 19, HIFI_GREEN, true);
				}
				if (h > 32) {
					GDS_DrawBox(display, bx, by_bottom - h, bx + 5, by_bottom - 33, HIFI_AMBER, true);
				}

				/* Peak hold indicator line */
				int py = by_bottom - bar_peaks[i] - 1;
				if (py >= 66) {
					GDS_DrawHLine(display, bx, py, 6, HIFI_WHITE);
				}
			}

			/* =========================================================================
			 * ZONE 4: BOTTOM PROGRESS BAR (X: 2..157, Y: 116..125)
			 * ========================================================================= */
			GDS_ClearWindow(display, 2, 116, 157, 125, HIFI_CARD);
			GDS_DrawBox(display, 2, 116, 157, 125, HIFI_BORDER, false);

			if (is_active && cur_duration > 0) {
				int fill_w = (int)((cur_elapsed * 151) / cur_duration);
				if (fill_w > 151) fill_w = 151;
				if (fill_w > 0) {
					GDS_DrawBox(display, 4, 118, 4 + fill_w, 123, HIFI_GREEN, true);
				}
			} else if (is_active) {
				/* Indeterminate glowing scanner bar for live AirPlay streams */
				int pos = (anim_tick * 3) % 120;
				GDS_DrawBox(display, 4 + pos, 118, min(155, 4 + pos + 32), 123, HIFI_GREEN, true);
			} else {
				/* Subtle indigo accent bar in standby */
				int pos = (anim_tick * 2) % 126;
				GDS_DrawBox(display, 4 + pos, 119, min(155, 4 + pos + 26), 122, HIFI_INDIGO, true);
			}

			GDS_Update(display);
			anim_tick++;
			vTaskDelay(pdMS_TO_TICKS(50));
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

	if (GDS_GetDepth(display) == 16 && GDS_GetWidth(display) >= 160 && GDS_GetHeight(display) >= 128) {
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

	if (displayer.timer) displayer.tick = xTaskGetTickCount();
	if (elapsed >= 0) displayer.elapsed = elapsed / 1000;	
	if (duration > 0) {
		displayer.duration.visible = true;
		displayer.duration.value = duration / 1000;

		if (displayer.duration.value > 3600) sprintf(displayer.duration.string, "%u:%02u:%02u", (displayer.duration.value / 3600) % 10,
													(displayer.duration.value % 3600) / 60, displayer.duration.value % 60);
		else sprintf(displayer.duration.string, "%u:%02u", displayer.duration.value / 60, displayer.duration.value % 60);

		char *buf;
		asprintf(&buf, "%s %s/%s", displayer.header, displayer.duration.string, displayer.duration.string);
		if (GDS_GetTextWidth(display, 1, 0, buf) > GDS_GetWidth(display)) {
			displayer.duration.visible = false;
		}
		free(buf);
	} else if (!duration) {
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
