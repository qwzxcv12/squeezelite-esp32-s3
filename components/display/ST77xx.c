/**
 * Copyright (c) 2017-2018 Tara Keeling
 *				 2020 Philippe G.
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gds.h"
#include "gds_private.h"

#define SHADOW_BUFFER
#define USE_IRAM
#define PAGE_BLOCK		2048
#define ENABLE_WRITE	0x2c

#define min(a,b) (((a) < (b)) ? (a) : (b))

static char TAG[] = "ST77xx";

enum { ST7735, ST7789 };

struct PrivateSpace {
	uint8_t *iRAM, *Shadowbuffer;
	struct {
		uint16_t Height, Width;
	} Offset;
    uint16_t iRAMSize;
	uint8_t MADCtl, PageSize;
	uint8_t Model;
};

// Functions are not declared to minimize # of lines

static void WriteByte( struct GDS_Device* Device, uint8_t Data ) {
	Device->WriteData( Device, &Data, 1 );
}

static void SetColumnAddress( struct GDS_Device* Device, uint16_t Start, uint16_t End ) {
	uint32_t Addr = __builtin_bswap16(Start) | (__builtin_bswap16(End) << 16);
	Device->WriteCommand( Device, 0x2A );
	Device->WriteData( Device, (uint8_t*) &Addr, 4 );
}

static void SetRowAddress( struct GDS_Device* Device, uint16_t Start, uint16_t End ) {
	uint32_t Addr = __builtin_bswap16(Start) | (__builtin_bswap16(End) << 16);
	Device->WriteCommand( Device, 0x2B );
	Device->WriteData( Device, (uint8_t*) &Addr, 4 );
}

static void Update16( struct GDS_Device* Device ) {
	struct PrivateSpace *Private = (struct PrivateSpace*) Device->Private;
		
#ifdef SHADOW_BUFFER
	uint32_t *optr = (uint32_t*) Private->Shadowbuffer, *iptr = (uint32_t*) Device->Framebuffer;
	int FirstCol = Device->Width / 2, LastCol = 0, FirstRow = -1, LastRow = 0;  
	
	for (int r = 0; r < Device->Height; r++) {
		// look for change and update shadow (cheap optimization = width is always a multiple of 2)
		for (int c = 0; c < Device->Width / 2; c++, iptr++, optr++) {
			if (*optr != *iptr) {
				*optr = *iptr;
				if (c < FirstCol) FirstCol = c;	
				if (c > LastCol) LastCol = c;
				if (FirstRow < 0) FirstRow = r;
				LastRow = r;
			}
		}

		// wait for a large enough window - careful that window size might increase by more than a line at once !
		if (FirstRow < 0 || ((LastCol - FirstCol + 1) * (r - FirstRow + 1) * 4 < PAGE_BLOCK && r != Device->Height - 1)) continue;
		
		FirstCol *= 2;
		LastCol = LastCol * 2 + 1;
		SetRowAddress( Device, FirstRow + Private->Offset.Height, LastRow + Private->Offset.Height);
		SetColumnAddress( Device, FirstCol + Private->Offset.Width, LastCol + Private->Offset.Width );
		Device->WriteCommand( Device, ENABLE_WRITE );
			
		int ChunkSize = (LastCol - FirstCol + 1) * 2;
			
		// own use of IRAM has not proven to be much better than letting SPI do its copy
		if (Private->iRAM) {
			uint8_t *optr = Private->iRAM;
			for (int i = FirstRow; i <= LastRow; i++) {
				memcpy(optr, Private->Shadowbuffer + (i * Device->Width + FirstCol) * 2, ChunkSize);
				optr += ChunkSize;
				if (optr - Private->iRAM <= (Private->iRAMSize - ChunkSize) && i < LastRow) continue;
				Device->WriteData(Device, Private->iRAM, optr - Private->iRAM);
				optr = Private->iRAM;
			}
		} else for (int i = FirstRow; i <= LastRow; i++) {
			Device->WriteData( Device, Private->Shadowbuffer + (i * Device->Width + FirstCol) * 2, ChunkSize );
		}	

		FirstCol = Device->Width / 2; LastCol = 0;
		FirstRow = -1;
	}	
#else
	// always update by full lines
	SetColumnAddress( Device, Private->Offset.Width, Device->Width - 1);
	
	for (int r = 0; r < Device->Height; r += min(Private->PageSize, Device->Height - r)) {
		int Height = min(Private->PageSize, Device->Height - r);
		
		SetRowAddress( Device, Private->Offset.Height + r, Private->Offset.Height + r + Height - 1 );
		Device->WriteCommand(Device, ENABLE_WRITE);
		
		if (Private->iRAM) {
			memcpy(Private->iRAM, Device->Framebuffer + r * Device->Width * 2, Height * Device->Width * 2 );
			Device->WriteData( Device, Private->iRAM, Height * Device->Width * 2 );
		} else	{
			Device->WriteData( Device, Device->Framebuffer + r * Device->Width * 2, Height * Device->Width * 2 );
		}	
	}	
#endif	
}

static void Update24( struct GDS_Device* Device ) {
	struct PrivateSpace *Private = (struct PrivateSpace*) Device->Private;
		
#ifdef SHADOW_BUFFER
	uint16_t *optr = (uint16_t*) Private->Shadowbuffer, *iptr = (uint16_t*) Device->Framebuffer;
	int FirstCol = (Device->Width * 3) / 2, LastCol = 0, FirstRow = -1, LastRow = 0;  
	
	for (int r = 0; r < Device->Height; r++) {
		// look for change and update shadow (cheap optimization = width always / by 2)
		for (int c = 0; c < (Device->Width * 3) / 2; c++, optr++, iptr++) {
			if (*optr != *iptr) {
				*optr = *iptr;
				if (c < FirstCol) FirstCol = c;	
				if (c > LastCol) LastCol = c;
				if (FirstRow < 0) FirstRow = r;
				LastRow = r;
			}
		}
		
		// do we have enough to send (cols are divided by 3/2)
		if (FirstRow < 0 || ((((LastCol - FirstCol + 1) * 2 + 3 - 1) / 3) * (r - FirstRow + 1) * 3 < PAGE_BLOCK && r != Device->Height - 1)) continue;
		
		FirstCol = (FirstCol * 2) / 3;
		LastCol = (LastCol * 2 + 1) / 3; 
		SetRowAddress( Device, FirstRow + Private->Offset.Height, LastRow + Private->Offset.Height);
		SetColumnAddress( Device, FirstCol + Private->Offset.Width, LastCol + Private->Offset.Width );
		Device->WriteCommand( Device, ENABLE_WRITE );
			
		int ChunkSize = (LastCol - FirstCol + 1) * 3;
					
		// own use of IRAM has not proven to be much better than letting SPI do its copy
		if (Private->iRAM) {
			uint8_t *optr = Private->iRAM;
			for (int i = FirstRow; i <= LastRow; i++) {
				memcpy(optr, Private->Shadowbuffer + (i * Device->Width + FirstCol) * 3, ChunkSize);
				optr += ChunkSize;
				if (optr - Private->iRAM <= (Private->iRAMSize - ChunkSize) && i < LastRow) continue;
				Device->WriteData(Device, Private->iRAM, optr - Private->iRAM);
				optr = Private->iRAM;
			}	
		} else for (int i = FirstRow; i <= LastRow; i++) {
			Device->WriteData( Device, Private->Shadowbuffer + (i * Device->Width + FirstCol) * 3, ChunkSize );
		}	

		FirstCol = (Device->Width * 3) / 2; LastCol = 0;
		FirstRow = -1;
	}	
#else
	// always update by full lines
	SetColumnAddress( Device, Private->Offset.Width, Device->Width - 1);
	
	for (int r = 0; r < Device->Height; r += min(Private->PageSize, Device->Height - r)) {
		int Height = min(Private->PageSize, Device->Height - r);
		
		SetRowAddress( Device, Private->Offset.Height + r, Private->Offset.Height + r + Height - 1 );
		Device->WriteCommand(Device, ENABLE_WRITE);
		
		if (Private->iRAM) {
			memcpy(Private->iRAM, Device->Framebuffer + r * Device->Width * 3, Height * Device->Width * 3 );
			Device->WriteData( Device, Private->iRAM, Height * Device->Width * 3 );
		} else	{
			Device->WriteData( Device, Device->Framebuffer + r * Device->Width * 3, Height * Device->Width * 3 );
		}	
	}	
#endif	
}

static void SetLayout( struct GDS_Device* Device, struct GDS_Layout *Layout ) { 
	struct PrivateSpace *Private = (struct PrivateSpace*) Device->Private;
	
	if (Private->Model == ST7789) {
		if (Layout->Rotate) Private->Offset.Width += Layout->HFlip ? 320 - Device->Width : 0;
		else Private->Offset.Height += Layout->HFlip ? 320 - Device->Height : 0;
		Device->WriteCommand( Device, Layout->Invert ? 0x20 : 0x21 );			
	} else {
		Device->WriteCommand( Device, Layout->Invert ? 0x21 : 0x20 );	
	}		

	Private->MADCtl = Layout->HFlip ? (Private->MADCtl | (1 << 7)) : (Private->MADCtl & ~(1 << 7));
	Private->MADCtl = Layout->VFlip ? (Private->MADCtl | (1 << 6)) : (Private->MADCtl & ~(1 << 6));
	Private->MADCtl = Layout->Rotate ? (Private->MADCtl | (1 << 5)) : (Private->MADCtl & ~(1 << 5));
	if (Private->Model == ST7735) {
		Private->MADCtl &= ~(1 << 3);
	} else {
		Private->MADCtl = Layout->ColorSwap ? (Private->MADCtl & ~(1 << 3)) : (Private->MADCtl | (1 << 3));
	}

	Device->WriteCommand( Device, 0x36 );
	WriteByte( Device, Private->MADCtl );

#ifdef SHADOW_BUFFER
	// force a full refresh (almost ...)
	memset(Private->Shadowbuffer, 0xAA, Device->FramebufferSize);
#endif	
}	

static void DisplayOn( struct GDS_Device* Device ) { Device->WriteCommand( Device, 0x29 ); }
static void DisplayOff( struct GDS_Device* Device ) { Device->WriteCommand( Device, 0x28 ); }

static void SetContrast( struct GDS_Device* Device, uint8_t Contrast ) {
	Device->WriteCommand( Device, 0x51 );
	WriteByte( Device, Contrast );
	
	Device->SetContrast = NULL;
	GDS_SetContrast( Device, Contrast );
	Device->SetContrast = SetContrast;
}

static bool Init( struct GDS_Device* Device ) {
	struct PrivateSpace *Private = (struct PrivateSpace*) Device->Private;
	int Depth = (Device->Depth + 8 - 1) / 8;
	
	Private->PageSize = min(8, PAGE_BLOCK / (Device->Width * Depth));

#ifdef SHADOW_BUFFER	
	Private->Shadowbuffer = heap_caps_malloc( Device->FramebufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT );
	if (!Private->Shadowbuffer) {
		Private->Shadowbuffer = malloc( Device->FramebufferSize );
	}
	memset(Private->Shadowbuffer, 0xFF, Device->FramebufferSize);
#endif
#ifdef USE_IRAM
    Private->iRAMSize = (Private->PageSize + 1) * Device->Width * Depth;    
    Private->iRAM = heap_caps_malloc( Private->iRAMSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA );
#endif

	ESP_LOGI(TAG, "ST77xx with bit depth %u, offsets %hu:%hu, page %u, iRAM %p", Device->Depth, Private->Offset.Height, Private->Offset.Width, Private->PageSize, Private->iRAM);
	// Reset + Sleepout + Booster with proper timing
	Device->WriteCommand( Device, 0x01 );
	vTaskDelay(pdMS_TO_TICKS(120));
	Device->WriteCommand( Device, 0x11 );
	vTaskDelay(pdMS_TO_TICKS(150));

	if (Private->Model == ST7735) {
		// FRMCTR1 (Frame rate control)
		Device->WriteCommand( Device, 0xB1 );
		uint8_t frm[] = { 0x01, 0x2C, 0x2D };
		Device->WriteData( Device, frm, sizeof(frm) );
		Device->WriteCommand( Device, 0xB2 );
		Device->WriteData( Device, frm, sizeof(frm) );
		Device->WriteCommand( Device, 0xB3 );
		uint8_t frm3[] = { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D };
		Device->WriteData( Device, frm3, sizeof(frm3) );
		Device->WriteCommand( Device, 0xB4 );
		WriteByte( Device, 0x07 );

		// PWCTR1..5 & VMCTR1 (Power control)
		Device->WriteCommand( Device, 0xC0 );
		uint8_t pw1[] = { 0xA2, 0x02, 0x84 };
		Device->WriteData( Device, pw1, sizeof(pw1) );

		Device->WriteCommand( Device, 0xC1 );
		WriteByte( Device, 0xC5 );

		Device->WriteCommand( Device, 0xC2 );
		uint8_t pw3[] = { 0x0A, 0x00 };
		Device->WriteData( Device, pw3, sizeof(pw3) );

		Device->WriteCommand( Device, 0xC3 );
		uint8_t pw4[] = { 0x8A, 0x2A };
		Device->WriteData( Device, pw4, sizeof(pw4) );

		Device->WriteCommand( Device, 0xC4 );
		uint8_t pw5[] = { 0x8A, 0xEE };
		Device->WriteData( Device, pw5, sizeof(pw5) );

		Device->WriteCommand( Device, 0xC5 );
		WriteByte( Device, 0x0E );

		// Inversion off & Normal mode on
		Device->WriteCommand( Device, 0x20 );
		Device->WriteCommand( Device, 0xE0 );
		uint8_t gm_p[] = {0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
		Device->WriteData( Device, gm_p, sizeof(gm_p) );
		Device->WriteCommand( Device, 0xE1 );
		uint8_t gm_n[] = {0x03,0x1d,0x07,0x06,0x2e,0x2c,0x29,0x2d,0x2e,0x2e,0x37,0x3f,0x00,0x00,0x02,0x10};
		Device->WriteData( Device, gm_n, sizeof(gm_n) );
		Device->WriteCommand( Device, 0x13 );
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	
	// need BGR & Address Mode
	Private->MADCtl = 1 << 3;
	Device->WriteCommand( Device, 0x36 );
	WriteByte( Device, Private->MADCtl );		
	
	// set flip modes & contrast
	GDS_SetContrast( Device, 0xff );
	struct GDS_Layout Layout = { };
	Device->SetLayout( Device, &Layout );
	
	// set screen depth (16/18)
	Device->WriteCommand( Device, 0x3A );
	if (Private->Model == ST7789) WriteByte( Device, Device->Depth == 24 ? 0x066 : 0x55 );
	else WriteByte( Device, Device->Depth == 24 ? 0x06 : 0x05 );
	
	// gone with the wind
	Device->DisplayOn( Device );
	vTaskDelay(pdMS_TO_TICKS(100));
	Device->Update( Device );

	return true;
}	

static const struct GDS_Device ST77xx = {
	.DisplayOn = DisplayOn, .DisplayOff = DisplayOff,
	.SetLayout = SetLayout,
	.Update = Update16, .Init = Init,
	.Mode = GDS_RGB565, .Depth = 16,
	.Alloc = GDS_ALLOC_IRAM_SPI,
};		

struct GDS_Device* ST77xx_Detect(char *Driver, struct GDS_Device* Device) {
	uint8_t Model;
	int Depth;
		
	if (strcasestr(Driver, "ST7735")) Model = ST7735;
	else if (strcasestr(Driver, "ST7789")) Model = ST7789;
	else return NULL;
		
	if (!Device) Device = calloc(1, sizeof(struct GDS_Device));
		
	*Device = ST77xx;	
	sscanf(Driver, "%*[^:]:%u", &Depth);

	struct PrivateSpace* Private = (struct PrivateSpace*) Device->Private;
	Private->Model = Model;

	sscanf(Driver, "%*[^:]%*[^x]%*[^=]=%hu", &Private->Offset.Height);		
	sscanf(Driver, "%*[^:]%*[^y]%*[^=]=%hu", &Private->Offset.Width);		
	
	if (Depth == 18) {
		Device->Mode = GDS_RGB666;
		Device->Depth = 24;
		Device->Update = Update24;
	} 	
	
	if (Model == ST7789) Device->SetContrast = SetContrast;

	return Device;
}