/*
 *  (c) Philippe G. 20201, philippe_44@outlook.com
 *	see other copyrights below
 *
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tools.h"

#if CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS < 2
#error CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS must be at least 2
#endif

const static char TAG[] = "tools";

/****************************************************************************************
 * UTF-8 tools
 */

// Copyright (c) 2008-2009 Bjoern Hoehrmann <bjoern@hoehrmann.de>
// See http://bjoern.hoehrmann.de/utf-8/decoder/dfa/ for details.
// Copyright (c) 2017 ZephRay <zephray@outlook.com>
//
// utf8to1252 - almost equivalent to iconv -f utf-8 -t windows-1252, but better

#define UTF8_ACCEPT 0
#define UTF8_REJECT 1

static const uint8_t utf8d[] = {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 00..1f
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 20..3f
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 40..5f
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 60..7f
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, // 80..9f
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, // a0..bf
	8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, // c0..df
	0xa,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3, // e0..ef
	0xb,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8, // f0..ff
	0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1, // s0..s0
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1, // s1..s2
	1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1, // s3..s4
	1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1, // s5..s6
	1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // s7..s8
};

static uint32_t decode(uint32_t* state, uint32_t* codep, uint32_t byte) {
	uint32_t type = utf8d[byte];

	*codep = (*state != UTF8_ACCEPT) ?
		(byte & 0x3fu) | (*codep << 6) :
		(0xff >> type) & (byte);

	*state = utf8d[256 + *state*16 + type];
	return *state;
}

static uint8_t UNICODEtoCP1252(uint16_t chr) {
	if (chr < 0x80) return (uint8_t)chr;
	switch (chr) {
		case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
		case 0x0102: case 0x1EA0: case 0x1EA2: case 0x1EA4: case 0x1EA6: case 0x1EA8:
		case 0x1EAA: case 0x1EAC: case 0x1EAE: case 0x1EB0: case 0x1EB2: case 0x1EB4: case 0x1EB6: return 'A';
		case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
		case 0x0103: case 0x1EA1: case 0x1EA3: case 0x1EA5: case 0x1EA7: case 0x1EA9:
		case 0x1EAB: case 0x1EAD: case 0x1EAF: case 0x1EB1: case 0x1EB3: case 0x1EB5: case 0x1EB7: return 'a';
		case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
		case 0x1EB8: case 0x1EBA: case 0x1EBC: case 0x1EBE: case 0x1EC0: case 0x1EC2: case 0x1EC4: case 0x1EC6: return 'E';
		case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
		case 0x1EB9: case 0x1EBB: case 0x1EBD: case 0x1EBF: case 0x1EC1: case 0x1EC3: case 0x1EC5: case 0x1EC7: return 'e';
		case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: case 0x0128: case 0x1EC8: case 0x1ECA: return 'I';
		case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: case 0x0129: case 0x1EC9: case 0x1ECB: return 'i';
		case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8:
		case 0x01A0: case 0x1ECC: case 0x1ECE: case 0x1ED0: case 0x1ED2: case 0x1ED4:
		case 0x1ED6: case 0x1ED8: case 0x1EDA: case 0x1EDC: case 0x1EDE: case 0x1EE0: case 0x1EE2: return 'O';
		case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
		case 0x01A1: case 0x1ECD: case 0x1ECF: case 0x1ED1: case 0x1ED3: case 0x1ED5:
		case 0x1ED7: case 0x1ED9: case 0x1EDB: case 0x1EDD: case 0x1EDF: case 0x1EE1: case 0x1EE3: return 'o';
		case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
		case 0x0168: case 0x01AF: case 0x1EE4: case 0x1EE6: case 0x1EE8: case 0x1EEA: case 0x1EEC: case 0x1EEE: case 0x1EF0: return 'U';
		case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
		case 0x0169: case 0x01B0: case 0x1EE5: case 0x1EE7: case 0x1EE9: case 0x1EEB: case 0x1EED: case 0x1EEF: case 0x1EF1: return 'u';
		case 0x00DD: case 0x1EF2: case 0x1EF4: case 0x1EF6: case 0x1EF8: return 'Y';
		case 0x00FD: case 0x00FF: case 0x1EF3: case 0x1EF5: case 0x1EF7: case 0x1EF9: return 'y';
		case 0x00D0: case 0x0110: return 'D';
		case 0x00F0: case 0x0111: return 'd';
		case 0x00D1: return 'N';
		case 0x00F1: return 'n';
		default: break;
	}
	if (chr <= 0xff)
		return (chr&0xff);
	else {
		ESP_LOGI(TAG, "some multi-byte %hx", chr);
		switch(chr) {
			case 0x20ac: return 0x80; break;
			case 0x201a: return 0x82; break;
			case 0x0192: return 0x83; break;
			case 0x201e: return 0x84; break;
			case 0x2026: return 0x85; break;
			case 0x2020: return 0x86; break;
			case 0x2021: return 0x87; break;
			case 0x02c6: return 0x88; break;
			case 0x2030: return 0x89; break;
			case 0x0160: return 0x8a; break;
			case 0x2039: return 0x8b; break;
			case 0x0152: return 0x8c; break;
			case 0x017d: return 0x8e; break;
			case 0x2018: return 0x91; break;
			case 0x2019: return 0x92; break;
			case 0x201c: return 0x93; break;
			case 0x201d: return 0x94; break;
			case 0x2022: return 0x95; break;
			case 0x2013: return 0x96; break;
			case 0x2014: return 0x97; break;
			case 0x02dc: return 0x98; break;
			case 0x2122: return 0x99; break;
			case 0x0161: return 0x9a; break;
			case 0x203a: return 0x9b; break;
			case 0x0153: return 0x9c; break;
			case 0x017e: return 0x9e; break;
			case 0x0178: return 0x9f; break;
			default: return ' '; break;
		}
	}
}

void utf8_decode(char *src) {
	uint32_t codep = 0, state = UTF8_ACCEPT;
	char *dst = src;

	while (src && *src) {
		if (!decode(&state, &codep, (uint8_t)*src++)) { if (codep < 0x0300 || codep > 0x036F) *dst++ = UNICODEtoCP1252(codep); }
	}

	*dst = '\0';
}

/****************************************************************************************
 * URL tools
 */

static inline char from_hex(char ch) {
  return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

void url_decode(char *url) {
	char *p, *src = strdup(url);
	for (p = src; *src; url++) {
		*url = *src++;
		if (*url == '%') {
			*url = from_hex(*src++) << 4;
			*url |= from_hex(*src++);
		} else if (*url == '+') {
			*url = ' ';
		}
	}
	*url = '\0';
	free(p);
}

/****************************************************************************************
 * Memory tools
 */

void * malloc_init_external(size_t sz){
	void * ptr=NULL;
	ptr = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if(ptr==NULL){
		ESP_LOGE(TAG,"malloc_init_external:  unable to allocate %d bytes of PSRAM!",sz);
	}
	else {
		memset(ptr,0x00,sz);
	}
	return ptr;
}

void * clone_obj_psram(void * source, size_t source_sz){
	void * ptr=NULL;
	ptr = heap_caps_malloc(source_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if(ptr==NULL){
		ESP_LOGE(TAG,"clone_obj_psram:  unable to allocate %d bytes of PSRAM!",source_sz);
	}
	else {
		memcpy(ptr,source,source_sz);
	}
	return ptr;
}

char * strdup_psram(const char * source){
	void * ptr=NULL;
	size_t source_sz = strlen(source)+1;
	ptr = heap_caps_malloc(source_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if(ptr==NULL){
		ESP_LOGE(TAG,"strdup_psram:  unable to allocate %d bytes of PSRAM! Cannot clone string %s",source_sz,source);
	}
	else {
		memset(ptr,0x00,source_sz);
		strcpy(ptr,source);
	}
	return ptr;
}

/****************************************************************************************
 * Task manager
 */
#define TASK_TLS_INDEX 1

typedef struct {
    StaticTask_t *xTaskBuffer;
    StackType_t *xStack;
} task_context_t;

static void task_cleanup(int index, task_context_t *context) {
    free(context->xTaskBuffer);
    free(context->xStack);
    free(context);    
}

BaseType_t xTaskCreateEXTRAM( TaskFunction_t pvTaskCode,
                            const char * const pcName,
                            configSTACK_DEPTH_TYPE usStackDepth,
                            void *pvParameters,
                            UBaseType_t uxPriority,
                            TaskHandle_t *pxCreatedTask) {
    // create the worker task as a static
    task_context_t *context = calloc(1, sizeof(task_context_t));
    context->xTaskBuffer = (StaticTask_t*) heap_caps_malloc(sizeof(StaticTask_t), (MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
	context->xStack = heap_caps_malloc(usStackDepth,(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    TaskHandle_t handle = xTaskCreateStatic(pvTaskCode, pcName, usStackDepth, pvParameters, uxPriority, context->xStack, context->xTaskBuffer);

    // store context in TCB or free everything in case of failure
    if (!handle) {
        free(context->xTaskBuffer);
        free(context->xStack);
        free(context);    
    } else {
        vTaskSetThreadLocalStoragePointerAndDelCallback( handle, TASK_TLS_INDEX, context, (TlsDeleteCallbackFunction_t) task_cleanup);
    }
    
    if (pxCreatedTask) *pxCreatedTask = handle;
    return handle != NULL ? pdPASS : pdFAIL;
}

void vTaskDeleteEXTRAM(TaskHandle_t xTask) {
    /* At this point we leverage FreeRTOS extension to have callbacks on task deletion. 
     * If not, we need to have here our own deletion implementation that include delayed
     * free for when this is called with NULL (self-deletion)
     */
    vTaskDelete(xTask);
}

/****************************************************************************************
 * URL download
 */

typedef struct {
	void *user_context;
	http_download_cb_t callback;
	size_t max, bytes;
	bool abort;
	uint8_t *data;
	esp_http_client_handle_t client;
} http_context_t;

static void http_downloader(void *arg);
static esp_err_t http_event_handler(esp_http_client_event_t *evt);

void http_download(char *url, size_t max, http_download_cb_t callback, void *context) {
	http_context_t *http_context = (http_context_t*) heap_caps_calloc(sizeof(http_context_t), 1, MALLOC_CAP_SPIRAM);

	esp_http_client_config_t config = {
		.url = url,
		.event_handler = http_event_handler,
		.user_data = http_context,
	};

	http_context->callback = callback;
	http_context->user_context = context;
	http_context->max = max;
	http_context->client = esp_http_client_init(&config);

	xTaskCreateEXTRAM(http_downloader, "downloader", 8*1024, http_context, ESP_TASK_PRIO_MIN + 1, NULL);
}

static void http_downloader(void *arg) {
	http_context_t *http_context = (http_context_t*) arg;

	esp_http_client_perform(http_context->client);
	esp_http_client_cleanup(http_context->client);

	free(http_context);
	vTaskDeleteEXTRAM(NULL);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
	http_context_t *http_context = (http_context_t*) evt->user_data;

	if (http_context->abort) return ESP_FAIL;

	switch(evt->event_id) {
	case HTTP_EVENT_ERROR:
		http_context->callback(NULL, 0, http_context->user_context);
		http_context->abort = true;
		break;
	case HTTP_EVENT_ON_HEADER:
		if (!strcasecmp(evt->header_key, "Content-Length")) {
			size_t len = atoi(evt->header_value);
			if (!len || len > http_context->max) {
				ESP_LOGI(TAG, "content-length null or too large %zu / %zu", len, http_context->max);
				http_context->abort = true;
			}
		}
		break;
	case HTTP_EVENT_ON_DATA: {
		size_t len = esp_http_client_get_content_length(evt->client);
		if (!http_context->data) {
			if ((http_context->data = (uint8_t*) malloc(len)) == NULL) {
				http_context->abort = true;
				ESP_LOGE(TAG, "failed to allocate memory for output buffer %zu", len);
				return ESP_FAIL;
			}
		}
		memcpy(http_context->data + http_context->bytes, evt->data, evt->data_len);
		http_context->bytes += evt->data_len;
		break;
	}
	case HTTP_EVENT_ON_FINISH:
		http_context->callback(http_context->data, http_context->bytes, http_context->user_context);
		break;
	case HTTP_EVENT_DISCONNECTED: {
		int mbedtls_err = 0;
		esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "HTTP download disconnect %d", err);
			if (http_context->data) free(http_context->data);
			http_context->callback(NULL, 0, http_context->user_context);
			return ESP_FAIL;
		}
		break;
	default:
		break;
	}
	}

	return ESP_OK;
}

