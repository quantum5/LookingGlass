/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "interface/platform.h"
#include "interface/capture.h"
#include "dynamic/capture.h"
#include "common/debug.h"
#include "common/option.h"
#include "common/locking.h"
#include "common/KVMFR.h"
#include "common/crash.h"
#include "common/thread.h"
#include "common/ivshmem.h"

#include <lgmp/host.h>

#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN_DN(x) ((uintptr_t)(x) & ~0x7F)
#define ALIGN_UP(x) ALIGN_DN(x + 0x7F)

#define LGMP_Q_POINTER_LEN 10
#define LGMP_Q_FRAME_LEN   2

#define MAX_POINTER_SIZE (sizeof(KVMFRCursor) + (128 * 128 * 4))

struct app
{
  PLGMPHost     lgmp;

  PLGMPHostQueue pointerQueue;
  PLGMPMemory    pointerMemory[LGMP_Q_POINTER_LEN];
  PLGMPMemory    pointerShape;
  bool           pointerShapeValid;
  unsigned int   pointerIndex;

  size_t         maxFrameSize;
  PLGMPHostQueue frameQueue;
  PLGMPMemory    frameMemory[LGMP_Q_FRAME_LEN];
  unsigned int   frameIndex;

  CaptureInterface * iface;

  bool       running;
  bool       reinit;
  LGThread * lgmpThread;
  LGThread * frameThread;
};

static struct app app;

static int lgmpThread(void * opaque)
{
  LGMP_STATUS status;
  while(app.running)
  {
    if ((status = lgmpHostProcess(app.lgmp)) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostProcess Failed: %s", lgmpStatusString(status));
      break;
    }
    usleep(1000);
  }

  app.running = false;
  return 0;
}

static int frameThread(void * opaque)
{
  DEBUG_INFO("Frame thread started");

  bool         frameValid     = false;
  bool         repeatFrame    = false;
  int          frameIndex     = 0;
  CaptureFrame frame          = { 0 };

  (void)frameIndex;
  (void)repeatFrame;

  while(app.running)
  {
    switch(app.iface->waitFrame(&frame))
    {
      case CAPTURE_RESULT_OK:
        repeatFrame = false;
        break;

      case CAPTURE_RESULT_REINIT:
      {
        app.reinit = true;
        DEBUG_INFO("Frame thread reinit");
        return 0;
      }

      case CAPTURE_RESULT_ERROR:
      {
        DEBUG_ERROR("Failed to get the frame");
        return 0;
      }

      case CAPTURE_RESULT_TIMEOUT:
      {
        if (frameValid && lgmpHostQueueNewSubs(app.frameQueue) > 0)
        {
          // resend the last frame
          repeatFrame = true;
          break;
        }

        continue;
      }
    }

    //wait until there is room in the queue
    if (lgmpHostQueuePending(app.frameQueue) == LGMP_Q_FRAME_LEN)
    {
      if (!app.running)
        break;
    }

    // if we are repeating a frame just send the last frame again
    if (repeatFrame)
    {
      lgmpHostQueuePost(app.frameQueue, 0, app.frameMemory[app.frameIndex]);
      continue;
    }

    // we increment the index first so that if we need to repeat a frame
    // the index still points to the latest valid frame
    if (frameIndex++ == LGMP_Q_FRAME_LEN)
      frameIndex = 0;

    KVMFRFrame * fi = lgmpHostMemPtr(app.frameMemory[app.frameIndex]);
    switch(frame.format)
    {
      case CAPTURE_FMT_BGRA  : fi->type = FRAME_TYPE_BGRA  ; break;
      case CAPTURE_FMT_RGBA  : fi->type = FRAME_TYPE_RGBA  ; break;
      case CAPTURE_FMT_RGBA10: fi->type = FRAME_TYPE_RGBA10; break;
      case CAPTURE_FMT_YUV420: fi->type = FRAME_TYPE_YUV420; break;
      default:
        DEBUG_ERROR("Unsupported frame format %d, skipping frame", frame.format);
        continue;
    }

    fi->width   = frame.width;
    fi->height  = frame.height;
    fi->stride  = frame.stride;
    fi->pitch   = frame.pitch;
    frameValid  = true;

    FrameBuffer fb = (FrameBuffer)(fi + 1);
    framebuffer_prepare(fb);

    /* we post and then get the frame, this is intentional! */
    lgmpHostQueuePost(app.frameQueue, 0, app.frameMemory[app.frameIndex]);
    app.iface->getFrame(fb);
  }
  DEBUG_INFO("Frame thread stopped");
  return 0;
}

bool startThreads()
{
  app.running = true;
  if (!lgCreateThread("LGMPThread", lgmpThread, NULL, &app.lgmpThread))
  {
    DEBUG_ERROR("Failed to create the LGMP thread");
    return false;
  }

  if (!lgCreateThread("FrameThread", frameThread, NULL, &app.frameThread))
  {
    DEBUG_ERROR("Failed to create the frame thread");
    return false;
  }

  return true;
}

bool stopThreads()
{
  bool ok = true;

  app.running = false;
  app.iface->stop();

  if (app.frameThread && !lgJoinThread(app.frameThread, NULL))
  {
    DEBUG_WARN("Failed to join the frame thread");
    ok = false;
  }
  app.frameThread = NULL;

  if (app.lgmpThread && !lgJoinThread(app.lgmpThread, NULL))
  {
    DEBUG_WARN("Failed to join the LGMP thread");
    ok = false;
  }
  app.lgmpThread = NULL;

  return ok;
}

static bool captureStart()
{
  DEBUG_INFO("Using            : %s", app.iface->getName());

  const unsigned int maxFrameSize = app.iface->getMaxFrameSize();
  if (maxFrameSize > app.maxFrameSize)
  {
    DEBUG_ERROR("Maximum frame size of %d bytes excceds maximum space available", maxFrameSize);
    return false;
  }
  DEBUG_INFO("Capture Size     : %u MiB (%u)", maxFrameSize / 1048576, maxFrameSize);

  DEBUG_INFO("==== [ Capture  Start ] ====");
  return startThreads();
}

static bool captureRestart()
{
  DEBUG_INFO("==== [ Capture Restart ] ====");
  if (!stopThreads())
    return false;

  if (!app.iface->deinit() || !app.iface->init())
  {
    DEBUG_ERROR("Failed to reinitialize the capture device");
    return false;
  }

  if (!captureStart())
    return false;

  return true;
}

bool captureGetPointerBuffer(void ** data, uint32_t * size)
{
  // spin until there is room
  while(lgmpHostQueuePending(app.pointerQueue) == LGMP_Q_POINTER_LEN)
  {
    DEBUG_INFO("pending");
    if (!app.running)
      return false;
  }

  PLGMPMemory mem = app.pointerMemory[app.pointerIndex];
  *data = ((uint8_t*)lgmpHostMemPtr(mem)) + sizeof(KVMFRCursor);
  *size = MAX_POINTER_SIZE - sizeof(KVMFRCursor);
  return true;
}

void capturePostPointerBuffer(CapturePointer pointer)
{
  PLGMPMemory mem;
  const bool newClient = lgmpHostQueueNewSubs(app.pointerQueue) > 0;

  if (pointer.shapeUpdate || newClient)
  {
    if (pointer.shapeUpdate)
    {
      // swap the latest shape buffer out of rotation
      PLGMPMemory tmp  = app.pointerShape;
      app.pointerShape = app.pointerMemory[app.pointerIndex];
      app.pointerMemory[app.pointerIndex] = tmp;
    }

    // use the last known shape buffer
    mem = app.pointerShape;
  }
  else
  {
    mem = app.pointerMemory[app.pointerIndex];
    if (++app.pointerIndex == LGMP_Q_POINTER_LEN)
      app.pointerIndex = 0;
  }

  KVMFRCursor *cursor = lgmpHostMemPtr(mem);
  cursor->x       = pointer.x;
  cursor->y       = pointer.y;
  cursor->visible = pointer.visible;

  if (pointer.shapeUpdate)
  {
    // remember which slot has the latest shape
    cursor->width  = pointer.width;
    cursor->height = pointer.height;
    cursor->pitch  = pointer.pitch;
    switch(pointer.format)
    {
      case CAPTURE_FMT_COLOR : cursor->type = CURSOR_TYPE_COLOR       ; break;
      case CAPTURE_FMT_MONO  : cursor->type = CURSOR_TYPE_MONOCHROME  ; break;
      case CAPTURE_FMT_MASKED: cursor->type = CURSOR_TYPE_MASKED_COLOR; break;

      default:
        DEBUG_ERROR("Invalid pointer type");
        return;
    }

    app.pointerShapeValid = true;
  }

  const uint32_t sendShape =
    ((pointer.shapeUpdate || newClient) && app.pointerShapeValid) ? 1 : 0;

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(app.pointerQueue, sendShape, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
      continue;

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer): %s", lgmpStatusString(status));
    return;
  }
}

// this is called from the platform specific startup routine
int app_main(int argc, char * argv[])
{
  if (!installCrashHandler(os_getExecutable()))
    DEBUG_WARN("Failed to install the crash handler");

  ivshmemOptionsInit();

  // register capture interface options
  for(int i = 0; CaptureInterfaces[i]; ++i)
    if (CaptureInterfaces[i]->initOptions)
      CaptureInterfaces[i]->initOptions();

  // try load values from a config file
  option_load("looking-glass-host.ini");

  // parse the command line arguments
  if (!option_parse(argc, argv))
  {
    option_free();
    DEBUG_ERROR("Failure to parse the command line");
    return -1;
  }

  if (!option_validate())
  {
    option_free();
    return -1;
  }

  // perform platform specific initialization
  if (!app_init())
    return -1;

  DEBUG_INFO("Looking Glass Host (" BUILD_VERSION ")");

  struct IVSHMEM shmDev;
  if (!ivshmemOpen(&shmDev))
  {
    DEBUG_ERROR("Failed to open the IVSHMEM device");
    return -1;
  }

  int exitcode  = 0;
  DEBUG_INFO("IVSHMEM Size     : %u MiB", shmDev.size / 1048576);
  DEBUG_INFO("IVSHMEM Address  : 0x%" PRIXPTR, (uintptr_t)shmDev.mem);

  LGMP_STATUS status;
  if ((status = lgmpHostInit(shmDev.mem, shmDev.size, &app.lgmp)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostInit Failed: %s", lgmpStatusString(status));
    goto fail;
  }

  if ((status = lgmpHostQueueNew(app.lgmp, LGMP_Q_FRAME, LGMP_Q_FRAME_LEN, &app.frameQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Frame): %s", lgmpStatusString(status));
    goto fail;
  }

  if ((status = lgmpHostQueueNew(app.lgmp, LGMP_Q_POINTER, LGMP_Q_POINTER_LEN, &app.pointerQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueNew Failed (Pointer): %s", lgmpStatusString(status));
    goto fail;
  }

  for(int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
  {
    if ((status = lgmpHostMemAlloc(app.lgmp, MAX_POINTER_SIZE, &app.pointerMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer): %s", lgmpStatusString(status));
      goto fail;
    }
  }

  app.pointerShapeValid = false;
  if ((status = lgmpHostMemAlloc(app.lgmp, MAX_POINTER_SIZE, &app.pointerShape)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer Shape): %s", lgmpStatusString(status));
    goto fail;
  }

  app.maxFrameSize = ALIGN_DN(lgmpHostMemAvail(app.lgmp) / LGMP_Q_FRAME_LEN);
  for(int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if ((status = lgmpHostMemAlloc(app.lgmp, app.maxFrameSize, &app.frameMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Frame): %s", lgmpStatusString(status));
      goto fail;
    }
  }

  DEBUG_INFO("Max Pointer Size : %u KiB", (unsigned int)MAX_POINTER_SIZE / 1024);
  DEBUG_INFO("Max Frame Size   : %u MiB", (unsigned int)(app.maxFrameSize / 1048576LL));

  CaptureInterface * iface = NULL;
  for(int i = 0; CaptureInterfaces[i]; ++i)
  {
    iface = CaptureInterfaces[i];
    DEBUG_INFO("Trying           : %s", iface->getName());

    if (!iface->create(captureGetPointerBuffer, capturePostPointerBuffer))
    {
      iface = NULL;
      continue;
    }

    if (iface->init())
      break;

    iface->free();
    iface = NULL;
  }

  if (!iface)
  {
    DEBUG_ERROR("Failed to find a supported capture interface");
    exitcode = -1;
    goto fail;
  }

  app.iface = iface;

  if (!captureStart())
  {
    exitcode = -1;
    goto exit;
  }

  while(app.running)
  {
    if (app.reinit && !captureRestart())
    {
      exitcode = -1;
      goto exit;
    }
    app.reinit = false;

    switch(iface->capture())
    {
      case CAPTURE_RESULT_OK:
        break;

      case CAPTURE_RESULT_TIMEOUT:
        continue;

      case CAPTURE_RESULT_REINIT:
        if (!captureRestart())
        {
          exitcode = -1;
          goto exit;
        }
        app.reinit = false;
        continue;

      case CAPTURE_RESULT_ERROR:
        DEBUG_ERROR("Capture interface reported a fatal error");
        exitcode = -1;
        goto finish;
    }
  }

finish:
  stopThreads();
exit:

  iface->deinit();
  iface->free();
fail:

  for(int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    lgmpHostMemFree(&app.frameMemory[i]);
  for(int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
    lgmpHostMemFree(&app.pointerMemory[i]);
  lgmpHostMemFree(&app.pointerShape);
  lgmpHostFree(&app.lgmp);

  ivshmemClose(&shmDev);
  return exitcode;
}

void app_quit()
{
  app.running = false;
}