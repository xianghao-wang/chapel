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

// We need to define this variable outside of the commented out HAS_GPU_LOCALE
// section due to the fact that GpuDiagnostics module accesses it (and this
// module can be used despite what locale model you're using).
#include <stdbool.h>
bool chpl_gpu_debug = false;
bool chpl_gpu_no_cpu_mode_warning = false;
int chpl_gpu_num_devices = -1;

#ifdef HAS_GPU_LOCALE

#include "chplrt.h"
#include "chpl-gpu.h"
#include "chpl-gpu-impl.h"
#include "chpl-gpu-diags.h"
#include "chpl-tasks.h"
#include "error.h"
#include "chplcgfns.h"
#include "chpl-linefile-support.h"
#include "chpl-env-gen.h"
#include "chpl-env.h"
#include "chpl-comm-compiler-macros.h"

void chpl_gpu_init(void) {
  // Get available number of devices per locale from env
  const char* env;
  chpl_gpu_num_devices = -1;
  if ((env = chpl_env_rt_get("NUM_GPUS_PER_LOCALE", NULL)) != NULL) {
    if (sscanf(env, "%" SCNi32, &chpl_gpu_num_devices) != 1) {
      chpl_error("Cannot parse CHPL_RT_NUM_GPUS_PER_LOCALE environment "
                 "variable", 0, 0);
    }

    if (chpl_gpu_num_devices < 0) {
      chpl_error("CHPL_RT_NUM_GPUS_PER_LOCALE must be >= 0", 0, 0);
    }
  }

  // Initialize maximum NUM_GPUS_PER_LOCALE devices
  chpl_gpu_impl_init(&chpl_gpu_num_devices);
  assert(chpl_gpu_num_devices >= 0);
}

void chpl_gpu_support_module_finished_initializing(void) {
  // we can't use `CHPL_GPU_DEBUG` before the support module is finished
  // initializing. This call back is used to signal the runtime that that module
  // has finished initializing.

  CHPL_GPU_DEBUG("GPU layer initialized.\n");
  CHPL_GPU_DEBUG("  Memory allocation strategy for ---\n");
  #ifdef CHPL_GPU_MEM_STRATEGY_ARRAY_ON_DEVICE
    CHPL_GPU_DEBUG("    array data: device memory\n");
    CHPL_GPU_DEBUG("         other: page-locked host memory\n");
  #else
    CHPL_GPU_DEBUG("    array data: unified memory\n");
    CHPL_GPU_DEBUG("         other: unified memory\n");
  #endif
}

inline void chpl_gpu_launch_kernel(int ln, int32_t fn,
                                   const char* name,
                                   int grd_dim_x, int grd_dim_y, int grd_dim_z,
                                   int blk_dim_x, int blk_dim_y, int blk_dim_z,
                                   int nargs, ...) {
  CHPL_GPU_DEBUG("Kernel launcher called. (subloc %d)\n"
                 "\tLocation: %s:%d\n"
                 "\tKernel: %s\n"
                 "\tNumArgs: %d\n",
                 chpl_task_getRequestedSubloc(),
                 chpl_lookupFilename(fn),
                 ln,
                 name,
                 nargs);

  va_list args;
  va_start(args, nargs);

  chpl_gpu_impl_use_device(chpl_task_getRequestedSubloc());

  chpl_gpu_diags_verbose_launch(ln, fn, chpl_task_getRequestedSubloc(),
                                blk_dim_x, blk_dim_y, blk_dim_z);
  chpl_gpu_diags_incr(kernel_launch);

  chpl_gpu_impl_launch_kernel(ln, fn,
                              name,
                              grd_dim_x, grd_dim_y, grd_dim_z,
                              blk_dim_x, blk_dim_y, blk_dim_z,
                              nargs, args);
  va_end(args);

  CHPL_GPU_DEBUG("Kernel launcher returning. (subloc %d)\n"
                 "\tKernel: %s\n",
                 chpl_task_getRequestedSubloc(),
                 name);
}

inline void chpl_gpu_launch_kernel_flat(int ln, int32_t fn,
                                        const char* name,
                                        int64_t num_threads, int blk_dim, int nargs,
                                        ...) {

  CHPL_GPU_DEBUG("Kernel launcher called. (subloc %d)\n"
                 "\tLocation: %s:%d\n"
                 "\tKernel: %s\n"
                 "\tNumArgs: %d\n"
                 "\tNumThreads: %lld\n",
                 chpl_task_getRequestedSubloc(),
                 chpl_lookupFilename(fn),
                 ln,
                 name,
                 nargs,
                 num_threads);

  chpl_gpu_impl_use_device(chpl_task_getRequestedSubloc());

  va_list args;
  va_start(args, nargs);

  chpl_gpu_diags_verbose_launch(ln, fn, chpl_task_getRequestedSubloc(),
      blk_dim, 1, 1);
  chpl_gpu_diags_incr(kernel_launch);

  chpl_gpu_impl_launch_kernel_flat(ln, fn,
                                   name,
                                   num_threads, blk_dim,
                                   nargs, args);
  va_end(args);

  CHPL_GPU_DEBUG("Kernel launcher returning. (subloc %d)\n"
                 "\tKernel: %s\n",
                 chpl_task_getRequestedSubloc(),
                 name);
}

extern void chpl_gpu_comm_on_put(c_sublocid_t dst_subloc, void *addr,
                                 c_nodeid_t src_node, c_sublocid_t src_subloc,
                                 void* raddr, size_t size);

extern void chpl_gpu_comm_on_get(c_sublocid_t src_subloc, void* addr,
                                 c_nodeid_t dst_node, c_sublocid_t dst_subloc,
                                 void* raddr, size_t size);


void chpl_gpu_comm_put(c_nodeid_t dst_node, c_sublocid_t dst_subloc, void *dst,
                       c_sublocid_t src_subloc, void *src,
                       size_t size, int32_t commID, int ln, int32_t fn)
{
  void* src_data = src;
  c_sublocid_t src_data_subloc = src_subloc;
  if (src_subloc >= 0) {
    // source is on device, we can't pass device pointers to comm layer. We'll
    // create a copy of the source on the local host.
    src_data = chpl_malloc(size);
    src_data_subloc = c_sublocid_any;

    chpl_gpu_memcpy(src_data_subloc, src_data, src_subloc, src, size, commID,
                    ln, fn);
  }

  if (dst_subloc >= 0) {
    // destination is on device, we can't write to remote GPU memory yet. So,
    // we'll use on+get instead
    chpl_gpu_comm_on_get(src_data_subloc, src_data, dst_node, dst_subloc, dst,
                         size);
  }
  else {
    // destination is on the host, we can do a direct put
    chpl_gen_comm_put(src_data, dst_node, dst, size, commID, ln, fn);
  }

  if (src_subloc >= 0) {
    chpl_free(src_data);
  }
}

void chpl_gpu_comm_get(c_sublocid_t dst_subloc, void *dst,
                       c_nodeid_t src_node, c_sublocid_t src_subloc, void *src,
                       size_t size, int32_t commID, int ln, int32_t fn)
{
  void* dst_buff = dst;
  c_sublocid_t dst_buff_subloc = dst_subloc;
  if (dst_subloc >= 0) {
    // destination is on device, we can't pass device pointers to comm layer.
    // We'll create a buffer on the local host.
    dst_buff = chpl_malloc(size);
    dst_buff_subloc = c_sublocid_any;
  }

  if (src_subloc >= 0) {
    // source is on device, we can't read from remote GPU memory yet. So,
    // we'll use on+put instead
    chpl_gpu_comm_on_put(dst_buff_subloc, dst_buff, src_node, src_subloc, src,
                         size);
  }
  else {
    // source is on the host, we can do a direct put
    chpl_gen_comm_get(dst_buff, src_node, src, size, commID, ln, fn);
  }

  if (dst_subloc >= 0) {
    chpl_gpu_memcpy(dst_subloc, dst, dst_buff_subloc, dst_buff, size,
                    commID, ln, fn);
    chpl_free(dst_buff);
  }
}


void chpl_gpu_memcpy(c_sublocid_t dst_subloc, void* dst,
                     c_sublocid_t src_subloc, const void* src, size_t n,
                     int32_t commID, int ln, int32_t fn) {
  #ifdef CHPL_GPU_MEM_STRATEGY_ARRAY_ON_DEVICE
  if (dst_subloc < 0 && src_subloc < 0) {
    chpl_memmove(dst, src, n);
  }
  else {
    bool dst_on_host = chpl_gpu_impl_is_host_ptr(dst);
    bool src_on_host = chpl_gpu_impl_is_host_ptr(src);

    if (!dst_on_host && !src_on_host) {
      chpl_gpu_copy_device_to_device(dst_subloc, dst, src_subloc, src, n,
                                     commID, ln, fn);
    }
    else if (!dst_on_host) {
      chpl_gpu_copy_host_to_device(dst_subloc, dst, src, n, commID, ln, fn);
    }
    else if (!src_on_host) {
      chpl_gpu_copy_device_to_host(dst, src_subloc, src, n, commID, ln, fn);
    }
    else {
      // Note: this is the case where both source and destination have been
      // created on a GPU sublocale. Therefore, the wide pointers that refer to
      // them have non-negative sublocale. However, not everything created on a
      // GPU sublocale is allocated on the GPU memory. Think of this as a copy
      // between two ints that happen to hav been created on a GPU sublocale.
      assert(dst_on_host && src_on_host);
      memmove(dst, src, n);
    }
  }
  #else

  // for unified memory strategy we don't want to generate calls to copy
  // data from the device to host (since it can just be accessed directly)
  // TODO however, the code path above could be more efficient.
  memmove(dst, src, n);
  #endif

}

void* chpl_gpu_memset(void* addr, const uint8_t val, size_t n) {
  CHPL_GPU_DEBUG("Doing GPU memset of %zu bytes from %p. Val=%d\n\n", n, addr,
                 val);

  void* ret = chpl_gpu_impl_memset(addr, val, n);

  CHPL_GPU_DEBUG("chpl_gpu_memset successful\n");
  return ret;
}

void chpl_gpu_copy_device_to_device(c_sublocid_t dst_dev, void* dst,
                                    c_sublocid_t src_dev, const void* src,
                                    size_t n, int32_t commID, int ln,
                                    int32_t fn) {
  assert(chpl_gpu_is_device_ptr(src));

  CHPL_GPU_DEBUG("Copying %zu bytes from device to host\n", n);

  chpl_gpu_impl_use_device(dst_dev);

  chpl_gpu_diags_verbose_device_to_device_copy(ln, fn, dst_dev, src_dev, n,
                                               commID);
  chpl_gpu_diags_incr(device_to_device);

  chpl_gpu_impl_copy_device_to_device(dst, src, n);

  CHPL_GPU_DEBUG("Copy successful\n");
}

void chpl_gpu_copy_device_to_host(void* dst, c_sublocid_t src_dev,
                                  const void* src, size_t n, int32_t commID,
                                  int ln, int32_t fn) {
  assert(chpl_gpu_is_device_ptr(src));

  CHPL_GPU_DEBUG("Copying %zu bytes from device to host\n", n);

  chpl_gpu_impl_use_device(src_dev);

  chpl_gpu_diags_verbose_device_to_host_copy(ln, fn, src_dev, n, commID);
  chpl_gpu_diags_incr(device_to_host);

  chpl_gpu_impl_copy_device_to_host(dst, src, n);

  CHPL_GPU_DEBUG("Copy successful\n");
}

void chpl_gpu_copy_host_to_device(c_sublocid_t dst_dev, void* dst,
                                  const void* src, size_t n, int32_t commID,
                                  int ln, int32_t fn) {
  assert(chpl_gpu_is_device_ptr(dst));

  CHPL_GPU_DEBUG("Copying %zu bytes from host to device\n", n);

  chpl_gpu_impl_use_device(dst_dev);

  chpl_gpu_diags_verbose_host_to_device_copy(ln, fn, dst_dev, n, commID);
  chpl_gpu_diags_incr(host_to_device);

  chpl_gpu_impl_copy_host_to_device(dst, src, n);

  CHPL_GPU_DEBUG("Copy successful\n");
}

void* chpl_gpu_comm_async(void *dst, void *src, size_t n) {
  assert(chpl_gpu_is_device_ptr(dst) || chpl_gpu_is_device_ptr(src));

  CHPL_GPU_DEBUG("Copying %zu bytes asynchronously between host and device\n", n);

  return chpl_gpu_impl_comm_async(dst, src, n);
}

void chpl_gpu_comm_wait(void *stream) {
  chpl_gpu_impl_comm_wait(stream);
}

size_t chpl_gpu_get_alloc_size(void* ptr) {
  return chpl_gpu_impl_get_alloc_size(ptr);
}

void* chpl_gpu_mem_alloc(size_t size, chpl_mem_descInt_t description,
                         int32_t lineno, int32_t filename) {

  CHPL_GPU_DEBUG("chpl_gpu_mem_alloc called. Size:%d file:%s line:%d\n", size,
               chpl_lookupFilename(filename), lineno);

  void *ptr = NULL;
  if (size > 0) {
    chpl_gpu_impl_use_device(chpl_task_getRequestedSubloc());

    chpl_memhook_malloc_pre(1, size, description, lineno, filename);
    ptr = chpl_gpu_impl_mem_alloc(size);
    chpl_memhook_malloc_post((void*)ptr, 1, size, description, lineno, filename);

    CHPL_GPU_DEBUG("chpl_gpu_mem_alloc returning %p\n", (void*)ptr);
  }
  else {
    CHPL_GPU_DEBUG("chpl_gpu_mem_alloc returning NULL (size was 0)\n");
  }

  return ptr;
}

void* chpl_gpu_mem_array_alloc(size_t size, chpl_mem_descInt_t description,
                               int32_t lineno, int32_t filename) {
  CHPL_GPU_DEBUG("chpl_gpu_mem_array_alloc called. Size:%zu file:%s line:%d\n",
                 size, chpl_lookupFilename(filename), lineno);

  chpl_gpu_impl_use_device(chpl_task_getRequestedSubloc());

  void* ptr = 0;
  if (size > 0) {
    chpl_memhook_malloc_pre(1, size, description, lineno, filename);
    ptr = chpl_gpu_impl_mem_array_alloc(size);
    chpl_memhook_malloc_post((void*)ptr, 1, size, description, lineno, filename);

    CHPL_GPU_DEBUG("chpl_gpu_mem_array_alloc returning %p\n", (void*)ptr);
  }
  else {
    CHPL_GPU_DEBUG("chpl_gpu_mem_array_alloc returning NULL (size was 0)\n");
  }

  return ptr;
}

void chpl_gpu_mem_free(void* memAlloc, int32_t lineno, int32_t filename) {
  CHPL_GPU_DEBUG("chpl_gpu_mem_free is called. Ptr %p\n", memAlloc);

  chpl_gpu_impl_use_device(chpl_task_getRequestedSubloc());

  chpl_memhook_free_pre(memAlloc, 0, lineno, filename);
  chpl_gpu_impl_mem_free(memAlloc);

  CHPL_GPU_DEBUG("chpl_gpu_mem_free is returning\n");
}


void* chpl_gpu_mem_calloc(size_t number, size_t size,
                          chpl_mem_descInt_t description,
                          int32_t lineno, int32_t filename) {

  CHPL_GPU_DEBUG("chpl_gpu_mem_calloc called. Size:%d file:%s line:%d\n", size,
               chpl_lookupFilename(filename), lineno);

  void *ptr = NULL;

  if (size > 0) {
    // TODO this is a really poor implementation -- CUDA has memset, that can
    // help a bit, but omp doesn't expose it. I don't know whether performance
    // here matters.
    const size_t total_size = number*size;

    void *host_mem = chpl_mem_calloc(number, size, description, lineno,
                                     filename);

    c_sublocid_t dev_id = chpl_task_getRequestedSubloc();
    chpl_gpu_impl_use_device(dev_id);

    chpl_memhook_malloc_pre(1, total_size, description, lineno, filename);
    ptr = chpl_gpu_impl_mem_alloc(total_size);
    chpl_memhook_malloc_post((void*)ptr, 1, total_size, description, lineno, filename);

    chpl_gpu_impl_copy_host_to_device(ptr, host_mem, total_size);

    chpl_mem_free(host_mem, lineno, filename);

    CHPL_GPU_DEBUG("chpl_gpu_mem_calloc returning %p\n", (void*)ptr);
  }
  else {
    CHPL_GPU_DEBUG("chpl_gpu_mem_calloc returning NULL (size was 0)\n");
  }

  return ptr;
}

void* chpl_gpu_mem_realloc(void* memAlloc, size_t size,
                           chpl_mem_descInt_t description,
                           int32_t lineno, int32_t filename) {

  CHPL_GPU_DEBUG("chpl_gpu_mem_realloc called. Size:%d\n", size);

  assert(chpl_gpu_is_device_ptr(memAlloc));

  c_sublocid_t dev_id = chpl_task_getRequestedSubloc();
  chpl_gpu_impl_use_device(dev_id);

#ifdef GPU_RUNTIME_CPU
    return chpl_mem_realloc(memAlloc, size, description, lineno, filename);
#else
  size_t cur_size = chpl_gpu_get_alloc_size(memAlloc);
  assert(cur_size >= 0);

  if (size == cur_size) {
    return memAlloc;
  }

  // TODO we could probably do something smarter, especially for the case where
  // the new allocation size is smaller than the original allocation size.
  void* new_alloc = chpl_gpu_mem_alloc(size, description, lineno, filename);

  const size_t copy_size = size < cur_size ? size : cur_size;
  chpl_gpu_impl_copy_device_to_device(new_alloc, memAlloc, copy_size);
  chpl_gpu_mem_free(memAlloc, lineno, filename);

  return new_alloc;
#endif
}

void* chpl_gpu_mem_memalign(size_t boundary, size_t size,
                            chpl_mem_descInt_t description,
                            int32_t lineno, int32_t filename) {
  // ENGIN: I don't know if it is possible to allocate memory with custom
  // alignment on GPU. It looks like GPUs typically have a default alignment
  // (512?) that cannot be changed. I don't think we'd need more than that
  // today, and if we want, we can play some pointer games to return something
  // with a larger alignment here.

  chpl_internal_error("Allocating aligned GPU memory is not supported yet");
  return NULL;
}

void chpl_gpu_hostmem_register(void *memAlloc, size_t size) {
  CHPL_GPU_DEBUG("chpl_gpu_hostmem_register is called. Ptr %p, size: %d\n", memAlloc, size);
  chpl_gpu_impl_hostmem_register(memAlloc, size);
}

bool chpl_gpu_is_device_ptr(const void* ptr) {
  return chpl_gpu_impl_is_device_ptr(ptr);
}

bool chpl_gpu_is_host_ptr(const void* ptr) {
  return chpl_gpu_impl_is_host_ptr(ptr);
}

bool chpl_gpu_can_access_peer(int dev1, int dev2) {
  return chpl_gpu_impl_can_access_peer(dev1, dev2);
}

void chpl_gpu_set_peer_access(int dev1, int dev2, bool enable) {
  chpl_gpu_impl_set_peer_access(dev1, dev2, enable);
}

#endif
