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

#include "wrapper.h"
#include "common/windebug.h"
#include <windows.h>
#include <NvFBC/nvFBCCuda.h>

#ifdef _WIN64
#define NVFBC_DLL "NvFBC64.dll"
#else
#define NVFBC_DLL "NvFBC.dll"
#endif

struct NVAPI
{
  bool initialized;
  HMODULE dll;

  NvFBC_CreateFunctionExType      createEx;
  NvFBC_SetGlobalFlagsType        setGlobalFlags;
  NvFBC_GetStatusExFunctionType   getStatusEx;
  NvFBC_EnableFunctionType        enable;
  NvFBC_GetSDKVersionFunctionType getVersion;
};

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef uintptr_t CUdeviceptr;

#define CUDA_SUCCESS 0

struct CUDA
{
  bool initialized;
  HMODULE dll;

  CUresult (*cuInit)(unsigned int Flags);
  CUresult (*cuCtxGetCurrent)(CUcontext *pctx);
  CUresult (*cuCtxSetCurrent)(CUcontext ctx);
  CUresult (*cuMemAlloc_v2)(CUdeviceptr *dptr, size_t bytesize);
  CUresult (*cuMemFree_v2)(CUdeviceptr dptr);
  CUresult (*cuMemcpyHtoD_v2)(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount);
  CUresult (*cuMemcpyDtoH_v2)(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount);
  CUresult (*cuMemHostRegister_v2)(void *p, size_t bytesize, unsigned int Flags);
  CUresult (*cuMemHostUnregister)(void *p);
};

struct stNvFBCHandle
{
  NvFBCCuda * nvfbc;
  HANDLE      cursorEvent;
  int         retry;
  CUcontext   context;
  CUdeviceptr buffer;
};

static NVAPI nvapi;
static CUDA cuda;

bool NvFBCInit()
{
  if (nvapi.initialized)
    return true;

  nvapi.dll = LoadLibraryA(NVFBC_DLL);
  if (!nvapi.dll)
  {
    DEBUG_WINERROR("Failed to load " NVFBC_DLL, GetLastError());
    return false;
  }

  nvapi.createEx       = (NvFBC_CreateFunctionExType     )GetProcAddress(nvapi.dll, "NvFBC_CreateEx"      );
  nvapi.setGlobalFlags = (NvFBC_SetGlobalFlagsType       )GetProcAddress(nvapi.dll, "NvFBC_SetGlobalFlags");
  nvapi.getStatusEx    = (NvFBC_GetStatusExFunctionType  )GetProcAddress(nvapi.dll, "NvFBC_GetStatusEx"   );
  nvapi.enable         = (NvFBC_EnableFunctionType       )GetProcAddress(nvapi.dll, "NvFBC_Enable"        );
  nvapi.getVersion     = (NvFBC_GetSDKVersionFunctionType)GetProcAddress(nvapi.dll, "NvFBC_GetSDKVersion" );

  if (
    !nvapi.createEx       ||
    !nvapi.setGlobalFlags ||
    !nvapi.getStatusEx    ||
    !nvapi.enable         ||
    !nvapi.getVersion)
  {
    DEBUG_ERROR("Failed to get the required proc addresses");
    return false;
  }

  cuda.dll = LoadLibraryA("nvcuda.dll");
  if (!cuda.dll)
  {
    DEBUG_WINERROR("Failed to load nvcuda.dll", GetLastError());
    return false;
  }

#define LOAD_CUDA_FUNC(x) cuda.x = (decltype(cuda.x)) GetProcAddress(cuda.dll, #x); \
  if (!cuda.x) { DEBUG_ERROR("Failed to load " #x); return false; }
  LOAD_CUDA_FUNC(cuInit);
  LOAD_CUDA_FUNC(cuCtxGetCurrent);
  LOAD_CUDA_FUNC(cuCtxSetCurrent);
  LOAD_CUDA_FUNC(cuMemAlloc_v2);
  LOAD_CUDA_FUNC(cuMemFree_v2);
  LOAD_CUDA_FUNC(cuMemcpyHtoD_v2);
  LOAD_CUDA_FUNC(cuMemcpyDtoH_v2);
  LOAD_CUDA_FUNC(cuMemHostRegister_v2);
  LOAD_CUDA_FUNC(cuMemHostUnregister);
#undef LOAD_CUDA_FUNC

  CUresult status;
  if ((status = cuda.cuInit(0)) != CUDA_SUCCESS)
  {
    DEBUG_ERROR("Failed to initialize CUDA: %d", status);
    return false;
  }

  NvU32 version;
  if (nvapi.getVersion(&version) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to get the NvFBC SDK version");
    return false;
  }

  DEBUG_INFO("NvFBC SDK Version: %lu", version);

  if (nvapi.enable(NVFBC_STATE_ENABLE) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to enable the NvFBC interface");
    return false;
  }

  nvapi.initialized = true;
  return true;
}

void NvFBCFree()
{
  if (!nvapi.initialized)
    return;

  FreeLibrary(nvapi.dll);
  FreeLibrary(cuda.dll);
  nvapi.initialized = false;
}

bool NvFBCCudaCreate(
  void         * privData,
  unsigned int   privDataSize,
  NvFBCHandle  * handle,
  unsigned int * maxWidth,
  unsigned int * maxHeight
)
{
  NvFBCCreateParams params = {0};

  params.dwVersion         = NVFBC_CREATE_PARAMS_VER;
  params.dwInterfaceType   = NVFBC_SHARED_CUDA;
  params.pDevice           = NULL;
  params.dwAdapterIdx      = 0;
  params.dwPrivateDataSize = privDataSize;
  params.pPrivateData      = privData;

  if (nvapi.createEx(&params) != NVFBC_SUCCESS || params.pNvFBC == NULL)
  {
    *handle = NULL;
    return false;
  }

  NvFBCCuda *nvfbc = static_cast<NvFBCCuda *>(params.pNvFBC);
  DWORD maxBufferSize;
  if (nvfbc->NvFBCCudaGetMaxBufferSize(&maxBufferSize) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to obtain max NvFBC frame size");
    return false;
  }

  CUcontext context;
  CUdeviceptr buffer;
  CUresult result;

  if ((result = cuda.cuCtxGetCurrent(&context)) != CUDA_SUCCESS)
  {
    DEBUG_ERROR("Failed to to get current CUDA context: %d", result);
    return false;
  }

  if ((result = cuda.cuMemAlloc_v2(&buffer, maxBufferSize)) != CUDA_SUCCESS)
  {
    DEBUG_ERROR("Failed to allocate memory for cuda: %d", result);
    return false;
  }

  *handle = (NvFBCHandle)calloc(sizeof(struct stNvFBCHandle), 1);
  (*handle)->nvfbc = nvfbc;
  (*handle)->context = context;
  (*handle)->buffer = buffer;

  if (maxWidth)
    *maxWidth = params.dwMaxDisplayWidth;

  if (maxHeight)
    *maxHeight = params.dwMaxDisplayHeight;

  return true;
}

void NvFBCCudaRelease(NvFBCHandle * handle)
{
  if (!*handle)
    return;

  cuda.cuMemFree_v2((*handle)->buffer);
  (*handle)->nvfbc->NvFBCCudaRelease();
  free(*handle);
  *handle = NULL;
}

bool NvFBCCudaSetup(
  NvFBCHandle           handle,
  enum                  BufferFormat format,
  bool                  seperateCursorCapture,
  HANDLE              * cursorEvent
)
{
  NVFBC_CUDA_SETUP_PARAMS params = {0};
  params.dwVersion = NVFBC_CUDA_SETUP_PARAMS_VER;

  switch(format)
  {
    case BUFFER_FMT_ARGB      : params.eFormat = NVFBC_TOCUDA_ARGB; break;
    case BUFFER_FMT_ARGB10    :
      params.eFormat     = NVFBC_TOCUDA_ARGB10;
      params.bHDRRequest = TRUE;
      break;

    default:
      DEBUG_INFO("Invalid format");
      return false;
  }

  params.bEnableSeparateCursorCapture = seperateCursorCapture ? TRUE : FALSE;

  NVFBCRESULT status = handle->nvfbc->NvFBCCudaSetup(&params);
  if (status != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to setup NVFBCCuda");
    return false;
  }

  if (cursorEvent)
    *cursorEvent = params.hCursorCaptureEvent;

  return true;
}

CaptureResult NvFBCCudaCapture(
  NvFBCHandle          handle,
  const unsigned int   waitTime,
  const unsigned int   x,
  const unsigned int   y,
  const unsigned int   width,
  const unsigned int   height,
  NvFBCFrameGrabInfo * grabInfo
)
{
  CUresult result;

  static __thread bool setCtx = false;
  if (!setCtx)
  {
    if ((result = cuda.cuCtxSetCurrent(handle->context)) != CUDA_SUCCESS)
    {
      DEBUG_ERROR("Failed to make CUDA context current: %d", result);
      return CAPTURE_RESULT_ERROR;
    }
    setCtx = true;
  }

  NVFBC_CUDA_GRAB_FRAME_PARAMS params = {0};

  params.dwVersion           = NVFBC_CUDA_GRAB_FRAME_PARAMS_VER;
  params.dwFlags             = NVFBC_TOCUDA_WAIT_WITH_TIMEOUT;
  params.dwWaitTime          = waitTime;
  params.pCUDADeviceBuffer   = (void *) handle->buffer;
  params.pNvFBCFrameGrabInfo = grabInfo;

  memset(grabInfo, 0, sizeof *grabInfo);
  grabInfo->bMustRecreate = FALSE;
  NVFBCRESULT status = handle->nvfbc->NvFBCCudaGrabFrame(&params);
  if (grabInfo->bMustRecreate)
  {
    DEBUG_INFO("NvFBC reported recreation is required");
    return CAPTURE_RESULT_REINIT;
  }

  switch(status)
  {
    case NVFBC_SUCCESS:
      handle->retry = 0;
      break;

    case NVFBC_ERROR_INVALID_PARAM:
      if (handle->retry < 2)
      {
        Sleep(100);
        ++handle->retry;
        return CAPTURE_RESULT_TIMEOUT;
      }
      return CAPTURE_RESULT_ERROR;

    case NVFBC_ERROR_DYNAMIC_DISABLE:
      DEBUG_ERROR("NvFBC was disabled by someone else");
      return CAPTURE_RESULT_ERROR;

    case NVFBC_ERROR_INVALIDATED_SESSION:
      DEBUG_WARN("Session was invalidated, attempting to restart");
      return CAPTURE_RESULT_REINIT;

    default:
      DEBUG_ERROR("Unknown NVFBCRESULT failure 0x%x", status);
      return CAPTURE_RESULT_ERROR;
  }

  return CAPTURE_RESULT_OK;
}

CaptureResult NvFBCCudaGetCursor(NvFBCHandle handle, CapturePointer * pointer, void * buffer, unsigned int size)
{
  NVFBC_CURSOR_CAPTURE_PARAMS params;
  params.dwVersion = NVFBC_CURSOR_CAPTURE_PARAMS_VER;

  if (handle->nvfbc->NvFBCCudaCursorCapture(&params) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to get the cursor");
    return CAPTURE_RESULT_ERROR;
  }

  pointer->hx          = params.dwXHotSpot;
  pointer->hy          = params.dwYHotSpot;
  pointer->width       = params.dwWidth;
  pointer->height      = params.dwHeight;
  pointer->pitch       = params.dwPitch;
  pointer->visible     = params.bIsHwCursor;
  pointer->shapeUpdate = params.bIsHwCursor;

  if (!params.bIsHwCursor)
    return CAPTURE_RESULT_OK;

  switch(params.dwPointerFlags & 0x7)
  {
    case 0x1:
      pointer->format  = CAPTURE_FMT_MONO;
      pointer->height *= 2;
      break;

    case 0x2:
      pointer->format = CAPTURE_FMT_COLOR;
      break;

    case 0x4:
      pointer->format = CAPTURE_FMT_MASKED;
      break;

    default:
      DEBUG_ERROR("Invalid/unknown pointer data format");
      return CAPTURE_RESULT_ERROR;
  }

  if (params.dwBufferSize > size)
  {
    DEBUG_WARN("Cursor data larger then provided buffer");
    params.dwBufferSize = size;
  }

  memcpy(buffer, params.pBits, params.dwBufferSize);
  return CAPTURE_RESULT_OK;
}

bool NvFBCCudaCopyFrame(NvFBCHandle handle, void * target, size_t size)
{
  CUresult result;

  static __thread bool setCtx = false;
  if (!setCtx)
  {
    if ((result = cuda.cuCtxSetCurrent(handle->context)) != CUDA_SUCCESS)
    {
      DEBUG_ERROR("Failed to make CUDA context current: %d", result);
      return false;
    }
    setCtx = true;
  }

  static void *registered[2] = {0};
  static int used = 0;

  bool in = false;
  for (int i = 0; i < used; ++i) {
    if (registered[i] == target)
      in = true;
  }

  if (!in && used < 2) {
    registered[used++] = target;
    if ((result = cuda.cuMemHostRegister_v2(target, size, 0)) != CUDA_SUCCESS)
    {
      DEBUG_ERROR("Failed to register memory for CUDA: %d", result);
      return false;
    }
  }

  if ((result = cuda.cuMemcpyDtoH_v2(target, handle->buffer, size)) != CUDA_SUCCESS)
  {
    DEBUG_ERROR("Failed to copy memory from CUDA: %d", result);
    return false;
  }

  return true;
}
