/*
 * Copyright 2020-2023 Hewlett Packard Enterprise Development LP
 * Copyright 2004-2019 Cray Inc.
 * Other additional copyright holders may be indicated within.  *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAS_GPU_LOCALE

// #define CHPL_GPU_ENABLE_PROFILE // define this before including chpl-gpu.h

#include "sys_basic.h"
#include "chplrt.h"
#include "chpl-mem.h"
#include "chpl-gpu.h"
#include "chpl-gpu-impl.h"
#include "chpl-linefile-support.h"
#include "chpl-tasks.h"
#include "error.h"
#include "chplcgfns.h"
#include "../common/cuda-utils.h"
#include "../common/cuda-shared.h"
#include "chpl-env-gen.h"

#include <cuda.h>
#include <cuda_runtime.h>
#include <assert.h>
#include <stdbool.h>

// this is compiler-generated
extern const char* chpl_gpuBinary;

static CUcontext *chpl_gpu_primary_ctx;
static CUdevice  *chpl_gpu_devices;

// array indexed by device ID (we load the same module once for each GPU).
static CUmodule *chpl_gpu_cuda_modules;

static int *deviceClockRates;


static bool chpl_gpu_has_context(void) {
  CUcontext cuda_context = NULL;

  CUresult ret = cuCtxGetCurrent(&cuda_context);

  if (ret == CUDA_ERROR_NOT_INITIALIZED || ret == CUDA_ERROR_DEINITIALIZED) {
    return false;
  }
  else {
    return cuda_context != NULL;
  }
}

static void switch_context(int dev_id) {
  CUcontext next_context = chpl_gpu_primary_ctx[dev_id];

  if (!chpl_gpu_has_context()) {
    CUDA_CALL(cuCtxPushCurrent(next_context));
  }
  else {
    CUcontext cur_context = NULL;
    cuCtxGetCurrent(&cur_context);
    if (cur_context == NULL) {
      chpl_internal_error("Unexpected GPU context error");
    }

    if (cur_context != next_context) {
      CUcontext popped;
      CUDA_CALL(cuCtxPopCurrent(&popped));
      CUDA_CALL(cuCtxPushCurrent(next_context));
    }
  }
}

extern c_nodeid_t chpl_nodeID;

// we can put this logic in chpl-gpu.c. However, it needs to execute
// per-context/module. That's currently too low level for that layer.
static void chpl_gpu_impl_set_globals(c_sublocid_t dev_id, CUmodule module) {
  CUdeviceptr ptr;
  size_t glob_size;
  CUDA_CALL(cuModuleGetGlobal(&ptr, &glob_size, module, "chpl_nodeID"));
  assert(glob_size == sizeof(c_nodeid_t));
  chpl_gpu_impl_copy_host_to_device((void*)ptr, &chpl_nodeID, glob_size);
}

void chpl_gpu_impl_use_device(c_sublocid_t dev_id) {
  switch_context(dev_id);
}

void chpl_gpu_impl_init(int* num_devices) {
  CUDA_CALL(cuInit(0));

  int32_t num = - 1;
  CUDA_CALL(cuDeviceGetCount(&num));
  assert(num != -1);

  if (*num_devices == -1)
    *num_devices = num;
  else
    *num_devices = *num_devices < num ? *num_devices : num;

  const int loc_num_devices = *num_devices;
  chpl_gpu_primary_ctx = chpl_malloc(sizeof(CUcontext)*loc_num_devices);
  chpl_gpu_devices = chpl_malloc(sizeof(CUdevice)*loc_num_devices);
  chpl_gpu_cuda_modules = chpl_malloc(sizeof(CUmodule)*loc_num_devices);
  deviceClockRates = chpl_malloc(sizeof(int)*loc_num_devices);

  int i;
  for (i=0 ; i<loc_num_devices ; i++) {
    CUdevice device;
    CUcontext context;

    CUDA_CALL(cuDeviceGet(&device, i));
    CUDA_CALL(cuDevicePrimaryCtxSetFlags(device, CU_CTX_SCHED_BLOCKING_SYNC));
    CUDA_CALL(cuDevicePrimaryCtxRetain(&context, device));

    CUDA_CALL(cuCtxSetCurrent(context));
    // load the module and setup globals within
    CUmodule module = chpl_gpu_load_module(chpl_gpuBinary);
    chpl_gpu_cuda_modules[i] = module;

    cuDeviceGetAttribute(&deviceClockRates[i], CU_DEVICE_ATTRIBUTE_CLOCK_RATE, device);

    chpl_gpu_devices[i] = device;
    chpl_gpu_primary_ctx[i] = context;

    // TODO can we refactor some of this to chpl-gpu to avoid duplication
    // between runtime layers?
    chpl_gpu_impl_set_globals(i, module);
  }
}

bool chpl_gpu_impl_is_device_ptr(const void* ptr) {
  return chpl_gpu_common_is_device_ptr(ptr);
}

bool chpl_gpu_impl_is_host_ptr(const void* ptr) {
  unsigned int res;
  CUresult ret_val = cuPointerGetAttribute(&res,
                                           CU_POINTER_ATTRIBUTE_MEMORY_TYPE,
                                           (CUdeviceptr)ptr);

  if (ret_val != CUDA_SUCCESS) {
    if (ret_val == CUDA_ERROR_INVALID_VALUE ||
        ret_val == CUDA_ERROR_NOT_INITIALIZED ||
        ret_val == CUDA_ERROR_DEINITIALIZED) {
      return true;
    }
    else {
      CUDA_CALL(ret_val);
    }
  }
  else {
    return res == CU_MEMORYTYPE_HOST;
  }

  return true;
}

static void chpl_gpu_launch_kernel_help(int ln,
                                        int32_t fn,
                                        const char* name,
                                        int grd_dim_x,
                                        int grd_dim_y,
                                        int grd_dim_z,
                                        int blk_dim_x,
                                        int blk_dim_y,
                                        int blk_dim_z,
                                        int nargs,
                                        va_list args) {
  CHPL_GPU_START_TIMER(load_time);

  c_sublocid_t dev_id = chpl_task_getRequestedSubloc();
  CUmodule cuda_module = chpl_gpu_cuda_modules[dev_id];
  void* function = chpl_gpu_load_function(cuda_module, name);

  CHPL_GPU_STOP_TIMER(load_time);
  CHPL_GPU_START_TIMER(prep_time);

  // TODO: this should use chpl_mem_alloc
  void*** kernel_params = chpl_malloc(nargs*sizeof(void**));

  assert(function);
  assert(kernel_params);

  CHPL_GPU_DEBUG("Creating kernel parameters\n");
  CHPL_GPU_DEBUG("\tgridDims=(%d, %d, %d), blockDims(%d, %d, %d)\n",
                 grd_dim_x, grd_dim_y, grd_dim_z,
                 blk_dim_x, blk_dim_y, blk_dim_z);

  // Keep track of kernel parameters we dynamically allocate memory for so
  // later on we know what we need to free.
  bool* was_memory_dynamically_allocated_for_kernel_param =
    chpl_malloc(nargs*sizeof(bool));

  for (int i=0 ; i<nargs ; i++) {
    void* cur_arg = va_arg(args, void*);
    size_t cur_arg_size = va_arg(args, size_t);

    if (cur_arg_size > 0) {
      was_memory_dynamically_allocated_for_kernel_param[i] = true;

      // TODO this allocation needs to use `chpl_mem_alloc` with a proper desc
      kernel_params[i] = chpl_malloc(1*sizeof(CUdeviceptr));

      *kernel_params[i] = chpl_gpu_mem_alloc(cur_arg_size,
                                             CHPL_RT_MD_GPU_KERNEL_ARG,
                                             ln, fn);

      chpl_gpu_impl_copy_host_to_device(*kernel_params[i], cur_arg,
                                        cur_arg_size);

      CHPL_GPU_DEBUG("\tKernel parameter %d: %p (device ptr)\n",
                   i, *kernel_params[i]);
    }
    else {
      was_memory_dynamically_allocated_for_kernel_param[i] = false;
      kernel_params[i] = cur_arg;
      CHPL_GPU_DEBUG("\tKernel parameter %d: %p\n",
                   i, kernel_params[i]);
    }
  }

  CHPL_GPU_STOP_TIMER(prep_time);
  CHPL_GPU_START_TIMER(kernel_time);

  CUDA_CALL(cuLaunchKernel((CUfunction)function,
                           grd_dim_x, grd_dim_y, grd_dim_z,
                           blk_dim_x, blk_dim_y, blk_dim_z,
                           0,  // shared memory in bytes
                           0,  // stream ID
                           (void**)kernel_params,
                           NULL));  // extra options

  CHPL_GPU_DEBUG("cuLaunchKernel returned %s\n", name);

  CUDA_CALL(cudaDeviceSynchronize());

  CHPL_GPU_DEBUG("Synchronization complete %s\n", name);
  CHPL_GPU_STOP_TIMER(kernel_time);
  CHPL_GPU_START_TIMER(teardown_time);

  // free GPU memory allocated for kernel parameters
  for (int i=0 ; i<nargs ; i++) {
    if (was_memory_dynamically_allocated_for_kernel_param[i]) {
      chpl_gpu_mem_free(*kernel_params[i], ln, fn);
    }
  }

  // TODO: this should use chpl_mem_free
  chpl_free(kernel_params);

  CHPL_GPU_STOP_TIMER(teardown_time);
  CHPL_GPU_PRINT_TIMERS("<%20s> Load: %Lf, "
                               "Prep: %Lf, "
                               "Kernel: %Lf, "
                               "Teardown: %Lf\n",
         name, load_time, prep_time, kernel_time, teardown_time);
}

inline void chpl_gpu_impl_launch_kernel(int ln, int32_t fn,
                                        const char* name,
                                        int grd_dim_x,
                                        int grd_dim_y,
                                        int grd_dim_z,
                                        int blk_dim_x,
                                        int blk_dim_y,
                                        int blk_dim_z,
                                        int nargs, va_list args) {
  chpl_gpu_launch_kernel_help(ln, fn,
                              name,
                              grd_dim_x, grd_dim_y, grd_dim_z,
                              blk_dim_x, blk_dim_y, blk_dim_z,
                              nargs, args);
}

inline void chpl_gpu_impl_launch_kernel_flat(int ln, int32_t fn,
                                             const char* name,
                                             int64_t num_threads,
                                             int blk_dim,
                                             int nargs,
                                             va_list args) {
  int grd_dim = (num_threads+blk_dim-1)/blk_dim;

  chpl_gpu_launch_kernel_help(ln, fn,
                              name,
                              grd_dim, 1, 1,
                              blk_dim, 1, 1,
                              nargs, args);
}

void* chpl_gpu_impl_memset(void* addr, const uint8_t val, size_t n) {
  assert(chpl_gpu_is_device_ptr(addr));

  CUDA_CALL(cuMemsetD8((CUdeviceptr)addr, (unsigned int)val, n));

  return addr;
}

void chpl_gpu_impl_copy_device_to_host(void* dst, const void* src, size_t n) {
  assert(chpl_gpu_is_device_ptr(src));

  CUDA_CALL(cuMemcpyDtoH(dst, (CUdeviceptr)src, n));
}

void chpl_gpu_impl_copy_host_to_device(void* dst, const void* src, size_t n) {
  assert(chpl_gpu_is_device_ptr(dst));

  CUDA_CALL(cuMemcpyHtoD((CUdeviceptr)dst, src, n));
}

void chpl_gpu_impl_copy_device_to_device(void* dst, const void* src, size_t n) {
  assert(chpl_gpu_is_device_ptr(dst) && chpl_gpu_is_device_ptr(src));

  CUDA_CALL(cuMemcpyDtoD((CUdeviceptr)dst, (CUdeviceptr)src, n));
}


void* chpl_gpu_impl_comm_async(void *dst, void *src, size_t n) {
  CUstream stream;
  cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING);
  cuMemcpyAsync((CUdeviceptr)dst, (CUdeviceptr)src, n, stream);
  return stream;
}

void chpl_gpu_impl_comm_wait(void *stream) {
  cuStreamSynchronize((CUstream)stream);
  cuStreamDestroy((CUstream)stream);
}

void* chpl_gpu_impl_mem_array_alloc(size_t size) {
  assert(size>0);

  CUdeviceptr ptr = 0;

#ifdef CHPL_GPU_MEM_STRATEGY_ARRAY_ON_DEVICE
    CUDA_CALL(cuMemAlloc(&ptr, size));
#else
    CUDA_CALL(cuMemAllocManaged(&ptr, size, CU_MEM_ATTACH_GLOBAL));
#endif

  return (void*)ptr;
}


void* chpl_gpu_impl_mem_alloc(size_t size) {
#ifdef CHPL_GPU_MEM_STRATEGY_ARRAY_ON_DEVICE
  void* ptr = 0;
  CUDA_CALL(cuMemAllocHost(&ptr, size));
#else
  CUdeviceptr ptr = 0;
  CUDA_CALL(cuMemAllocManaged(&ptr, size, CU_MEM_ATTACH_GLOBAL));
#endif
  assert(ptr!=0);

  return (void*)ptr;
}

void chpl_gpu_impl_mem_free(void* memAlloc) {
  if (memAlloc != NULL) {
    assert(chpl_gpu_is_device_ptr(memAlloc));
#ifdef CHPL_GPU_MEM_STRATEGY_ARRAY_ON_DEVICE
    if (chpl_gpu_impl_is_host_ptr(memAlloc)) {
      CUDA_CALL(cuMemFreeHost(memAlloc));
    }
    else {
#endif
    CUDA_CALL(cuMemFree((CUdeviceptr)memAlloc));
#ifdef CHPL_GPU_MEM_STRATEGY_ARRAY_ON_DEVICE
    }
#endif
  }
}

void chpl_gpu_impl_hostmem_register(void *memAlloc, size_t size) {
  // The CUDA driver uses DMA to transfer page-locked memory to the GPU; if
  // memory is not page-locked it must first be transferred into a page-locked
  // buffer, which degrades performance. So in the array_on_device mode we
  // choose to page-lock such memory even if it's on the host-side.
  #ifdef CHPL_GPU_MEM_STRATEGY_ARRAY_ON_DEVICE
  cudaHostRegister(memAlloc, size, cudaHostRegisterPortable);
  #endif
}

// This can be used for proper reallocation
size_t chpl_gpu_impl_get_alloc_size(void* ptr) {
  return chpl_gpu_common_get_alloc_size(ptr);
}

unsigned int chpl_gpu_device_clock_rate(int32_t devNum) {
  return (unsigned int)deviceClockRates[devNum];
}

bool chpl_gpu_impl_can_access_peer(int dev1, int dev2) {
  int p2p;
  CUDA_CALL(cuDeviceCanAccessPeer(&p2p, chpl_gpu_devices[dev1],
    chpl_gpu_devices[dev2]));
  return p2p != 0;
}

void chpl_gpu_impl_set_peer_access(int dev1, int dev2, bool enable) {
  switch_context(dev1);
  if(enable) {
    CUDA_CALL(cuCtxEnablePeerAccess(chpl_gpu_primary_ctx[dev2], 0));
  } else {
    CUDA_CALL(cuCtxDisablePeerAccess(chpl_gpu_primary_ctx[dev2]));
  }
}

#endif // HAS_GPU_LOCALE
