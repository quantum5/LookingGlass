/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "interface/capture.h"
#include "interface/platform.h"
#include "common/windebug.h"
#include "windows/mousehook.h"
#include "windows/force_compose.h"
#include "common/option.h"
#include "common/framebuffer.h"
#include "common/event.h"
#include "common/thread.h"
#include "common/dpi.h"
#include "common/KVMFR.h"
#include <assert.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <windows.h>

#include <NvFBC/nvFBC.h>
#include "wrapper.h"

#define DIFF_MAP_DIM(x) ((x + 127) / 128)

struct FrameInfo
{
  unsigned int width;
  unsigned int height;
  bool wasFresh;
  uint8_t * diffMap;
};

struct iface
{
  bool        stop;
  NvFBCHandle nvfbc;

  bool                       seperateCursor;
  CaptureGetPointerBuffer    getPointerBufferFn;
  CapturePostPointerBuffer   postPointerBufferFn;
  LGThread                 * pointerThread;

  unsigned int maxWidth , maxHeight;
  unsigned int width    , height;
  unsigned int dpi;

  unsigned int formatVer;
  unsigned int grabWidth, grabHeight, grabStride;

  uint8_t * frameBuffer;
  uint8_t * diffMap;

  NvFBCFrameGrabInfo grabInfo;

  LGEvent * frameEvent;
  LGEvent * cursorEvent;

  int mouseX, mouseY, mouseHotX, mouseHotY;
  bool mouseVisible, hasMousePosition;

  bool mouseHookCreated;
  bool forceCompositionCreated;

  struct FrameInfo frameInfo[LGMP_Q_FRAME_LEN];
};

static struct iface * this = NULL;

static bool nvfbc_deinit(void);
static void nvfbc_free(void);
static int pointerThread(void * unused);

static void getDesktopSize(unsigned int * width, unsigned int * height, unsigned int * dpi)
{
  HMONITOR    monitor     = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO monitorInfo = {
    .cbSize = sizeof(MONITORINFO)
  };

  GetMonitorInfo(monitor, &monitorInfo);
  *dpi = monitor_dpi(monitor);

  *width  = monitorInfo.rcMonitor.right  - monitorInfo.rcMonitor.left;
  *height = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
}

static void on_mouseMove(int x, int y)
{
  this->hasMousePosition = true;
  this->mouseX           = x;
  this->mouseY           = y;

  const CapturePointer pointer =
  {
    .positionUpdate = true,
    .visible        = this->mouseVisible,
    .x              = x - this->mouseHotX,
    .y              = y - this->mouseHotY
  };

  this->postPointerBufferFn(pointer);
}

static const char * nvfbc_getName(void)
{
  return "NVFBC (NVidia Frame Buffer Capture)";
};

static void nvfbc_initOptions(void)
{
  struct Option options[] =
  {
    {
      .module         = "nvfbc",
      .name           = "decoupleCursor",
      .description    = "Capture the cursor seperately",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = true
    },
    {0}
  };

  option_register(options);
}

static bool nvfbc_create(
    CaptureGetPointerBuffer  getPointerBufferFn,
    CapturePostPointerBuffer postPointerBufferFn)
{
  if (!NvFBCInit())
    return false;

  this = (struct iface *)calloc(sizeof(struct iface), 1);
  this->frameEvent = lgCreateEvent(true, 17);
  if (!this->frameEvent)
  {
    DEBUG_ERROR("failed to create the frame event");
    nvfbc_free();
    return false;
  }

  this->seperateCursor      = option_get_bool("nvfbc", "decoupleCursor");
  this->getPointerBufferFn  = getPointerBufferFn;
  this->postPointerBufferFn = postPointerBufferFn;

  return true;
}

static bool nvfbc_init(void)
{
  this->stop = false;

  int       bufferLen   = GetEnvironmentVariable("NVFBC_PRIV_DATA", NULL, 0);
  uint8_t * privData    = NULL;
  int       privDataLen = 0;

  if (bufferLen)
  {
    char * buffer = malloc(bufferLen);
    GetEnvironmentVariable("NVFBC_PRIV_DATA", buffer, bufferLen);

    privDataLen = (bufferLen - 1) / 2;
    privData    = (uint8_t *)malloc(privDataLen);
    char hex[3] = {0};
    for (int i = 0; i < privDataLen; ++i)
    {
      memcpy(hex, &buffer[i*2], 2);
      privData[i] = (uint8_t)strtoul(hex, NULL, 16);
    }

    free(buffer);
  }

  // NOTE: Calling this on hardware that doesn't support NvFBC such as GeForce
  // causes a substantial performance pentalty even if it fails! As such we only
  // attempt NvFBC as a last resort, or if configured via the app:capture
  // option.
  if (!NvFBCToSysCreate(privData, privDataLen, &this->nvfbc, &this->maxWidth, &this->maxHeight))
  {
    free(privData);
    return false;
  }
  free(privData);

  getDesktopSize(&this->width, &this->height, &this->dpi);
  lgResetEvent(this->frameEvent);

  HANDLE event;
  if (!NvFBCToSysSetup(
    this->nvfbc,
    BUFFER_FMT_ARGB,
    !this->seperateCursor,
    this->seperateCursor,
    true,
    DIFFMAP_BLOCKSIZE_128X128,
    (void **)&this->frameBuffer,
    (void **)&this->diffMap,
    &event
  ))
  {
    return false;
  }

  if (this->seperateCursor)
    this->cursorEvent = lgWrapEvent(event);

  if (!this->mouseHookCreated)
  {
    mouseHook_install(on_mouseMove);
    this->mouseHookCreated = true;
  }

  if (!this->forceCompositionCreated)
  {
    dwmForceComposition();
    this->forceCompositionCreated = true;
  }

  DEBUG_INFO("Cursor mode      : %s", this->seperateCursor ? "decoupled" : "integrated");

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    this->frameInfo[i].width    = 0;
    this->frameInfo[i].height   = 0;
    this->frameInfo[i].wasFresh = false;
    this->frameInfo[i].diffMap  = malloc(DIFF_MAP_DIM(this->maxWidth) * DIFF_MAP_DIM(this->maxHeight));
    if (!this->frameInfo[i].diffMap)
    {
      DEBUG_ERROR("Failed to allocate memory for diffMaps");
      nvfbc_deinit();
      return false;
    }
  }

  Sleep(100);

  if (!lgCreateThread("NvFBCPointer", pointerThread, NULL, &this->pointerThread))
  {
    DEBUG_ERROR("Failed to create the NvFBCPointer thread");
    nvfbc_deinit();
    return false;
  }

  ++this->formatVer;
  return true;
}

static void nvfbc_stop(void)
{
  this->stop = true;

  lgSignalEvent(this->cursorEvent);
  lgSignalEvent(this->frameEvent);

  if (this->pointerThread)
  {
    lgJoinThread(this->pointerThread, NULL);
    this->pointerThread = NULL;
  }
}

static bool nvfbc_deinit(void)
{
  this->cursorEvent = NULL;

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    free(this->frameInfo[i].diffMap);
    this->frameInfo[i].diffMap = NULL;
  }

  if (this->nvfbc)
  {
    NvFBCToSysRelease(&this->nvfbc);
    this->nvfbc = NULL;
  }

  return true;
}

static void nvfbc_free(void)
{
  if (this->frameEvent)
    lgFreeEvent(this->frameEvent);

  if (this->mouseHookCreated)
    mouseHook_remove();

  if (this->forceCompositionCreated)
    dwmUnforceComposition();

  free(this);
  this = NULL;
  NvFBCFree();
}

static unsigned int nvfbc_getMouseScale(void)
{
  return this->dpi * 100 / DPI_100_PERCENT;
}

static CaptureResult nvfbc_capture(void)
{
  getDesktopSize(&this->width, &this->height, &this->dpi);
  NvFBCFrameGrabInfo grabInfo;
  CaptureResult result = NvFBCToSysCapture(
    this->nvfbc,
    1000,
    0, 0,
    this->width,
    this->height,
    &grabInfo
  );

  if (result != CAPTURE_RESULT_OK)
    return result;

  bool changed = false;
  const unsigned int h = DIFF_MAP_DIM(this->height);
  const unsigned int w = DIFF_MAP_DIM(this->width);
  for (unsigned int y = 0; y < h; ++y)
    for (unsigned int x = 0; x < w; ++x)
      if (this->diffMap[(y*w)+x])
      {
        changed = true;
        goto done;
      }

done:
  if (!changed)
    return CAPTURE_RESULT_TIMEOUT;

  memcpy(&this->grabInfo, &grabInfo, sizeof(grabInfo));
  lgSignalEvent(this->frameEvent);
  return CAPTURE_RESULT_OK;
}

struct DisjointSet {
  int id;
  int x1;
  int y1;
  int x2;
  int y2;
};

static int dsFind(struct DisjointSet * ds, int id)
{
  if (ds[id].id != id)
    ds[id].id = dsFind(ds, ds[id].id);
  return ds[id].id;
}

static void dsUnion(struct DisjointSet * ds, int a, int b)
{
  a = dsFind(ds, a);
  b = dsFind(ds, b);
  if (a == b)
    return;

  ds[b].id = a;
  ds[a].x1 = min(ds[a].x1, ds[b].x1);
  ds[a].x2 = max(ds[a].x2, ds[b].x2);
  ds[a].y1 = min(ds[a].y1, ds[b].y1);
  ds[a].y2 = max(ds[a].y2, ds[b].y2);
}

static void updateDamageRects(CaptureFrame * frame)
{
  const unsigned int h = DIFF_MAP_DIM(this->height);
  const unsigned int w = DIFF_MAP_DIM(this->width);

  struct DisjointSet ds[w * h];

  for (unsigned int y = 0; y < h; ++y)
    for (unsigned int x = 0; x < w; ++x)
      if (this->diffMap[y * w + x])
      {
        ds[y * w + x].id = y * w + x;
        ds[y * w + x].x1 = ds[y * w + x].x2 = x;
        ds[y * w + x].y1 = ds[y * w + x].y2 = y;

        if (y > 0 && this->diffMap[(y-1) * w + x])
          dsUnion(ds, (y-1) * w + x, y * w + x);

        if (x > 0 && this->diffMap[y * w + x - 1])
          dsUnion(ds, y * w + x, y * w + x - 1);
      }

  int rectId = 0;
  for (unsigned int y = 0; y < h; ++y)
    for (unsigned int x = 0; x < w; ++x)
      if (this->diffMap[y * w + x] && ds[y * w + x].id == y * w + x)
      {
        if (rectId >= KVMFR_MAX_DAMAGE_RECTS)
        {
          rectId = 0;
          goto done;
        }

        frame->damageRects[rectId++] = (FrameDamageRect) {
          .x = ds[y * w + x].x1 * 128,
          .y = ds[y * w + x].y1 * 128,
          .width = (ds[y * w + x].x2 - ds[y * w + x].x1 + 1) * 128,
          .height = (ds[y * w + x].y2 - ds[y * w + x].y1 + 1) * 128,
        };
      }

done:
  frame->damageRectsCount = rectId;
}

static CaptureResult nvfbc_waitFrame(CaptureFrame * frame,
    const size_t maxFrameSize)
{
  if (!lgWaitEvent(this->frameEvent, 1000))
    return CAPTURE_RESULT_TIMEOUT;

  if (this->stop)
    return CAPTURE_RESULT_REINIT;

  if (
    this->grabInfo.dwWidth       != this->grabWidth  ||
    this->grabInfo.dwHeight      != this->grabHeight ||
    this->grabInfo.dwBufferWidth != this->grabStride)
  {
    this->grabWidth  = this->grabInfo.dwWidth;
    this->grabHeight = this->grabInfo.dwHeight;
    this->grabStride = this->grabInfo.dwBufferWidth;
    ++this->formatVer;
  }

  const unsigned int maxHeight = maxFrameSize / (this->grabStride * 4);

  frame->formatVer  = this->formatVer;
  frame->width      = this->grabWidth;
  frame->height     = maxHeight > this->grabHeight ? this->grabHeight : maxHeight;
  frame->realHeight = this->grabHeight;
  frame->pitch      = this->grabStride * 4;
  frame->stride     = this->grabStride;
  frame->rotation   = CAPTURE_ROT_0;

  updateDamageRects(frame);

#if 0
  //NvFBC never sets bIsHDR so instead we check for any data in the alpha channel
  //If there is data, it's HDR. This is clearly suboptimal
  if (!this->grabInfo.bIsHDR)
    for(int y = 0; y < frame->height; ++y)
      for(int x = 0; x < frame->width; ++x)
      {
        int offset = (y * frame->pitch) + (x * 4);
        if (this->frameBuffer[offset + 3])
        {
          this->grabInfo.bIsHDR = 1;
          break;
        }
      }
#endif

  frame->format = this->grabInfo.bIsHDR ? CAPTURE_FMT_RGBA10 : CAPTURE_FMT_BGRA;
  return CAPTURE_RESULT_OK;
}

inline static void rectCopyAligned(uint8_t * dest, const uint8_t * src, int ystart,
    int yend, int dx, int stride, int width)
{
  const int cols = width / 16;
  assert(width % 16 == 0);
  assert((yend - ystart) % 16 == 0);

  for (int i = ystart; i < yend; i += 16)
  {
    unsigned int offset0  = (i +  0) * stride + dx;
    unsigned int offset1  = (i +  1) * stride + dx;
    unsigned int offset2  = (i +  2) * stride + dx;
    unsigned int offset3  = (i +  3) * stride + dx;
    unsigned int offset4  = (i +  4) * stride + dx;
    unsigned int offset5  = (i +  5) * stride + dx;
    unsigned int offset6  = (i +  6) * stride + dx;
    unsigned int offset7  = (i +  7) * stride + dx;
    unsigned int offset8  = (i +  8) * stride + dx;
    unsigned int offset9  = (i +  9) * stride + dx;
    unsigned int offset10 = (i + 10) * stride + dx;
    unsigned int offset11 = (i + 11) * stride + dx;
    unsigned int offset12 = (i + 12) * stride + dx;
    unsigned int offset13 = (i + 13) * stride + dx;
    unsigned int offset14 = (i + 14) * stride + dx;
    unsigned int offset15 = (i + 15) * stride + dx;

    for (int j = 0; j < cols; ++j)
    {
      __m128i xmm0  = _mm_stream_load_si128((__m128i *)(src + offset0));
      __m128i xmm1  = _mm_stream_load_si128((__m128i *)(src + offset1));
      __m128i xmm2  = _mm_stream_load_si128((__m128i *)(src + offset2));
      __m128i xmm3  = _mm_stream_load_si128((__m128i *)(src + offset3));
      __m128i xmm4  = _mm_stream_load_si128((__m128i *)(src + offset4));
      __m128i xmm5  = _mm_stream_load_si128((__m128i *)(src + offset5));
      __m128i xmm6  = _mm_stream_load_si128((__m128i *)(src + offset6));
      __m128i xmm7  = _mm_stream_load_si128((__m128i *)(src + offset7));
      __m128i xmm8  = _mm_stream_load_si128((__m128i *)(src + offset8));
      __m128i xmm9  = _mm_stream_load_si128((__m128i *)(src + offset9));
      __m128i xmm10 = _mm_stream_load_si128((__m128i *)(src + offset10));
      __m128i xmm11 = _mm_stream_load_si128((__m128i *)(src + offset11));
      __m128i xmm12 = _mm_stream_load_si128((__m128i *)(src + offset12));
      __m128i xmm13 = _mm_stream_load_si128((__m128i *)(src + offset13));
      __m128i xmm14 = _mm_stream_load_si128((__m128i *)(src + offset14));
      __m128i xmm15 = _mm_stream_load_si128((__m128i *)(src + offset15));

      _mm_store_si128((__m128i *)(dest + offset0),  xmm0);
      _mm_store_si128((__m128i *)(dest + offset1),  xmm1);
      _mm_store_si128((__m128i *)(dest + offset2),  xmm2);
      _mm_store_si128((__m128i *)(dest + offset3),  xmm3);
      _mm_store_si128((__m128i *)(dest + offset4),  xmm4);
      _mm_store_si128((__m128i *)(dest + offset5),  xmm5);
      _mm_store_si128((__m128i *)(dest + offset6),  xmm6);
      _mm_store_si128((__m128i *)(dest + offset7),  xmm7);
      _mm_store_si128((__m128i *)(dest + offset8),  xmm8);
      _mm_store_si128((__m128i *)(dest + offset9),  xmm9);
      _mm_store_si128((__m128i *)(dest + offset10), xmm10);
      _mm_store_si128((__m128i *)(dest + offset11), xmm11);
      _mm_store_si128((__m128i *)(dest + offset12), xmm12);
      _mm_store_si128((__m128i *)(dest + offset13), xmm13);
      _mm_store_si128((__m128i *)(dest + offset14), xmm14);
      _mm_store_si128((__m128i *)(dest + offset15), xmm15);

      offset0  += 16;
      offset1  += 16;
      offset2  += 16;
      offset3  += 16;
      offset4  += 16;
      offset5  += 16;
      offset6  += 16;
      offset7  += 16;
      offset8  += 16;
      offset9  += 16;
      offset10 += 16;
      offset11 += 16;
      offset12 += 16;
      offset13 += 16;
      offset14 += 16;
      offset15 += 16;
    }
  }
}

inline static void rectCopyUnaligned(uint8_t * dest, uint8_t * src, int ystart,
    int yend, int dx, int stride, int width)
{
  for (int i = ystart; i < yend; ++i)
  {
    unsigned int offset = i * stride + dx;
    memcpy(dest + offset, src + offset, width);
  }
}

static CaptureResult nvfbc_getFrame(FrameBuffer * frame,
    const unsigned int height, int frameIndex)
{
  const unsigned int h = DIFF_MAP_DIM(this->height);
  const unsigned int w = DIFF_MAP_DIM(this->width);
  uint8_t * frameData = framebuffer_get_data(frame);
  struct FrameInfo * info = this->frameInfo + frameIndex;

  if (info->width == this->grabWidth && info->height == this->grabHeight)
  {
    const bool wasFresh = info->wasFresh;

    for (unsigned int y = 0; y < h; ++y)
    {
      const unsigned int ystart = y * 128;
      const unsigned int yend = min(height, (y + 1) * 128);

      for (unsigned int x = 0; x < w; )
      {
        if ((wasFresh || !info->diffMap[y * w + x]) && !this->diffMap[y * w + x])
        {
          ++x;
          continue;
        }

        unsigned int x2 = x;
        while (x2 < w && ((!wasFresh && info->diffMap[y * w + x2]) || this->diffMap[y * w + x2]))
          ++x2;

        unsigned int width = (min(x2 * 128, this->grabStride) - x * 128) * 4;
        rectCopyUnaligned(frameData, this->frameBuffer, ystart, yend & ~0xF, x * 512,
            this->grabStride * 4, width);

        if (__builtin_expect(yend & 0xF, 0))
          rectCopyUnaligned(frameData, this->frameBuffer, yend & ~0xF, yend, x * 512,
            this->grabStride * 4, width);

        x = x2;
      }
      framebuffer_set_write_ptr(frame, yend * this->grabStride * 4);
    }
  }
  else
    framebuffer_write(
      frame,
      this->frameBuffer,
      height * this->grabInfo.dwBufferWidth * 4
    );

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if (i == frameIndex)
    {
      this->frameInfo[i].width    = this->grabWidth;
      this->frameInfo[i].height   = this->grabHeight;
      this->frameInfo[i].wasFresh = true;
    }
    else if (this->frameInfo[i].width == this->grabWidth &&
          this->frameInfo[i].height == this->grabHeight)
    {
      if (this->frameInfo[i].wasFresh)
      {
        memcpy(this->frameInfo[i].diffMap, this->diffMap, h * w);
        this->frameInfo[i].wasFresh = false;
      }
      else
      {
        for (unsigned int j = 0; j < h * w; ++j)
          this->frameInfo[i].diffMap[j] |= this->diffMap[j];
      }
    }
    else
    {
      this->frameInfo[i].width  = 0;
      this->frameInfo[i].height = 0;
    }
  }
  return CAPTURE_RESULT_OK;
}

static int pointerThread(void * unused)
{
  while(!this->stop)
  {
    if (!lgWaitEvent(this->cursorEvent, 1000))
      continue;

    if (this->stop)
      break;

    CaptureResult  result;
    CapturePointer pointer = { 0 };

    void * data;
    uint32_t size;
    if (!this->getPointerBufferFn(&data, &size))
    {
      DEBUG_WARN("failed to get a pointer buffer");
      continue;
    }

    result = NvFBCToSysGetCursor(this->nvfbc, &pointer, data, size);
    if (result != CAPTURE_RESULT_OK)
    {
      DEBUG_WARN("NvFBCToSysGetCursor failed");
      continue;
    }

    this->mouseVisible = pointer.visible;
    this->mouseHotX    = pointer.hx;
    this->mouseHotY    = pointer.hy;

    pointer.positionUpdate = true;
    pointer.visible        = this->mouseVisible;
    pointer.x              = this->mouseX - pointer.hx;
    pointer.y              = this->mouseY - pointer.hy;

    this->postPointerBufferFn(pointer);
  }

  return 0;
}

struct CaptureInterface Capture_NVFBC =
{
  .shortName       = "NvFBC",
  .getName         = nvfbc_getName,
  .initOptions     = nvfbc_initOptions,

  .create          = nvfbc_create,
  .init            = nvfbc_init,
  .stop            = nvfbc_stop,
  .deinit          = nvfbc_deinit,
  .free            = nvfbc_free,
  .getMouseScale   = nvfbc_getMouseScale,
  .capture         = nvfbc_capture,
  .waitFrame       = nvfbc_waitFrame,
  .getFrame        = nvfbc_getFrame
};
