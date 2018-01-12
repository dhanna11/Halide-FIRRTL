/**
 * Zynq runtime API implementation, mostly copied from src/runtime/zynq.cpp
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "HalideRuntime.h"

#ifndef CMA_BUFFER_T_DEFINED
#define CMA_BUFFER_T_DEFINED
struct mMap;
typedef struct cma_buffer_t {
  unsigned int id; // ID flag for internal use
  unsigned int width; // Width of the image
  unsigned int stride; // Stride between rows, in pixels. This must be >= width
  unsigned int height; // Height of the image
  unsigned int depth; // Byte-depth of the image
  unsigned int phys_addr; // Bus address for DMA
  void* kern_addr; // Kernel virtual address
  struct mMap* cvals;
  unsigned int mmap_offset;
} cma_buffer_t;
#endif

#ifndef _IOCTL_CMDS_H_
#define _IOCTL_CMDS_H_

// TODO: switch these out for "proper" mostly-system-unique ioctl numbers
#define GET_BUFFER 1000 // Get an unused buffer
#define GRAB_IMAGE 1001 // Acquire image from camera
#define FREE_IMAGE 1002 // Release buffer
#define PROCESS_IMAGE 1003 // Push to stencil path
#define PEND_PROCESSED 1004 // Retreive from stencil path

#endif

// file descriptors of devices
int fd_hwacc = 0;
int fd_cma = 0;

int halide_zynq_init() {
    if (fd_cma || fd_hwacc) {
        printf("Zynq runtime is already initialized.\n");
        return -1;
    }
    fd_cma = open("/dev/cmabuffer0", O_RDWR, 0644);
    if(fd_cma == -1) {
        printf("Failed to open cma provider!\n");
        fd_cma = fd_hwacc = 0;
        return -2;
    }
    fd_hwacc = open("/dev/hwacc0", O_RDWR, 0644);
    if(fd_hwacc == -1) {
        printf("Failed to open hwacc device!\n");
        close(fd_cma);
        fd_cma = fd_hwacc = 0;
        return -2;
    }
    return 0;
}

void halide_zynq_free(void *user_context, void *ptr) {
    // do nothing
}

static int cma_get_buffer(cma_buffer_t* ptr) {
    return ioctl(fd_cma, GET_BUFFER, (long unsigned int)ptr);
}

static int cma_free_buffer(cma_buffer_t* ptr) {
    return ioctl(fd_cma, FREE_IMAGE, (long unsigned int)ptr);
}

int halide_zynq_cma_alloc(struct halide_buffer_t *buf) {
    if (fd_cma == 0) {
        printf("Zynq runtime is uninitialized.\n");
        return -1;
    }

    cma_buffer_t *cbuf = (cma_buffer_t *)malloc(sizeof(cma_buffer_t));
    if (cbuf == NULL) {
        printf("malloc failed.\n");
        return -1;
    }

    // TODO check the strides of buf are monotonically increasing

    // Currently kernel buffer only supports 2-D data layout,
    // so we fold lower dimensions into the 'depth' field.
    size_t nDims = buf->dimensions;
    if (nDims < 2) {
        free(cbuf);
        printf("buffer_t has less than 2 dimension, not supported in CMA driver.");
        return -3;
    }
    cbuf->depth = buf->type.bytes();
    if (nDims > 2) {
        for (size_t i = 0; i < nDims - 2; i++)
            cbuf->depth *= buf->dim[i].extent;
    }
    cbuf->width = buf->dim[nDims-2].extent;
    cbuf->height = buf->dim[nDims-1].extent;
    // TODO check stride of dimension are the same as width
    cbuf->stride = cbuf->width;

    int status = cma_get_buffer(cbuf);
    if (status != 0) {
        free(cbuf);
        printf("cma_get_buffer() returned %d (failed).\n", status);
        return -2;
    }

    buf->device = (uint64_t) cbuf;
    buf->host = (uint8_t*) mmap(NULL, cbuf->stride * cbuf->height * cbuf->depth,
                                PROT_WRITE, MAP_SHARED, fd_cma, cbuf->mmap_offset);

    if ((void *) buf->host == (void *) -1) {
        free(cbuf);
        printf("mmap failed.\n");
        return -3;
    }
    return 0;
}

int halide_zynq_cma_free(struct halide_buffer_t *buf) {
    if (fd_cma == 0) {
        printf("Zynq runtime is uninitialized.\n");
        return -1;
    }

    cma_buffer_t *cbuf = (cma_buffer_t *)buf->device;
    munmap((void*)buf->host, cbuf->stride * cbuf->height * cbuf->depth);
    cma_free_buffer(cbuf);
    free(cbuf);
    buf->device = 0;
    return 0;
}

int halide_zynq_subimage(const struct halide_buffer_t* image, struct cma_buffer_t* subimage, void *address_of_subimage_origin, int width, int height) {
    //*subimage = *((cma_buffer_t *)image->device); // copy depth, stride, data, etc.
    subimage->depth = image->dim[0].extent;
    subimage->stride = image->dim[1].stride;
    subimage->kern_addr = image->host;
    subimage->width = width;
    subimage->height = height;
    size_t offset = (uint8_t *)address_of_subimage_origin - image->host;
    subimage->phys_addr += offset;
    subimage->mmap_offset += offset;
    return 0;
}

int halide_zynq_hwacc_launch(struct cma_buffer_t bufs[]) {
    if (fd_hwacc == 0) {
        printf("Zynq runtime is uninitialized.\n");
        return -1;
    }
    int res = ioctl(fd_hwacc, PROCESS_IMAGE, (long unsigned int)bufs);
    return res;
}

int halide_zynq_hwacc_sync(int task_id){
    if (fd_hwacc == 0) {
        printf("Zynq runtime is uninitialized.\n");
        return -1;
    }
    int res = ioctl(fd_hwacc, PEND_PROCESSED, (long unsigned int)task_id);
    return res;
}

