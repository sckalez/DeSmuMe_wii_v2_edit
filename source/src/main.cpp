/*
    Copyright (C) 2006,2007 DeSmuME Team
    Copyright (C) 2007 Pascal Giard (evilynux)
    Copyright (C) 2009 Yoshihiro (DsonPSP)
    Copyright (C) 2012 DeSmuMEWii team

    This file is part of DeSmuMEWii

    DeSmuMEWii is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DeSmuMEWii is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DeSmuMEWii; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include <stdio.h>
#include <unistd.h>
#include <fat.h>
#include <ogcsys.h>
#include <sdcard/wiisd_io.h>
#include <ogc/disc_io.h>
#include <sys/time.h>
#include <wiiuse/wpad.h>
#include <sys/dir.h>
#include <ogc/lwp_watchdog.h>
#include "FileSystem.h"
// #include <dopmii/FileSystem.h>
#include "MMU.h"
#include "NDSSystem.h"
#include "cflash.h"
#include "sndogc.h"
#include "ctrlssdl.h"
#include "GPU.h"
#include "render3D.h"
#include "FrontEnd.h"
#include "version.h"
#include "log_console.h"
#include "GXRender.h"
#include "rasterize.h"
#include "filebrowser.h"

//#include <sdcard/wiisd_io.h>
#include <ogc/usbstorage.h>


#include "gekko_utils/usb2storage.h"
#include "gekko_utils/mload.h"

#define NUM_FRAMES_TO_TIME 60
#define FPS_LIMITER_FRAME_PERIOD 8
#define DEFAULT_FIFO_SIZE (256*1024)

NDS_header * header;

GXRModeObj *rmode = NULL;
Mtx44 perspective;
Mtx GXmodelView2D;
unsigned int *xfb[2]; // Double framebuffer [frameBuffer[fb]]
int currfb;           // Current framebuffer (0 or 1)

static u8 gp_fifo[DEFAULT_FIFO_SIZE] __attribute__((aligned(32)));
static u16 TopScreen[256*192] __attribute__((aligned(32)));
static u16 BottomScreen[256*192] __attribute__((aligned(32)));

static GXTexObj TopTex;
static GXTexObj BottomTex;
static GXTexObj CursorTex;

// TODO: Make this fancier
static u16 CursorData[16] __attribute__((aligned(32))) = {
	0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
	0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
	0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
	0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF
};
static int drawcursor = 1;
static lwp_t vidthread = LWP_THREAD_NULL;
mutex_t vidmutex = LWP_MUTEX_NULL;
static bool abort_thread = false;

static float nds_screen_size_ratio = 1.0f;
static u16 keypad;
static bool quit_game = false;
volatile bool execute = false;
static bool show_console = true;
static int SkipFrame = 0;
static int SkipFrameTracker = 0;
static u32 pad, wpad;

// Which rendering core we are using (SoftRast or GX)
u8 current3Dcore = 1;

SoundInterface_struct *SNDCoreList[] = {
	&SNDDummy,
	//&SNDFile,
	&SNDOGC,
	NULL
};

GPU3DInterface *core3DList[] = {
	&gpu3DNull,
	&gpu3Dgx,
	&gpu3DRasterize,
	NULL
};

//////////////////////////////////////////////////////////////////
////////////////////// FUNCTION PROTOTYPES ///////////////////////
//////////////////////////////////////////////////////////////////

void init();
void ShowCredits();
bool PickDevice();
static void Draw(void);
void ShowFPS();
void DSExec();
void Pause();
static void *draw_thread(void*);
void Execute();
void create_dummy_firmware();
bool CheckBios(bool);

//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////

/*
#ifdef HW_RVL
static bool FindIOS(u32 ios)
{
	s32 ret;
	u32 n;
	
	u64 *titles = NULL;
	u32 num_titles=0;
	
	ret = ES_GetNumTitles(&num_titles);
	if (ret < 0)
		return false;
	
	if(num_titles < 1) 
		return false;
	
	titles = (u64 *)memalign(32, num_titles * sizeof(u64) + 32);
	if (!titles)
		return false;
	
	ret = ES_GetTitles(titles, num_titles);
	if (ret < 0)
	{
		free(titles);
		return false;
	}
	
	for(n=0; n < num_titles; n++)
	{
		if((titles[n] & 0xFFFFFFFF)==ios) 
		{
			free(titles); 
			return true;
		}
	}
	free(titles); 
	return false;
}
#endif */

#ifdef __cplusplus
extern "C"
#endif

int main(int argc, char **argv)
{
	IO::SD OurSD;
	OurSD.Mount();
	IO::USB OurUSB;
	OurUSB.Startup();
	OurUSB.Mount();
	
//	struct armcpu_memory_iface *arm9_memio = &arm9_base_memory_iface;
//	struct armcpu_memory_iface *arm7_memio = &arm7_base_memory_iface;
//	struct armcpu_ctrl_iface *arm9_ctrl_iface;
//	struct armcpu_ctrl_iface *arm7_ctrl_iface;
	char filename[MAX_PATH];
	char *rom_filename = filename;
  
	init();

	log_console_init(rmode, 0, 20, 30, rmode->fbWidth - 40, rmode->xfbHeight - 60);
	log_console_enable_video(true);
	//log_console_enable_log(true);

	VIDEO_WaitVSync();
	
	bool device = PickDevice();
  
	VIDEO_WaitVSync();

	if(!device){
		fatUnmount("sd:/");
		__io_wiisd.shutdown();
		fatMountSimple("sd", &__io_wiisd);
		sprintf(rom_filename, "sd:/DS/ROMS");
	}
	else {
		fatUnmount("usb:/");
		for(int i = 0; i < 11; i++) {
			bool isMounted = fatMountSimple("usb", &__io_usbstorage);
			if (isMounted) break;
			sleep(1);
		}
		sprintf(rom_filename, "usb:/DS/ROMS");
	}

	if(FileBrowser(rom_filename) != 0)
		quit_game = true;
	
	cflash_disk_image_file = NULL;

	printf("Initializing virtual Nintendo DS...\n");

	if (CheckBios(device)) // See if we have external bios files
		printf("Found external BIOS files.  Will Use!\n");
	else 
		printf("No external BIOS files found.\n");

	// Initialize the DS!
	NDS_Init();
	create_dummy_firmware(); // Must do for some games!

	NDS_3D_ChangeCore(current3Dcore);
	printf("Initialization successful!\n");

	enable_sound = true;

	if ( enable_sound) {
		printf("Setting up for sound...\n");
		SPU_Init(SNDCORE_OGC, 768);	// audio samples count is 512 or 1024. Buffer is arg*2. 768*2 = 512*3.
	}
  
	printf("Placing ROM into virtual NDS...\n");
	if (NDS_LoadROM(rom_filename, cflash_disk_image_file) < 0) {
		printf("Error loading ROM\n");
		exit(0);
	}

	execute = true;

	log_console_enable_video(false);
	
	Execute();
	
	exit(0);
}

//////////////////////////////////////////////////////////////////
//////////////////////////// FUNCTIONS ///////////////////////////
//////////////////////////////////////////////////////////////////

void init(){
	u32 xfbHeight;
	f32 yscale;

	GXColor background = {0, 0, 0, 0xff};
	currfb = 0;

	// button initialization
	PAD_Init();
	WPAD_Init();
	VIDEO_Init();

	rmode = VIDEO_GetPreferredMode(NULL);

	switch (rmode->viTVMode >> 2)
	{
		case VI_NTSC: // 480 lines (NTSC 60hz)
			break;
		case VI_PAL: // 576 lines (PAL 50hz)
			rmode = &TVPal576IntDfScale;
			rmode->xfbHeight = 480;
			rmode->viYOrigin = (VI_MAX_HEIGHT_PAL - 480)/2;
			rmode->viHeight = 480;
			break;
		default: // 480 lines (PAL 60Hz)
			break;
	}

	
	WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);
	WPAD_SetVRes(WPAD_CHAN_ALL,rmode->viWidth,rmode->viHeight);
	WPAD_SetIdleTimeout(200);

	VIDEO_Configure(rmode);

	xfb[0] = (u32 *)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[1] = (u32 *)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

	VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
	VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
	VIDEO_SetNextFramebuffer (xfb[0]);

	VIDEO_SetBlack(FALSE);

	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
	else while (VIDEO_GetNextField()) VIDEO_WaitVSync();

	memset(gp_fifo, 0, DEFAULT_FIFO_SIZE);
	GX_Init(gp_fifo, DEFAULT_FIFO_SIZE);

	GX_SetCopyClear(background, GX_MAX_Z24);
 
	// other gx setup
	GX_SetViewport(0,0,rmode->fbWidth,rmode->efbHeight,0,1);
	yscale = GX_GetYScaleFactor(rmode->efbHeight,rmode->xfbHeight);
	xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0,0,rmode->fbWidth,rmode->efbHeight);
	GX_SetDispCopySrc(0,0,rmode->fbWidth,rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth,xfbHeight);
	GX_SetCopyFilter(rmode->aa,rmode->sample_pattern,GX_TRUE,rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering,((rmode->viHeight==2*rmode->xfbHeight)?GX_ENABLE:GX_DISABLE));

	if (rmode->aa)
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	else
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);

	GX_SetCullMode(GX_CULL_NONE);
	GX_CopyDisp(xfb[currfb],GX_TRUE);
	GX_SetDispCopyGamma(GX_GM_1_0);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);

	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);

	guOrtho(perspective,0,479,0,639,0,300);
	GX_LoadProjectionMtx(perspective, GX_ORTHOGRAPHIC);

	guMtxIdentity(GXmodelView2D);
	guMtxTransApply (GXmodelView2D, GXmodelView2D, 0.0F, 0.0F, -5.0F);
	GX_LoadPosMtxImm(GXmodelView2D,GX_PNMTX0);

	GX_SetViewport(0,0,rmode->fbWidth,rmode->efbHeight,0,1);
	GX_InvVtxCache();
	GX_ClearVtxDesc();
	GX_InvalidateTexAll();

	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);

	// In order to render the scene, we are taking all of the 
	// pixels and transforming them into a "texture" for the 
	// two quads that serve as our DS screens.
	GX_InitTexObj(&TopTex, TopScreen, 256, 192, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObj(&BottomTex, BottomScreen, 256, 192, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObj(&CursorTex, CursorData, 4, 4, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);

	memset(TopScreen, 0, 256*192*sizeof(*TopScreen));
	memset(BottomScreen, 0, 256*192*sizeof(*BottomScreen));

	if (vidmutex == LWP_MUTEX_NULL)
		LWP_MutexInit(&vidmutex, false);

	VIDEO_SetBlack(false);
}

#define RGB15_REVERSE(col) ( 0x8000 | (((col) & 0x001F) << 10) | ((col) & 0x03E0)  | (((col) & 0x7C00) >> 10) )

static void DrawFPSOverlay(void)
{
    u16 *sTop = (u16*)&GPU_screen; // top screen buffer pointer
    const int scrw = 256;
    const int scrh = 192;

    // 3x5 font for digits and letters used (bits left-to-right)
    };
    static const unsigned char font3x5_F[5] = {7,4,7,4,4};
    static const unsigned char font3x5_P[5] = {7,5,7,4,4};
    static const unsigned char font3x5_S[5] = {7,4,7,1,7};
    static const unsigned char font3x5_colon[5] = {0,2,0,2,0};

    char txt[16];
    int fpsval = FPS;
    if (fpsval < 0) fpsval = 0;
    if (fpsval > 999) fpsval = 999;
    sprintf(txt, "FPS:%d", fpsval);

    const int char_w = 3;
    const int char_h = 5;
    const int spacing = 1;
    int len = strlen(txt);
    int total_w = len * (char_w + spacing);
    int margin = 4;
    int start_x = scrw - margin - total_w;
    if (start_x < 0) start_x = 0;
    int start_y = 2;

    const u16 color = 0x03E0; // bright green in RGB15

    for (int ci = 0; ci < len; ++ci) {
        char c = txt[ci];
        const unsigned char *glyph = NULL;
        if (c >= '0' && c <= '9') glyph = font3x5_digits[c - '0'];
        else if (c == 'F') glyph = font3x5_F;
        else if (c == 'P') glyph = font3x5_P;
        else if (c == 'S') glyph = font3x5_S;
        else if (c == ':') glyph = font3x5_colon;
        else glyph = NULL;

        int cx = start_x + ci * (char_w + spacing);
        int cy = start_y;
        if (!glyph) continue;

        for (int row = 0; row < char_h; ++row) {
            unsigned char bits = glyph[row];
            int y = cy + row;
            if (y < 0 || y >= scrh) continue;
            u16 *rowptr = sTop + y * scrw;
            for (int col = 0; col < char_w; ++col) {
                if (bits & (1 << (char_w - 1 - col))) {
                    int x = cx + col;
                    if (x < 0 || x >= scrw) continue;
                    rowptr[x] = color;
                }
            }
        }
    }
}

static void Draw(void) {
	// convert to 4x4 textels for GX
	u16 *sTop = (u16*)&GPU_screen;
	u16 *sBottom = sTop+256*192;
	u16 *dTop = TopScreen;
	u16 *dBottom = BottomScreen;
	LWP_MutexLock(vidmutex);

	if (showfps) DrawFPSOverlay();

	for (int y = 0; y < 48; y++) {
		for (int h = 0; h < 4; h++) {
			for (int x = 0; x < 64; x++) {
				for (int w = 0; w < 4; w++) {
					*dTop++ = RGB15_REVERSE(sTop[w]);
					*dBottom++ = RGB15_REVERSE(sBottom[w]);
				}
				dTop+=12;     // next tile
				dBottom+=12;
				sTop+=4;
				sBottom+=4;
			}
			dTop-=1020;     // next line
			dBottom-=1020;
		}
		dTop+=1008;       // next row
		dBottom+=1008;
	}

	DCFlushRange(TopScreen, 256*192*2);
	DCFlushRange(BottomScreen, 256*192*2);

	LWP_MutexUnlock(vidmutex);
	
	return;
}

static bool change_screen_layout = true;

static void do_screen_layout()
{
	change_screen_layout = false;

	if(++screen_layout >= SCREEN_MAX)
		screen_layout = SCREEN_VERT_NORMAL;

	switch(screen_layout)
	{
		case SCREEN_HORI_NORMAL:
			// not scaled

			topX = 	int((rmode->viWidth /2) - ((width*2) / 2));
			topY = 	int((rmode->viHeight /2) - (height / 2));
			bottomX = topX + width;
			bottomY = topY;
			scaley = scalex = 1.0;
			break;

		case SCREEN_HORI_STRETCH:
			//scaled

			scalex = float(rmode->viWidth / (width*2.0f));
			scaley = float(rmode->viHeight / height);
			topX = topY = 0;
			bottomY = 0;
			bottomX = topX + width;
			break;

		case SCREEN_VERT_NORMAL:
			// normal
			topX = 	int((rmode->viWidth / 2) - (width / 2.0f));
			topY = 	int((rmode->viHeight / 2) - ((height * 2.0f) / 2));
			bottomX = topX;
			bottomY = topY+height;
			scaley = scalex = 1.0;
			break;

        case SCREEN_VERT_SEPARATED:
			// normal
			topX =     int((rmode->viWidth / 2) - (width / 2.0f));
			topY =     int((rmode->viHeight / 2) - ((height * 2.0f) / 2) - 24);
			bottomX = topX;
			bottomY = topY+height+48;
			scaley = scalex = 1.0;
			break;

		case SCREEN_VERT_STRETCH:
			// stretched
			topX = topY = 0;
			bottomX = 0;
			scalex = float(rmode->viWidth / (width));
			scaley = float(rmode->viHeight / (height*2));
			bottomY = height;
			break;

		case SCREEN_MAIN_STRETCH:
		case SCREEN_SUB_STRETCH:
			topX = topY = 0;
			bottomX = bottomY = 0;
			scalex = float(rmode->viWidth / (width));
			scaley = float(rmode->viHeight / (height));
			break;

		case SCREEN_MAIN_NORMAL: 
		case SCREEN_SUB_NORMAL:
			topX = bottomX = int((rmode->viWidth /2) - (width / 2));
			topY = bottomY = int((rmode->viHeight /2) - (height / 2));
			scaley = scalex = 1.0;
			break;

		case SCREEN_VERT_SEPARATED_ROT_90:
			topX =    int((rmode->viWidth / 2) - (width / 2.0f));
			topY =    int((rmode->viHeight / 2) - ((height * 2.0f) / 2) - 24);
			bottomX = topX;
			bottomY = topY+height+48;
			scaley = scalex = 1.0;
			break;
	}
};

static void *draw_thread(void*)
{
	while(1)
	{
		if (abort_thread)
			break;

		if(change_screen_layout)	// call it only when necessary.
			do_screen_layout();
		
		LWP_MutexLock(vidmutex);
		
		// Transform for scaling and rotate

		Mtx m, m1, m2, mv;

		guMtxIdentity (m1);
		guMtxScaleApply(m1, m1, scalex, scaley, 1.0f);
		
		guVector axis =(guVector){0, 0, 1};
		guMtxRotAxisDeg (m2, &axis, rotate_angle);
		guMtxConcat(m2, m1, m);

		guMtxTransApply(m, m, 0, 0, 0);
		guMtxConcat (GXmodelView2D, m, mv);

		GX_LoadPosMtxImm (mv, GX_PNMTX0);

		// TOP SCREEN
		if ((screen_layout != SCREEN_SUB_NORMAL) && (screen_layout != SCREEN_SUB_STRETCH))
		{
			GX_LoadTexObj(&TopTex, GX_TEXMAP0);
			GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
				GX_Position2f32(topX, topY);
				GX_TexCoord2f32(0, 0);
				GX_Position2f32(topX, topY+height);
				GX_TexCoord2f32(0, 1);
				GX_Position2f32(topX+width, topY+height);
				GX_TexCoord2f32(1, 1);
				GX_Position2f32(topX+width, topY);
				GX_TexCoord2f32(1, 0);
			GX_End();
		}
		// BOTTOM SCREEN
		if (screen_layout != SCREEN_MAIN_NORMAL && (screen_layout != SCREEN_MAIN_STRETCH))
		{
 
			GX_LoadTexObj(&BottomTex, GX_TEXMAP0);
			GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
				GX_Position2f32(bottomX, bottomY);
				GX_TexCoord2f32(0, 0);
				GX_Position2f32(bottomX, bottomY+height);
				GX_TexCoord2f32(0, 1);
				GX_Position2f32(bottomX+width, bottomY+height);
				GX_TexCoord2f32(1, 1);
				GX_Position2f32(bottomX+width, bottomY);
				GX_TexCoord2f32(1, 0);
			GX_End();

			// CURSOR
			if (drawcursor)
			{
				GX_LoadTexObj(&CursorTex, GX_TEXMAP0);
				GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
					GX_Position2f32(bottomX+mouse.x-5, bottomY+mouse.y-5);
					GX_TexCoord2f32(0, 0);
					GX_Position2f32(bottomX+mouse.x-5, bottomY+mouse.y+5);
					GX_TexCoord2f32(0, 1);
					GX_Position2f32(bottomX+mouse.x+5, bottomY+mouse.y+5);
					GX_TexCoord2f32(1, 1);
					GX_Position2f32(bottomX+mouse.x+5, bottomY+mouse.y-5);
					GX_TexCoord2f32(1, 0);
				GX_End();
			}
		}

		GX_DrawDone();

		currfb ^= 1;

		GX_CopyDisp(xfb[currfb],GX_TRUE);
		VIDEO_SetNextFramebuffer(xfb[currfb]);
		VIDEO_Flush();

		LWP_MutexUnlock(vidmutex);

		VIDEO_WaitVSync();
	}

	return NULL;
}

void Execute() {
	if(vidthread == LWP_THREAD_NULL)
		LWP_CreateThread(&vidthread, draw_thread, NULL, NULL, 0, 67);

	while(!quit_game){
		 
		if(SkipFrameTracker) NDS_SkipNextFrame(); 
	
		DSExec();

		SkipFrameTracker++;
		
		if(SkipFrameTracker > SkipFrame) SkipFrameTracker = 0;
		
	}

	abort_thread = true;
	LWP_MutexDestroy(vidmutex);
	vidmutex = LWP_MUTEX_NULL;
	LWP_JoinThread(vidthread, NULL);
	vidthread = LWP_THREAD_NULL;

	NDS_DeInit();

	GX_AbortFrame();
	GX_Flush();

	VIDEO_Flush();
	VIDEO_WaitVSync();
	VIDEO_SetBlack(true);

	return;
}

// persistent FPS updater — call once per frame (before Draw())
void ShowFPS()
{
    // persistent state across calls
    static u32 fps_frame_counter = 0;
    static u32 fps_accum_ms = 0;
    static u32 fps_last_time_ms = 0;

    // get current time in ms (project already provides gettime() and ticks_to_millisecs)
    u32 now_ms = ticks_to_millisecs(gettime());

    if (fps_last_time_ms == 0) fps_last_time_ms = now_ms;

    // accumulate
    fps_frame_counter++;
    fps_accum_ms += (now_ms - fps_last_time_ms);
    fps_last_time_ms = now_ms;

    // update once per second (or when accumulated >= 1000 ms)
    if (fps_accum_ms >= 1000) {
        if (fps_accum_ms > 0) {
            FPS = (int)((u64)fps_frame_counter * 1000ULL / (u64)fps_accum_ms);
        } else {
            FPS = 0;
        }
        fps_frame_counter = 0;
        fps_accum_ms = 0;
    }
}

void DSExec()
{
	PAD_ScanPads();
	WPAD_ScanPads();

	wpad = WPAD_ButtonsDown(WPAD_CHAN_0);
	pad = PAD_ButtonsDown(0);

	process_ctrls_event(&keypad, nds_screen_size_ratio);

	// Update mouse position and click
	if(mouse.down) {
		NDS_setTouchPos(mouse.x, mouse.y);//ir.x, ir.y
	}
	
	if(mouse.click){ 
		NDS_releaseTouch();
		mouse.click = FALSE;
	}

	update_keypad(keypad);     /* Update keypad */

	if ((wpad & WPAD_BUTTON_1) || (pad & PAD_BUTTON_LEFT))
	{
		show_console = !show_console;
		log_console_enable_video(show_console);
	}
	
	if ((wpad & WPAD_BUTTON_2) || (pad & PAD_BUTTON_UP))
	{
		change_screen_layout = true;
	}
	
	if ((wpad & WPAD_BUTTON_B) || (pad & PAD_BUTTON_RIGHT)){ 
		drawcursor ^= 1;
	}
	
	if (wpad & WPAD_BUTTON_PLUS)
		SkipFrame++;
	
	if (wpad &WPAD_BUTTON_MINUS){
		SkipFrame--;
		
		if(SkipFrame < 0)
			SkipFrame = 0;
	}

	if(	(wpad & WPAD_BUTTON_HOME) || ((pad & PAD_TRIGGER_Z) && (pad  & PAD_TRIGGER_R) && (pad & PAD_TRIGGER_L)) || 
		(wpad & WPAD_CLASSIC_BUTTON_HOME))
		quit_game = true;

	NDS_exec<TRUE>(0);

	// update FPS counters first so Draw() can render the latest value
	if (showfps) ShowFPS();

	// only update when !Frame skip tracker
	if (!SkipFrameTracker) Draw();
}

void Pause(){
	for(;;){
		WPAD_ScanPads();
		if(WPAD_ButtonsDown(WPAD_CHAN_0)&WPAD_BUTTON_A)
			break;
	}
}

bool PickDevice() {
    // Menu item representation using C-style arrays for easy insertion into main.cpp
    struct MenuItem {
        const char* label;      // label text (e.g., "Select Device:")
        const char** opts;      // pointer to array of option strings
        int optCount;           // number of options
        int sel;                // currently selected option index
    };

    // Options for each menu item
    static const char* deviceOpts[]   = { "SD",  "USB" };
    static const char* rendererOpts[] = { "Soft", "GX" };
    static const char* skipOpts[] = {
        "0","1","2","3","4","5","6","7","8","9",
        "10","11","12","13","14","15","16","17","18","19","20"
    };
    static const char* showFpsOpts[] = { "No", "Yes" }; // new Show FPS options

    // Menu items: add more entries here to extend the menu
    static MenuItem menuItems[] = {
        { "Select Device:",   deviceOpts,   2, 0 }, // default SD (sel=0)
        { "Select Renderer:", rendererOpts, 2, 0 }, // default Soft (sel=0)
        { "SkipFrame:",       skipOpts,    21, 0 }, // default 0
        { "Show FPS:",        showFpsOpts,  2, 0 }  // default No (sel=0) -- newly added
    };

    const int menuCount = sizeof(menuItems) / sizeof(menuItems[0]);
    int highlight = 0; // which menu item is highlighted (0..menuCount-1)

    // Initialize defaults (preserve original behavior)
    bool device = false;    // false -> SD, true -> USB
    bool useGX = false;
    current3Dcore = 2; // Soft Raster by default (same as original)

    // Input edge detection
    bool prevLeft = false, prevRight = false, prevUp = false, prevDown = false;
    bool prevA = false, prevB = false;

    // Input cooldown: number of frames to ignore inputs on menu entry
    // (helps avoid accidental double-press when returning from credits)
    const int cooldownFramesInit = 10; // ~10 frames (adjust if needed)
    int cooldownFrames = cooldownFramesInit;

    // Warning flash timer (frames). When >0, show yellow warning line.
    int warnFrames = 0;
    const int warnFramesInit = 120; // ~2 seconds at 60Hz

    while (true) {
        PAD_ScanPads();
        WPAD_ScanPads();

        // Read raw inputs (use the same GetInput calls you already have)
        bool left  = GetInput(LEFT, LEFT, LEFT);
        bool right = GetInput(RIGHT, RIGHT, RIGHT);
        bool up    = GetInput(UP, UP, UP);
        bool down  = GetInput(DOWN, DOWN, DOWN);
        bool a     = GetInput(A, A, A);
        bool b     = GetInput(B, B, B);

        // Render menu (console-style, same as original printf usage)
        // Clear and home cursor (you already used these sequences)
        printf("\x1b[2J");
        printf("\x1b[2;0H");
        printf("Welcome to DeSmuME Wii v2!!!\n\n");

        for (int i = 0; i < menuCount; ++i) {
            MenuItem &mi = menuItems[i];

            if (i == highlight) {
                // Highlighted: print the whole line in green, then reset color
                // \x1b[32m = green, \x1b[0m = reset
                printf("  \x1b[32m%s << %s >>\x1b[0m\n", mi.label, mi.opts[mi.sel]);
            } else {
                // Non-highlighted line (normal color)
                printf("  %s << %s >>\n", mi.label, mi.opts[mi.sel]);
            }
        }

        printf("\nPress B to see the credits.\n");

        // If warning active, print it in yellow below the menu
        if (warnFrames > 0) {
            // \x1b[33m = yellow
            printf("\x1b[33mWarning: USB device not found. Please insert USB or choose SD.\x1b[0m\n");
        }

        // Input handling: if cooldown active, do not change menu state.
        // Also update prev* to current so we don't get a spurious edge after cooldown.
        if (cooldownFrames > 0) {
            // decrement cooldown and set prev states to current to avoid edge triggers
            cooldownFrames--;
            prevLeft = left;
            prevRight = right;
            prevUp = up;
            prevDown = down;
            prevA = a;
            prevB = b;

            // decrement warning timer as well
            if (warnFrames > 0) warnFrames--;
            VIDEO_WaitVSync();
            continue;
        }

        // Normal input handling: Up/Down moves highlight (wrap), Left/Right toggles option for highlighted item.
        if (up && !prevUp) {
            highlight = (highlight - 1 + menuCount) % menuCount;
        }
        if (down && !prevDown) {
            highlight = (highlight + 1) % menuCount;
        }

        if (left && !prevLeft) {
            MenuItem &mi = menuItems[highlight];
            if (mi.optCount > 0) {
                mi.sel = (mi.sel - 1 + mi.optCount) % mi.optCount;
            }
        }
        if (right && !prevRight) {
            MenuItem &mi = menuItems[highlight];
            if (mi.optCount > 0) {
                mi.sel = (mi.sel + 1) % mi.optCount;
            }
        }

        // Apply A: accept selections and exit loop
        if (a && !prevA) {
            // Device selection is menuItems[0].sel: 0 = SD, 1 = USB
            bool wantUSB = (menuItems[0].sel != 0);

            // Renderer selection is menuItems[1].sel: 0 = Soft, 1 = GX
            useGX = (menuItems[1].sel != 0);
            if (useGX) {
                current3Dcore = 1; // Use GX as before
            } else {
                current3Dcore = 2; // Soft raster
            }

            // SkipFrame selection is menuItems[2].sel -> integer 0..20
            // Write to global SkipFrame variable (declared elsewhere)
            SkipFrame = menuItems[2].sel;

            // Wire showfps (frontend.h) from menuItems[3]
            showfps = (menuItems[3].sel != 0);

            if (!wantUSB) {
                // SD chosen: proceed normally
                device = false;
                break;
            } else {
                // USB chosen: attempt a quick mount check before leaving menu.
                // If mount fails, flash a warning and stay in menu.
                bool isMounted = fatMountSimple("usb", &__io_usbstorage);
                if (!isMounted) {
                    // flash warning for a few seconds and do not proceed
                    warnFrames = warnFramesInit;
                    // ensure we don't immediately re-trigger due to held A: set prevA true
                    prevA = true;
                } else {
                    // mounted successfully; proceed
                    device = true;
                    break;
                }
            }
        }

        // B shows credits (same as original)
        if (b && !prevB) {
            ShowCredits();
            // After returning from credits, set a short cooldown to avoid accidental double-press
            cooldownFrames = cooldownFramesInit;
            // Also reset prev states to avoid immediate edge triggers
            prevLeft = prevRight = prevUp = prevDown = prevA = prevB = false;
        }

        // Save previous states for edge detection
        prevLeft  = left;
        prevRight = right;
        prevUp    = up;
        prevDown  = down;
        prevA     = a;
        prevB     = b;

        // decrement warning timer if active
        if (warnFrames > 0) warnFrames--;

        VIDEO_WaitVSync();
    }

    // Return device selection (false = SD, true = USB) to match original signature
    return device;
}

void ShowCredits() {

	printf("\x1b[2J");
	printf("\x1b[2;0H");
	
	printf("DeSmuME Wii\n\n");
	printf("http://code.google.com/p/desmumewii\n\n");
	printf("Written By:\n\n");
	printf("Arikado - http://arikadosblog.blogspot.com\n");
	printf("scanff\n");
	printf("DCN\n");	
	printf("firnis\n");
	printf("baby.lueshi\n");
	printf("With contributions from Cyan\n");
	printf("v2 start by radicalten\n");
	printf("v2 continuation by loki_cx\n\n");

	printf("Press A to return to the menu.");
	
	while(true){ 
	    PAD_ScanPads();
		WPAD_ScanPads();
		if(GetInput(A, A, A))
		    break;
	}

}

//needed for some games
void create_dummy_firmware(){
	// Create the dummy firmware
	NDS_fw_config_data dummy;
	
	NDS_FillDefaultFirmwareConfigData(&dummy);

	NDS_CreateDummyFirmware( &dummy);
}

/*
	As we don't have a menu right now this function is used to see if the user
	has external bios files.  If they do we mark them to be used
*/
bool CheckBios(bool device){
	char path[256] = {0};

	if (!device) strcat(path,"sd:/DS/BIOS/");
	else strcat(path,"usb:/DS/BIOS/");

	FILE* biosfile = 0;

	// Check arm7 bios
	sprintf(CommonSettings.ARM7BIOS,"%sbiosnds7.rom",path);

	biosfile = fopen(CommonSettings.ARM7BIOS,"rb");
	if (!biosfile){
		printf("No ARM7 BIOS\n");
		memset(CommonSettings.ARM7BIOS,0,256);
		return false;		
	}

	fclose(biosfile);
	biosfile = 0;

	// Check arm9 bios
	sprintf(CommonSettings.ARM9BIOS,"%sbiosnds9.rom",path);

	biosfile = fopen(CommonSettings.ARM9BIOS,"rb");
	if (!biosfile){
		printf("No ARM9 BIOS\n");
		memset(CommonSettings.ARM9BIOS,0,256);
		return false;		
	}

	fclose(biosfile);
	biosfile = 0;

	CommonSettings.UseExtBIOS = true;
	
	return true;
}
