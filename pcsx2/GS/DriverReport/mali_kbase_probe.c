/*
 * SPDX-FileCopyrightText: 2026 ARMSX2 and bmdhacks
 * SPDX-License-Identifier: MIT
 */

/*
 * Mali kernel driver (kbase) probe for the driver diagnostics.
 *
 * Reports what a Vulkan driver talking to kbase directly needs to know about
 * the device: which kbase flavour the kernel runs (CSF or job manager), its
 * user-kernel interface ("UK") version, the GPU's identity and core layout,
 * the vendor's Mali release where the kernel module names say it, and
 * whether each kernel call a CSF driver depends on works from this process
 * (SELinux ioctl filtering and vendor kernel changes show up here).
 *
 * No GPU work is submitted: queues are created and torn down but never
 * kicked, so the probe cannot hang the GPU. It opens its own kbase
 * contexts, which the kernel frees with the file descriptor, and takes
 * about a millisecond.
 *
 * Kernel structures are our own definitions of the kbase UAPI (the kernel's
 * headers are GPL-2.0 WITH Linux-syscall-note), checked by the static
 * asserts below.
 */

#if !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

int mali_kbase_probe(char *json, size_t cap);

/* ---------------------------------------------------------------------- */
/* kbase UAPI subset                                                        */

#define KB_TYPE 0x80

/* CSF kernels speak UK 1.x and take VERSION_CHECK as ioctl 52; job-manager
 * kernels speak 11.x and take it as ioctl 0. Each flavour answers the other
 * number with EPERM. */
#define KB_UK_CSF_MAJOR 1
#define KB_UK_JM_MAJOR 11
/* The CSF interface version the libmali driver is written against. */
#define KB_UK_DRIVER_MINOR 20

struct kb_version_check {
   uint16_t major;
   uint16_t minor;
};

struct kb_set_flags {
   uint32_t create_flags;
};

struct kb_get_gpuprops {
   uint64_t buffer;
   uint32_t size;
   uint32_t flags;
};

union kb_mem_alloc_ex {
   struct {
      uint64_t va_pages;
      uint64_t commit_pages;
      uint64_t extension;
      uint64_t flags;
      uint64_t fixed_address;
      uint64_t extra[3];
   } in;
   struct {
      uint64_t flags;
      uint64_t gpu_va;
   } out;
};

struct kb_mem_free {
   uint64_t gpu_addr;
};

struct kb_mem_exec_init {
   uint64_t va_pages;
};

struct kb_mem_jit_init {
   uint64_t va_pages;
   uint8_t max_allocations;
   uint8_t trim_level;
   uint8_t group_id;
   uint8_t padding[5];
   uint64_t phys_pages;
};

union kb_cs_get_glb_iface {
   struct {
      uint32_t max_group_num;
      uint32_t max_total_stream_num;
      uint64_t groups_ptr;
      uint64_t streams_ptr;
   } in;
   struct {
      uint32_t glb_version;
      uint32_t features;
      uint32_t group_num;
      uint32_t prfcnt_size;
      uint32_t total_stream_num;
      uint32_t instr_features;
   } out;
};

struct kb_cs_group_control {
   uint32_t features;
   uint32_t stream_num;
   uint32_t suspend_size;
   uint32_t padding;
};

struct kb_cs_stream_control {
   uint32_t features;
   uint32_t padding;
};

struct kb_cs_queue_register {
   uint64_t buffer_gpu_addr;
   uint32_t buffer_size;
   uint8_t priority;
   uint8_t padding[3];
};

union kb_cs_queue_bind {
   struct {
      uint64_t buffer_gpu_addr;
      uint8_t group_handle;
      uint8_t csi_index;
      uint8_t padding[6];
   } in;
   struct {
      uint64_t mmap_handle;
   } out;
};

struct kb_cs_queue_terminate {
   uint64_t buffer_gpu_addr;
};

/* The UK >= 1.19 form. */
union kb_cs_queue_group_create {
   struct {
      uint64_t tiler_mask;
      uint64_t fragment_mask;
      uint64_t compute_mask;
      uint8_t cs_min;
      uint8_t priority;
      uint8_t tiler_max;
      uint8_t fragment_max;
      uint8_t compute_max;
      uint8_t csi_handlers;
      uint16_t reserved;
      uint64_t dvs_buf;
      uint64_t padding[9];
   } in;
   struct {
      uint8_t group_handle;
      uint8_t padding[3];
      uint32_t group_uid;
   } out;
};

struct kb_cs_queue_group_term {
   uint8_t group_handle;
   uint8_t padding[7];
};

/* The UK >= 1.14 form. */
union kb_cs_tiler_heap_init {
   struct {
      uint32_t chunk_size;
      uint32_t initial_chunks;
      uint32_t max_chunks;
      uint16_t target_in_flight;
      uint8_t group_id;
      uint8_t padding;
      uint64_t buf_desc_va;
   } in;
   struct {
      uint64_t gpu_heap_va;
      uint64_t first_chunk_va;
   } out;
};

struct kb_cs_tiler_heap_term {
   uint64_t gpu_heap_va;
};

union kb_get_cpu_gpu_timeinfo {
   struct {
      uint32_t request_flags;
      uint32_t paddings[7];
   } in;
   struct {
      uint64_t sec;
      uint32_t nsec;
      uint32_t padding;
      uint64_t timestamp;
      uint64_t cycle_counter;
   } out;
};

_Static_assert(sizeof(struct kb_version_check) == 4, "version_check");
_Static_assert(sizeof(struct kb_get_gpuprops) == 16, "get_gpuprops");
_Static_assert(sizeof(union kb_mem_alloc_ex) == 64, "mem_alloc_ex");
_Static_assert(sizeof(struct kb_mem_jit_init) == 24, "mem_jit_init");
_Static_assert(offsetof(struct kb_mem_jit_init, phys_pages) == 16, "jit phys_pages");
_Static_assert(sizeof(union kb_cs_get_glb_iface) == 24, "get_glb_iface");
_Static_assert(sizeof(struct kb_cs_queue_register) == 16, "queue_register");
_Static_assert(sizeof(union kb_cs_queue_bind) == 16, "queue_bind");
_Static_assert(sizeof(union kb_cs_queue_group_create) == 112, "group_create");
_Static_assert(sizeof(union kb_cs_tiler_heap_init) == 24, "tiler_heap_init");
_Static_assert(sizeof(union kb_get_cpu_gpu_timeinfo) == 32, "timeinfo");

#define KB_IOCTL_VERSION_CHECK_JM  _IOWR(KB_TYPE, 0, struct kb_version_check)
#define KB_IOCTL_SET_FLAGS         _IOW(KB_TYPE, 1, struct kb_set_flags)
#define KB_IOCTL_GET_GPUPROPS      _IOW(KB_TYPE, 3, struct kb_get_gpuprops)
#define KB_IOCTL_MEM_FREE          _IOW(KB_TYPE, 7, struct kb_mem_free)
#define KB_IOCTL_MEM_JIT_INIT      _IOW(KB_TYPE, 14, struct kb_mem_jit_init)
#define KB_IOCTL_CS_QUEUE_REGISTER _IOW(KB_TYPE, 36, struct kb_cs_queue_register)
#define KB_IOCTL_MEM_EXEC_INIT     _IOW(KB_TYPE, 38, struct kb_mem_exec_init)
#define KB_IOCTL_CS_QUEUE_BIND     _IOWR(KB_TYPE, 39, union kb_cs_queue_bind)
#define KB_IOCTL_CS_QUEUE_TERMINATE _IOW(KB_TYPE, 41, struct kb_cs_queue_terminate)
#define KB_IOCTL_CS_QUEUE_GROUP_TERMINATE _IOW(KB_TYPE, 43, struct kb_cs_queue_group_term)
#define KB_IOCTL_CS_TILER_HEAP_INIT _IOWR(KB_TYPE, 48, union kb_cs_tiler_heap_init)
#define KB_IOCTL_CS_TILER_HEAP_TERM _IOW(KB_TYPE, 49, struct kb_cs_tiler_heap_term)
#define KB_IOCTL_GET_CPU_GPU_TIMEINFO _IOWR(KB_TYPE, 50, union kb_get_cpu_gpu_timeinfo)
#define KB_IOCTL_CS_GET_GLB_IFACE  _IOWR(KB_TYPE, 51, union kb_cs_get_glb_iface)
#define KB_IOCTL_VERSION_CHECK_CSF _IOWR(KB_TYPE, 52, struct kb_version_check)
#define KB_IOCTL_CS_QUEUE_GROUP_CREATE _IOWR(KB_TYPE, 58, union kb_cs_queue_group_create)
#define KB_IOCTL_MEM_ALLOC_EX      _IOWR(KB_TYPE, 59, union kb_mem_alloc_ex)

#define KB_PAGE_SIZE 4096u

#define KB_MEM_PROT_CPU_RD      (1ull << 0)
#define KB_MEM_PROT_CPU_WR      (1ull << 1)
#define KB_MEM_PROT_GPU_RD      (1ull << 2)
#define KB_MEM_PROT_GPU_WR      (1ull << 3)
#define KB_MEM_PROT_GPU_EX      (1ull << 4)
#define KB_MEM_COHERENT_SYSTEM  (1ull << 10)
#define KB_MEM_COHERENT_LOCAL   (1ull << 11)
#define KB_MEM_CACHED_CPU       (1ull << 12)
#define KB_MEM_SAME_VA          (1ull << 13)
#define KB_MEM_CSF_EVENT        (1ull << 19)
#define KB_MEM_GROUP_ID(g)      (((uint64_t)(g) & 0xf) << 22)
#define KB_MEM_PROT_ALL \
   (KB_MEM_PROT_CPU_RD | KB_MEM_PROT_CPU_WR | KB_MEM_PROT_GPU_RD | KB_MEM_PROT_GPU_WR)

/* mmap offsets on the kbase fd. */
#define KB_MEM_CSF_USER_REG_PAGE_HANDLE (47ull << 12)

#define KB_TIMEINFO_MONOTONIC (1u << 0)
#define KB_TIMEINFO_TIMESTAMP (1u << 1)

/* GET_GPUPROPS keys: the stream is (u32 key << 2 | size code, value)
 * records, size code 0..3 = 1, 2, 4, 8 bytes. */
enum {
   KP_PRODUCT_ID = 1,
   KP_VERSION_STATUS = 2,
   KP_MINOR_REVISION = 3,
   KP_MAJOR_REVISION = 4,
   KP_GPU_FREQ_KHZ_MAX = 6,
   KP_GPU_AVAILABLE_MEMORY_SIZE = 12,
   KP_L2_LOG2_LINE_SIZE = 13,
   KP_L2_LOG2_CACHE_SIZE = 14,
   KP_L2_NUM_L2_SLICES = 15,
   KP_TILER_BIN_SIZE_BYTES = 16,
   KP_TILER_MAX_ACTIVE_LEVELS = 17,
   KP_MAX_THREADS = 18,
   KP_MAX_WORKGROUP_SIZE = 19,
   KP_MAX_REGISTERS = 21,
   KP_RAW_SHADER_PRESENT = 25,
   KP_RAW_TILER_PRESENT = 26,
   KP_RAW_L2_PRESENT = 27,
   KP_RAW_L2_FEATURES = 29,
   KP_RAW_CORE_FEATURES = 30,
   KP_RAW_MEM_FEATURES = 31,
   KP_RAW_MMU_FEATURES = 32,
   KP_RAW_TILER_FEATURES = 51,
   KP_RAW_GPU_ID = 55,
   KP_RAW_THREAD_FEATURES = 59,
   KP_RAW_COHERENCY_MODE = 60,
   KP_NUM_EXEC_ENGINES = 82,
   KP_RAW_GPU_FEATURES = 85,
   KP_COUNT = 86,
};

/* ---------------------------------------------------------------------- */
/* JSON output                                                             */

struct out {
   char *buf;
   size_t cap;
   size_t len;
   int overflow;
};

static void
out_raw(struct out *o, const char *s, size_t n)
{
   if (o->len + n + 1 > o->cap) {
      o->overflow = 1;
      return;
   }
   memcpy(o->buf + o->len, s, n);
   o->len += n;
   o->buf[o->len] = '\0';
}

#define out_lit(o, s) out_raw((o), (s), sizeof(s) - 1)

static void
out_fmt(struct out *o, const char *fmt, ...)
{
   char tmp[256];
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
   va_end(ap);
   if (n < 0)
      return;
   if ((size_t)n >= sizeof(tmp)) {
      o->overflow = 1;
      return;
   }
   out_raw(o, tmp, (size_t)n);
}

static void
out_str(struct out *o, const char *s)
{
   if (!s) {
      out_lit(o, "null");
      return;
   }
   out_lit(o, "\"");
   for (; *s; s++) {
      unsigned char c = (unsigned char)*s;
      if (c == '"' || c == '\\') {
         char e[2] = {'\\', (char)c};
         out_raw(o, e, 2);
      } else if (c < 0x20 || c >= 0x7f) {
         out_fmt(o, "\\u%04x", c);
      } else {
         out_raw(o, (const char *)&c, 1);
      }
   }
   out_lit(o, "\"");
}

/* ---------------------------------------------------------------------- */
/* Steps                                                                   */

#define MAX_STEPS 32

struct step {
   const char *name;
   int ok;
   char error[96];
   double ms;
};

struct probe {
   struct step steps[MAX_STEPS];
   unsigned nsteps;
   struct timespec t0;
};

static double
ms_since(const struct timespec *t0)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (double)(t.tv_sec - t0->tv_sec) * 1e3 + (double)(t.tv_nsec - t0->tv_nsec) / 1e6;
}

static void
step_begin(struct probe *p)
{
   clock_gettime(CLOCK_MONOTONIC, &p->t0);
}

static const char *
errno_name(int e)
{
   switch (e) {
   case EPERM: return "EPERM";
   case ENOENT: return "ENOENT";
   case EINTR: return "EINTR";
   case EIO: return "EIO";
   case ENXIO: return "ENXIO";
   case EBADF: return "EBADF";
   case EAGAIN: return "EAGAIN";
   case ENOMEM: return "ENOMEM";
   case EACCES: return "EACCES";
   case EFAULT: return "EFAULT";
   case EBUSY: return "EBUSY";
   case EEXIST: return "EEXIST";
   case ENODEV: return "ENODEV";
   case EINVAL: return "EINVAL";
   case ENOSPC: return "ENOSPC";
   case ENOTTY: return "ENOTTY";
   case ENOSYS: return "ENOSYS";
   case EOPNOTSUPP: return "EOPNOTSUPP";
   case ETIMEDOUT: return "ETIMEDOUT";
   default: return NULL;
   }
}

/* Records a finished step. err: 0 = ok, > 0 an errno, < 0 a failure
 * described by msg alone. */
static void
step_end(struct probe *p, const char *name, int err, const char *msg)
{
   if (p->nsteps >= MAX_STEPS)
      return;
   struct step *s = &p->steps[p->nsteps++];
   s->name = name;
   s->ok = err == 0;
   s->ms = ms_since(&p->t0);
   s->error[0] = '\0';
   if (err > 0) {
      const char *n = errno_name(err);
      if (msg)
         snprintf(s->error, sizeof(s->error), "%s: %s (%d)", msg, n ? n : strerror(err), err);
      else
         snprintf(s->error, sizeof(s->error), "%s (%d)", n ? n : strerror(err), err);
   } else if (err < 0 && msg) {
      snprintf(s->error, sizeof(s->error), "%s", msg);
   }
}

static void
step_skip(struct probe *p, const char *name, const char *why)
{
   step_begin(p);
   char msg[96];
   snprintf(msg, sizeof(msg), "skipped: %s failed", why);
   step_end(p, name, -1, msg);
}

static int
kioctl(int fd, unsigned long req, void *arg)
{
   int r;
   do {
      r = ioctl(fd, req, arg);
   } while (r < 0 && errno == EINTR);
   return r < 0 ? errno : 0;
}

/* ---------------------------------------------------------------------- */
/* GPU properties                                                          */

struct gpu_props {
   int valid;
   uint64_t v[KP_COUNT];
   uint8_t seen[KP_COUNT];
};

static int
read_gpu_props(int fd, struct gpu_props *gp)
{
   /* A call with no buffer returns the stream size. */
   struct kb_get_gpuprops q = {0};
   int size = ioctl(fd, KB_IOCTL_GET_GPUPROPS, &q);
   if (size < 0)
      return errno;
   if (size == 0 || size > 16384)
      return EINVAL;

   uint8_t buf[16384];
   memset(buf, 0, (size_t)size);
   q.buffer = (uint64_t)(uintptr_t)buf;
   q.size = (uint32_t)size;
   int got = ioctl(fd, KB_IOCTL_GET_GPUPROPS, &q);
   if (got < 0)
      return errno;
   if (got > size)
      got = size;

   memset(gp, 0, sizeof(*gp));
   size_t pos = 0;
   while (pos + 4 <= (size_t)got) {
      uint32_t token = (uint32_t)buf[pos] | (uint32_t)buf[pos + 1] << 8 |
                       (uint32_t)buf[pos + 2] << 16 | (uint32_t)buf[pos + 3] << 24;
      unsigned key = token >> 2;
      unsigned bytes = 1u << (token & 3);
      pos += 4;
      if (pos + bytes > (size_t)got)
         break;
      uint64_t val = 0;
      for (unsigned i = 0; i < bytes; i++)
         val |= (uint64_t)buf[pos + i] << (8 * i);
      pos += bytes;
      if (key < KP_COUNT) {
         gp->v[key] = val;
         gp->seen[key] = 1;
      }
   }
   gp->valid = 1;
   return 0;
}

static unsigned
popcount64(uint64_t v)
{
   unsigned n = 0;
   for (; v; v &= v - 1)
      n++;
   return n;
}

/* Product names by PRODUCT_ID with the arch revision nibble cleared
 * (arch major, arch minor, product major); the same table as Mesa's
 * pan_model.c. Job-manager Midgard parts use another numbering and are
 * reported as unknown. */
static const char *
product_name(uint32_t product_id, unsigned cores)
{
   switch (product_id & 0xff0f) {
   case 0x6000: return "Mali-G71";
   case 0x6201: return "Mali-G72";
   case 0x7000: return "Mali-G51";
   case 0x7003: return "Mali-G31";
   case 0x7201: return "Mali-G76";
   case 0x7202: return "Mali-G52";
   case 0x7402: return "Mali-G52 r1";
   case 0x9001: return "Mali-G57";
   case 0x9003: return "Mali-G57";
   case 0x9204: return "Mali-G68";
   case 0xa807: return "Mali-G610";
   case 0xac04: return "Mali-G310";
   case 0xb802: return cores < 7 ? "Mali-G615" : "Mali-G715";
   case 0xb803: return "Mali-G615";
   case 0xc800: return "Mali-G720";
   case 0xd800: return "Mali-G725";
   case 0xe800: return "Mali-G1-Ultra";
   case 0xe801: return "Mali-G1-Premium";
   case 0xe803: return "Mali-G1-Pro";
   default: return NULL;
   }
}

/* ---------------------------------------------------------------------- */
/* The vendor's Mali release                                               */

/* Vendors often build the kbase module under a name that carries the Mali
 * release it comes from (MediaTek: "mali_kbase_mt6897_r44"), and the
 * userspace driver ships from the same release. Reading the module names is
 * cheap; the release string inside the userspace driver is assembled at run
 * time and would need the whole library scanned. */
#define MAX_MODULES 8

struct modules {
   char name[MAX_MODULES][64];
   unsigned count;
   char release[16]; /* "r44", from the first name with an _r<N> suffix */
   int readable;
};

static void
read_modules(struct modules *m)
{
   memset(m, 0, sizeof(*m));
   DIR *d = opendir("/sys/module");
   if (!d)
      return;
   m->readable = 1;
   struct dirent *de;
   while ((de = readdir(d)) != NULL && m->count < MAX_MODULES) {
      if (!strstr(de->d_name, "mali"))
         continue;
      snprintf(m->name[m->count], sizeof(m->name[0]), "%.63s", de->d_name);
      if (!m->release[0]) {
         const char *r = strstr(de->d_name, "_r");
         while (r) {
            const char *q = r + 2;
            size_t n = 0;
            while (q[n] >= '0' && q[n] <= '9')
               n++;
            if (n && (q[n] == '\0' || q[n] == '_' || q[n] == 'p')) {
               size_t len = 1 + n;
               if (q[n] == 'p')
                  while (q[len] >= '0' && q[len] <= '9')
                     len++;
               if (len < sizeof(m->release)) {
                  memcpy(m->release, r + 1, len);
                  m->release[len] = '\0';
               }
               break;
            }
            r = strstr(r + 2, "_r");
         }
      }
      m->count++;
   }
   closedir(d);
}

/* ---------------------------------------------------------------------- */
/* CSF context checks                                                      */

struct csf_info {
   int valid;
   uint32_t glb_version;
   uint32_t group_num;
   uint32_t total_stream_num;
   uint32_t cs_work_registers;
   uint32_t instr_features;
};

struct bo {
   uint64_t va; /* GPU VA; equals the CPU address for SAME_VA */
   void *cpu;
   size_t size;
};

/* MEM_ALLOC_EX, then the mmap that turns a SAME_VA cookie into an
 * address. */
static int
alloc_bo(int fd, uint64_t flags, size_t size, struct bo *bo, const char **what)
{
   memset(bo, 0, sizeof(*bo));
   /* The kernel overwrites the input half of the union with its answer. */
   const uint64_t pages = (size + KB_PAGE_SIZE - 1) / KB_PAGE_SIZE;
   union kb_mem_alloc_ex a;
   memset(&a, 0, sizeof(a));
   a.in.va_pages = pages;
   a.in.commit_pages = pages;
   a.in.flags = flags;
   int e = kioctl(fd, KB_IOCTL_MEM_ALLOC_EX, &a);
   if (e) {
      *what = "MEM_ALLOC_EX";
      return e;
   }
   bo->size = (size_t)pages * KB_PAGE_SIZE;
   bo->va = a.out.gpu_va;
   if (flags & KB_MEM_SAME_VA) {
      int prot = PROT_NONE;
      if (flags & KB_MEM_PROT_CPU_RD)
         prot |= PROT_READ;
      if (flags & KB_MEM_PROT_CPU_WR)
         prot |= PROT_WRITE;
      void *p = mmap(NULL, bo->size, prot, MAP_SHARED, fd, (off_t)a.out.gpu_va);
      if (p == MAP_FAILED) {
         int err = errno;
         struct kb_mem_free f = {.gpu_addr = a.out.gpu_va};
         kioctl(fd, KB_IOCTL_MEM_FREE, &f);
         *what = "mmap";
         return err;
      }
      bo->cpu = p;
      bo->va = (uint64_t)(uintptr_t)p;
   }
   return 0;
}

static void
free_bo(int fd, struct bo *bo)
{
   if (bo->cpu)
      munmap(bo->cpu, bo->size);
   if (bo->va) {
      struct kb_mem_free f = {.gpu_addr = bo->va};
      kioctl(fd, KB_IOCTL_MEM_FREE, &f);
   }
   memset(bo, 0, sizeof(*bo));
}

/* An allocation of one page with the given flags; SAME_VA ones are
 * written and read back through the CPU mapping. */
static void
check_alloc(struct probe *p, int fd, const char *name, uint64_t flags)
{
   step_begin(p);
   struct bo bo;
   const char *what = NULL;
   int e = alloc_bo(fd, flags, KB_PAGE_SIZE, &bo, &what);
   if (!e && bo.cpu) {
      volatile uint32_t *w = bo.cpu;
      w[0] = 0x4d414c49u;
      w[1023] = 0x50524f42u;
      if (w[0] != 0x4d414c49u || w[1023] != 0x50524f42u) {
         free_bo(fd, &bo);
         step_end(p, name, -1, "CPU mapping does not read back what was written");
         return;
      }
   }
   if (!e)
      free_bo(fd, &bo);
   step_end(p, name, e, what);
}

static void
csf_checks(struct probe *p, int fd, struct csf_info *ci)
{
   int e;

   step_begin(p);
   struct kb_set_flags sf = {0};
   e = kioctl(fd, KB_IOCTL_SET_FLAGS, &sf);
   step_end(p, "driver_set_flags", e, NULL);
   if (e) {
      static const char *const rest[] = {
         "cs_get_glb_iface", "map_user_reg_page", "mem_exec_init", "mem_jit_init",
         "alloc_group0", "alloc_group6", "alloc_tls_group9", "alloc_exec",
         "alloc_csf_event", "queue_group_create", "queue_register_bind",
         "tiler_heap_init", "cpu_gpu_timeinfo",
      };
      for (unsigned i = 0; i < sizeof(rest) / sizeof(rest[0]); i++)
         step_skip(p, rest[i], "driver_set_flags");
      return;
   }

   /* The firmware's global interface: first the counts, then the arrays. */
   step_begin(p);
   union kb_cs_get_glb_iface g;
   memset(&g, 0, sizeof(g));
   e = kioctl(fd, KB_IOCTL_CS_GET_GLB_IFACE, &g);
   if (!e) {
      struct kb_cs_group_control groups[32];
      struct kb_cs_stream_control streams[256];
      uint32_t ng = g.out.group_num < 32 ? g.out.group_num : 32;
      uint32_t ns = g.out.total_stream_num < 256 ? g.out.total_stream_num : 256;
      memset(&g, 0, sizeof(g));
      g.in.max_group_num = ng;
      g.in.max_total_stream_num = ns;
      g.in.groups_ptr = (uint64_t)(uintptr_t)groups;
      g.in.streams_ptr = (uint64_t)(uintptr_t)streams;
      e = kioctl(fd, KB_IOCTL_CS_GET_GLB_IFACE, &g);
      if (!e) {
         ci->valid = 1;
         ci->glb_version = g.out.glb_version;
         ci->group_num = g.out.group_num;
         ci->total_stream_num = g.out.total_stream_num;
         ci->instr_features = g.out.instr_features;
         /* STREAM_FEATURES bits 7:0 = work registers - 1. */
         ci->cs_work_registers = ns ? (streams[0].features & 0xff) + 1 : 0;
      }
   }
   step_end(p, "cs_get_glb_iface", e, NULL);

   step_begin(p);
   void *reg = mmap(NULL, KB_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
                    (off_t)KB_MEM_CSF_USER_REG_PAGE_HANDLE);
   e = reg == MAP_FAILED ? errno : 0;
   if (!e)
      munmap(reg, KB_PAGE_SIZE);
   step_end(p, "map_user_reg_page", e, NULL);

   /* The executable zone: 4 GiB. */
   step_begin(p);
   struct kb_mem_exec_init ei = {.va_pages = 1ull << 20};
   e = kioctl(fd, KB_IOCTL_MEM_EXEC_INIT, &ei);
   step_end(p, "mem_exec_init", e, NULL);

   /* On a 64-bit CSF context this also creates the zone the kernel takes
    * tiler heap memory from. */
   step_begin(p);
   struct kb_mem_jit_init ji = {
      .va_pages = 0x2000000,
      .max_allocations = 255,
      .trim_level = 5,
      .group_id = 0,
      .phys_pages = 0x2000000,
   };
   int jit_err = kioctl(fd, KB_IOCTL_MEM_JIT_INIT, &ji);
   step_end(p, "mem_jit_init", jit_err, NULL);

   /* The memory groups the libmali driver uses: 0, 6 for most memory and
    * 9 for thread-local storage. A kernel whose memory group manager does
    * not know a group refuses the allocation. */
   const uint64_t same_va = KB_MEM_PROT_ALL | KB_MEM_SAME_VA | KB_MEM_COHERENT_LOCAL;
   check_alloc(p, fd, "alloc_group0", same_va | KB_MEM_GROUP_ID(0));
   check_alloc(p, fd, "alloc_group6", same_va | KB_MEM_GROUP_ID(6));
   check_alloc(p, fd, "alloc_tls_group9",
               KB_MEM_PROT_GPU_RD | KB_MEM_PROT_GPU_WR | KB_MEM_GROUP_ID(9));
   /* Shader code: not SAME_VA, so it lands in the executable zone. */
   check_alloc(p, fd, "alloc_exec",
               KB_MEM_PROT_CPU_RD | KB_MEM_PROT_CPU_WR | KB_MEM_PROT_GPU_RD |
                  KB_MEM_PROT_GPU_EX | KB_MEM_GROUP_ID(6));
   /* Sync objects the firmware waits on. */
   check_alloc(p, fd, "alloc_csf_event",
               KB_MEM_PROT_ALL | KB_MEM_SAME_VA | KB_MEM_COHERENT_SYSTEM |
                  KB_MEM_CSF_EVENT | KB_MEM_GROUP_ID(6));

   /* A queue group with three streams, as the driver creates, and one
    * queue registered and bound to it. Nothing is kicked. */
   step_begin(p);
   union kb_cs_queue_group_create gc;
   memset(&gc, 0, sizeof(gc));
   gc.in.tiler_mask = 1;
   gc.in.fragment_mask = ~0ull;
   gc.in.compute_mask = ~0ull;
   gc.in.cs_min = 3;
   gc.in.priority = 1; /* medium */
   gc.in.tiler_max = 1;
   gc.in.fragment_max = 64;
   gc.in.compute_max = 64;
   int group_err = kioctl(fd, KB_IOCTL_CS_QUEUE_GROUP_CREATE, &gc);
   step_end(p, "queue_group_create", group_err, NULL);

   if (group_err) {
      step_skip(p, "queue_register_bind", "queue_group_create");
   } else {
      step_begin(p);
      struct bo ring;
      const char *what = NULL;
      e = alloc_bo(fd, same_va | KB_MEM_GROUP_ID(6), KB_PAGE_SIZE, &ring, &what);
      if (!e) {
         struct kb_cs_queue_register qr = {
            .buffer_gpu_addr = ring.va,
            .buffer_size = KB_PAGE_SIZE,
            .priority = 0,
         };
         e = kioctl(fd, KB_IOCTL_CS_QUEUE_REGISTER, &qr);
         what = "CS_QUEUE_REGISTER";
         if (!e) {
            union kb_cs_queue_bind qb;
            memset(&qb, 0, sizeof(qb));
            qb.in.buffer_gpu_addr = ring.va;
            qb.in.group_handle = gc.out.group_handle;
            qb.in.csi_index = 0;
            e = kioctl(fd, KB_IOCTL_CS_QUEUE_BIND, &qb);
            what = "CS_QUEUE_BIND";
            if (!e) {
               /* The queue's doorbell, input and output pages. */
               void *io = mmap(NULL, 3 * KB_PAGE_SIZE, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, (off_t)qb.out.mmap_handle);
               if (io == MAP_FAILED) {
                  e = errno;
                  what = "mmap of the queue I/O pages";
               } else {
                  munmap(io, 3 * KB_PAGE_SIZE);
               }
            }
            struct kb_cs_queue_terminate qt = {.buffer_gpu_addr = ring.va};
            kioctl(fd, KB_IOCTL_CS_QUEUE_TERMINATE, &qt);
         }
         free_bo(fd, &ring);
      }
      step_end(p, "queue_register_bind", e, e ? what : NULL);

      struct kb_cs_queue_group_term gt = {.group_handle = gc.out.group_handle};
      kioctl(fd, KB_IOCTL_CS_QUEUE_GROUP_TERMINATE, &gt);
   }

   /* A tiler heap as the driver creates it: 2 MiB chunks, one to start. */
   if (jit_err) {
      step_skip(p, "tiler_heap_init", "mem_jit_init");
   } else {
      step_begin(p);
      struct bo desc;
      const char *what = NULL;
      e = alloc_bo(fd, same_va | KB_MEM_GROUP_ID(6), KB_PAGE_SIZE, &desc, &what);
      if (!e) {
         memset(desc.cpu, 0, desc.size);
         union kb_cs_tiler_heap_init hi;
         memset(&hi, 0, sizeof(hi));
         hi.in.chunk_size = 2u << 20;
         hi.in.initial_chunks = 1;
         hi.in.max_chunks = 552;
         hi.in.target_in_flight = 0xffff;
         hi.in.group_id = 0;
         hi.in.buf_desc_va = desc.va;
         e = kioctl(fd, KB_IOCTL_CS_TILER_HEAP_INIT, &hi);
         what = "CS_TILER_HEAP_INIT";
         if (!e) {
            struct kb_cs_tiler_heap_term ht = {.gpu_heap_va = hi.out.gpu_heap_va};
            kioctl(fd, KB_IOCTL_CS_TILER_HEAP_TERM, &ht);
         }
         free_bo(fd, &desc);
      }
      step_end(p, "tiler_heap_init", e, e ? what : NULL);
   }

   step_begin(p);
   union kb_get_cpu_gpu_timeinfo t;
   memset(&t, 0, sizeof(t));
   t.in.request_flags = KB_TIMEINFO_MONOTONIC | KB_TIMEINFO_TIMESTAMP;
   e = kioctl(fd, KB_IOCTL_GET_CPU_GPU_TIMEINFO, &t);
   step_end(p, "cpu_gpu_timeinfo", e, NULL);
}

/* ---------------------------------------------------------------------- */
/* Report                                                                  */

static void
out_gpu(struct out *o, const struct gpu_props *gp)
{
   const uint64_t *v = gp->v;
   uint32_t product_id = (uint32_t)v[KP_PRODUCT_ID];
   uint64_t gpu_id = gp->seen[KP_RAW_GPU_ID] ? v[KP_RAW_GPU_ID]
                                             : (uint64_t)product_id << 16;
   uint64_t shader = v[KP_RAW_SHADER_PRESENT];
   unsigned cores = popcount64(shader);
   const char *name = product_name(product_id, cores);

   out_fmt(o, "{\"gpu_id\":\"0x%llx\",\"product_id\":\"0x%04x\"",
           (unsigned long long)gpu_id, product_id);
   out_fmt(o, ",\"arch\":\"v%u (%u.%u rev %u)\"", (product_id >> 12) & 0xf,
           (product_id >> 12) & 0xf, (product_id >> 8) & 0xf, (product_id >> 4) & 0xf);
   out_lit(o, ",\"product\":");
   out_str(o, name);
   out_fmt(o, ",\"revision\":\"r%up%u\",\"version_status\":%u",
           (unsigned)v[KP_MAJOR_REVISION], (unsigned)v[KP_MINOR_REVISION],
           (unsigned)v[KP_VERSION_STATUS]);
   out_fmt(o, ",\"shader_core_mask\":\"0x%llx\",\"shader_cores\":%u",
           (unsigned long long)shader, cores);
   out_fmt(o, ",\"l2\":{\"present\":\"0x%llx\",\"slices\":%u,\"log2_line_size\":%u,"
              "\"log2_cache_size\":%u,\"features\":\"0x%llx\"}",
           (unsigned long long)v[KP_RAW_L2_PRESENT], (unsigned)v[KP_L2_NUM_L2_SLICES],
           (unsigned)v[KP_L2_LOG2_LINE_SIZE], (unsigned)v[KP_L2_LOG2_CACHE_SIZE],
           (unsigned long long)v[KP_RAW_L2_FEATURES]);
   out_fmt(o, ",\"tiler\":{\"present\":\"0x%llx\",\"bin_size_bytes\":%u,"
              "\"max_active_levels\":%u,\"features\":\"0x%llx\"}",
           (unsigned long long)v[KP_RAW_TILER_PRESENT], (unsigned)v[KP_TILER_BIN_SIZE_BYTES],
           (unsigned)v[KP_TILER_MAX_ACTIVE_LEVELS],
           (unsigned long long)v[KP_RAW_TILER_FEATURES]);
   out_fmt(o, ",\"gpu_freq_khz_max\":%llu,\"available_memory\":%llu",
           (unsigned long long)v[KP_GPU_FREQ_KHZ_MAX],
           (unsigned long long)v[KP_GPU_AVAILABLE_MEMORY_SIZE]);
   out_fmt(o, ",\"max_threads\":%u,\"max_workgroup_size\":%u,\"max_registers\":%u",
           (unsigned)v[KP_MAX_THREADS], (unsigned)v[KP_MAX_WORKGROUP_SIZE],
           (unsigned)v[KP_MAX_REGISTERS]);
   out_fmt(o, ",\"num_exec_engines\":%u,\"va_bits\":%u,\"coherency_mode\":%u",
           (unsigned)v[KP_NUM_EXEC_ENGINES], (unsigned)(v[KP_RAW_MMU_FEATURES] & 0xff),
           (unsigned)v[KP_RAW_COHERENCY_MODE]);
   out_fmt(o, ",\"core_features\":\"0x%llx\",\"thread_features\":\"0x%llx\","
              "\"mem_features\":\"0x%llx\",\"gpu_features\":\"0x%llx\"}",
           (unsigned long long)v[KP_RAW_CORE_FEATURES],
           (unsigned long long)v[KP_RAW_THREAD_FEATURES],
           (unsigned long long)v[KP_RAW_MEM_FEATURES],
           (unsigned long long)v[KP_RAW_GPU_FEATURES]);
}

int
mali_kbase_probe(char *json, size_t cap)
{
   static const char *const node = "/dev/mali0";
   struct out o = {.buf = json, .cap = cap};
   struct probe p;
   memset(&p, 0, sizeof(p));
   if (!json || cap == 0)
      return -ENOSPC;
   json[0] = '\0';

   struct gpu_props gp;
   memset(&gp, 0, sizeof(gp));
   struct csf_info ci;
   memset(&ci, 0, sizeof(ci));
   const char *frontend = NULL;
   struct kb_version_check uk = {0};
   int uk_valid = 0;
   struct kb_version_check drv = {0};
   int drv_valid = 0;

   /* First context: learn the flavour and the kernel's own UK version by
    * proposing a minor above any kernel's (the kernel answers the lower
    * of the two), then read the GPU properties. */
   step_begin(&p);
   int fd = open(node, O_RDWR | O_CLOEXEC);
   int open_err = fd < 0 ? errno : 0;
   step_end(&p, "open", open_err, NULL);

   if (fd >= 0) {
      step_begin(&p);
      struct kb_version_check vc = {.major = KB_UK_CSF_MAJOR, .minor = 0xffff};
      int e = kioctl(fd, KB_IOCTL_VERSION_CHECK_CSF, &vc);
      if (!e) {
         frontend = "csf";
      } else {
         vc.major = KB_UK_JM_MAJOR;
         vc.minor = 0xffff;
         int ej = kioctl(fd, KB_IOCTL_VERSION_CHECK_JM, &vc);
         if (!ej) {
            frontend = "jm";
            e = 0;
         }
      }
      if (!e) {
         uk = vc;
         uk_valid = 1;
      }
      step_end(&p, "version_handshake", e, NULL);

      if (!e) {
         step_begin(&p);
         struct kb_set_flags sf = {0};
         e = kioctl(fd, KB_IOCTL_SET_FLAGS, &sf);
         step_end(&p, "set_flags", e, NULL);
      } else {
         step_skip(&p, "set_flags", "version_handshake");
      }

      if (!e) {
         step_begin(&p);
         e = read_gpu_props(fd, &gp);
         step_end(&p, "get_gpuprops", e, NULL);
      } else {
         step_skip(&p, "get_gpuprops", uk_valid ? "set_flags" : "version_handshake");
      }
      close(fd);
   } else {
      step_skip(&p, "version_handshake", "open");
      step_skip(&p, "set_flags", "open");
      step_skip(&p, "get_gpuprops", "open");
   }

   /* Second context, CSF only: the handshake the libmali driver makes
    * (UK 1.20) and the kernel calls it depends on. */
   if (frontend && strcmp(frontend, "csf") == 0) {
      step_begin(&p);
      int fd2 = open(node, O_RDWR | O_CLOEXEC);
      int e = fd2 < 0 ? errno : 0;
      if (!e) {
         drv.major = KB_UK_CSF_MAJOR;
         drv.minor = KB_UK_DRIVER_MINOR;
         e = kioctl(fd2, KB_IOCTL_VERSION_CHECK_CSF, &drv);
         if (!e)
            drv_valid = 1;
         if (!e && (drv.major != KB_UK_CSF_MAJOR || drv.minor != KB_UK_DRIVER_MINOR)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "kernel answered UK %u.%u, the driver needs 1.%u",
                     drv.major, drv.minor, KB_UK_DRIVER_MINOR);
            step_end(&p, "driver_handshake", -1, msg);
            e = -1;
         } else {
            step_end(&p, "driver_handshake", e, NULL);
         }
      } else {
         step_end(&p, "driver_handshake", e, "reopen");
      }
      if (e == 0)
         csf_checks(&p, fd2, &ci);
      if (fd2 >= 0)
         close(fd2);
   }

   step_begin(&p);
   struct modules mods;
   read_modules(&mods);
   step_end(&p, "kernel_modules", mods.readable ? 0 : -1,
            mods.readable ? NULL : "/sys/module is not readable");

   /* The JSON object. */
   out_lit(&o, "{\"node\":");
   out_str(&o, node);
   out_fmt(&o, ",\"opened\":%s,\"errno\":%d", fd >= 0 ? "true" : "false", open_err);
   out_lit(&o, ",\"frontend\":");
   out_str(&o, frontend);
   if (uk_valid)
      out_fmt(&o, ",\"uk_version\":{\"major\":%u,\"minor\":%u}", uk.major, uk.minor);
   else
      out_lit(&o, ",\"uk_version\":null");
   if (drv_valid)
      out_fmt(&o, ",\"uk_driver_handshake\":{\"major\":%u,\"minor\":%u}", drv.major,
              drv.minor);
   else
      out_lit(&o, ",\"uk_driver_handshake\":null");
   out_fmt(&o, ",\"page_size\":%ld", sysconf(_SC_PAGESIZE));

   out_lit(&o, ",\"gpu\":");
   if (gp.valid)
      out_gpu(&o, &gp);
   else
      out_lit(&o, "null");

   out_lit(&o, ",\"csf\":");
   if (ci.valid)
      out_fmt(&o, "{\"glb_version\":\"0x%08x\",\"groups\":%u,\"streams\":%u,"
                  "\"cs_work_registers\":%u,\"instr_features\":\"0x%x\"}",
              ci.glb_version, ci.group_num, ci.total_stream_num, ci.cs_work_registers,
              ci.instr_features);
   else
      out_lit(&o, "null");

   out_lit(&o, ",\"vendor_ddk\":");
   out_str(&o, mods.release[0] ? mods.release : NULL);
   out_lit(&o, ",\"kernel_modules\":[");
   for (unsigned i = 0; i < mods.count; i++) {
      if (i)
         out_lit(&o, ",");
      out_str(&o, mods.name[i]);
   }
   out_lit(&o, "]");

   out_lit(&o, ",\"steps\":[");
   for (unsigned i = 0; i < p.nsteps; i++) {
      const struct step *s = &p.steps[i];
      if (i)
         out_lit(&o, ",");
      out_lit(&o, "{\"name\":");
      out_str(&o, s->name);
      out_fmt(&o, ",\"ok\":%s,\"error\":", s->ok ? "true" : "false");
      out_str(&o, s->error[0] ? s->error : NULL);
      out_fmt(&o, ",\"ms\":%.3f}", s->ms);
   }
   out_lit(&o, "]}");

   if (o.overflow) {
      json[0] = '\0';
      return -ENOSPC;
   }
   return 0;
}

#ifdef MALI_KBASE_PROBE_MAIN
int
main(void)
{
   static char buf[16384];
   struct timespec t0;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   int r = mali_kbase_probe(buf, sizeof(buf));
   double ms = ms_since(&t0);
   if (r) {
      fprintf(stderr, "mali_kbase_probe: %d\n", r);
      return 1;
   }
   puts(buf);
   fprintf(stderr, "total %.2f ms\n", ms);
   return 0;
}
#endif
