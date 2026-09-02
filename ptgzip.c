/*
 * (c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, GPL v2
 *
 * Note: some comment has been written by AI (diverse AI) because the AI was
 *       involved in writing templates about using specific functions or was
 *       asked to peer-review the code and/or commenting it. Sometimes also
 *       the code has been written by AI when like re-arranging the file by
 *       the list[] as source of truth, the implementation can vary but the
 *       conceptual problem as a deterministc mechanical solution. In that
 *       case my role as author was to remove the AI's overthinking related
 *       to general list[] corner cases which do not happen here by design.
 *       Memento: the AI is just a tool, as long as properly used, useful.
 */

#define _GNU_SOURCE

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <immintrin.h>
#include <semaphore.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>

#define LICENSE \
    "(c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, GPL v2"

#define ALWAYS_INLINE __attribute__ ((always_inline)) inline
#define ALIGNED4      __attribute__ ((aligned(4)))
#define ALIGNED8      __attribute__ ((aligned(8)))

#define mpages(_x) ((uint32_t)(_x) >> 12)
#define MPAGES(_x) mpages((_x) + 4095)

#define MAX_THREADS           16
#define MAX_CHUNK_SIZE       (1UL << 18)     /* max target size per segment */
#define MIN_CHUNK_SIZE       (1UL << 16)     /* 2x min gzip compress window */
#define PTGZ_HEADER_SIZE      30UL
#define PTGZ_LIST_START_OFF   20UL
#define PTGZ_LIST_MAX_WORDS   16380UL
#define PTGZ_HEADER_CURSIZE  (PTGZ_HEADER_SIZE + _g_ptgz_list_size)
#define PTGZ_HEADER_MAXSIZE  (PTGZ_HEADER_SIZE + (PTGZ_LIST_MAX_WORDS << 2))

static uint8_t __thread _g_ptgz_header[PTGZ_HEADER_MAXSIZE] ALIGNED4 = {0};

static int opt_stdout     = 0;    /* -c, --stdout, --to-stdout */
static int opt_help       = 0;    /* -h, --help */
static int opt_quiet      = 0;    /* -q, --quiet */
        // _g_compression_level ;    /* -#, --fast (=1), --best (=9) */
static int opt_keep       = 0;    /* -k, --keep */
static int opt_test       = 0;    /* -t, --test */
static int opt_force      = 0;    /* -f, --force */
static int opt_memory     = 0;    /* -m, --memory (KiB) */
static int opt_processes  = 0;    /* -p, --processes */
static int opt_verbose    = 0;    /* -v, --verbose */
static int opt_decompress = 0;    /* -d, --decompress */

typedef struct {
    pthread_t thr;
    const char *action;
    sem_t    *sem_ptr;
    uint8_t  *in;      /* pointer into mmap */
    uint8_t  *out;
    size_t    out_cap;
    int       infd;
    int       ofd;
    uint8_t   flags;
    size_t    read_len;
    uint8_t  *read;
//RAF: part that requires to be completely reset //
    int       idx;                               //
    uint8_t   state;                             //
    uint8_t   error;                             //
    size_t    in_len;                            //
    size_t    out_len;                           //
    off_t     out_off;                           //
    off_t     in_off;                            //
//RAF: part that requires to be completely reset //
    uint8_t   end;
} chunk_t ALIGNED4;

#define act_deflt "deflate"
#define act_inflt "inflate"

enum {
    b_flag_none = 0x00,
    b_flag_out  = 0x01,
    b_flag_in   = 0x02,
    b_flag_read = 0x04,
    b_flag_seek = 0x08,
    b_flag_free = 0x10,
};

#ifndef _DEBUG
#define _DEBUG    0 //xFF
#endif
#ifndef _USE_OPT
#define _USE_OPT  1 //RAF: no difference in gz speed
#endif
#ifndef _DNT_MMAP
#define _DNT_MMAP 1 //RAF: =1 to test mmap() failure, also file faster
#endif
#ifndef _USE_CPUM
#define _USE_CPUM 0 //RAF: cpu migration stabilise performance but slower
#endif              //     =0 to disable, =1 immediate, =2 before CPU workload
#ifndef _DO_OPTL
#define _DO_OPTL  7 //RAF: optional code, mask enabling bits: 1 2 4 (or 8)
#endif

#ifndef _DO_WRST
#define _DO_WRST  2 // 0: last run can be shorter than 1/2 _g_chunk_size
#endif              // 2: impose the same rule also to 2+ cycles runs
#ifndef _THR_WAIT
#define _THR_WAIT 0 // 1: wait for any of threads completes, 0: polling
#endif
#ifndef _USE_MMAP   // mmap() is performed by default, but it can fail
#define _USE_MMAP !_DNT_MMAP
#endif
#ifndef _USE_FREE
#define _USE_FREE 0 // free() isn't strictly necessary, but do testing
#endif
#ifndef _ONE_ZDF
#define _ONE_ZDF  1 //RAF: no difference in .gz size
#endif

#ifndef _ZLIB_MEM
#define _ZLIB_MEM 0 //RAF: =1 for zlib full agnosticy
#endif
#ifndef _USE_ZNG
#define _USE_ZNG  0 //RAF: just API, same speed/size
#else
#define _g_libz_name "zlib-ng"
#endif
#ifndef _USE_MNZ
#define _USE_MNZ  0 //RAF: libz/-ng by linker, miniz by compiler also
#endif

#ifndef _USE_UNGZ
#define _USE_UNGZ 0
#endif

#if   _USE_ZNG
  #include "zlib-ng.h"
  #define _deflate_init2 zng_deflateInit2
  #define _deflate_bound zng_deflateBound
  #define _deflate_end   zng_deflateEnd
  #define _deflate       zng_deflate
  #define _stream_t      zng_stream
#elif _USE_MNZ
  #include "miniz.h"
  #define _g_libz_name    "miniz"
  #define _deflate_init2  mz_deflateInit2
  #define _deflate_bound  mz_deflateBound
  #define _deflate_end    mz_deflateEnd
  #define _deflate        mz_deflate
  #define _stream_t       mz_stream
  /*
  #define Z_OK                  MZ_OK
  #define Z_FINISH              MZ_FINISH
  #define Z_NO_FLUSH            MZ_NO_FLUSH
  #define Z_DEFLATED            MZ_DEFLATED
  #define Z_STREAM_END          MZ_STREAM_END
  #define Z_DEFAULT_STRATEGY    MZ_DEFAULT_STRATEGY
  */
  #undef  Z_DEFAULT_COMPRESSION
  #define Z_DEFAULT_COMPRESSION 6
#else
  #include <zlib.h>
  #ifndef _g_libz_name
  #define _g_libz_name     "zlib"
  #endif
  #define _deflate_init2     deflateInit2
  #define _deflate_bound     deflateBound
  #define _deflate_end       deflateEnd
  #define _deflate           deflate
  #define _stream_t        z_stream
#endif
#ifndef _g_libz_name
#define _g_libz_name       "none"
#endif

#if   _USE_ZNG
  #define _inflate_init2 zng_inflateInit2
  #define _inflate       zng_inflate
  #define _inflate_end   zng_inflateEnd
#elif _USE_MNZ
  #define _inflate_init2 mz_inflateInit2
  #define _inflate       mz_inflate
  #define _inflate_end   mz_inflateEnd
#else
  #define _inflate_init2 inflateInit2
  #define _inflate       inflate
  #define _inflate_end   inflateEnd
#endif

#define zbuf_max_size(_len) ((size_t)(_len) + ((_len) >> 9) + 256)
#define WBUF_MAX_SIZE zbuf_max_size(_g_chunk_size)

#define is_outbuf_freeable(_c) (_c->out && !(_c->flags & b_flag_out))
#define  is_inbuf_freeable(_c) (_c->in  && !(_c->flags & b_flag_in ))

#define _print2(fmt...) while(!opt_quiet) { fprintf(stderr, fmt); break; }
#define _cpu_relax() do { if(sched_yield()) usleep(1); } while(0)
#define _int_div(_a, _b) (((_a) + (_b) - 1) / (_b))

static uint8_t *_g_read_mmap_base = NULL;
static uint8_t *_g_out_mmap_base  = NULL;
static off_t _g_read_file_size    = 0;
static size_t _g_ptgz_list_size   = 0;
static size_t _g_chunk_size       = 0;
static int _g_tot_chunks          = 0;
static int _g_compression_level   = 6;
static unsigned _g_cpu_procs      = 0;
static off_t _g_first_offeset     = 0; // RAF: eq. to PTGZ_HEADER_CURSIZE

static bool chunk_read(chunk_t *c);
static bool chunk_write(chunk_t *c);
static size_t full_write(int ofd, const void *buf, size_t len);
static uint8_t *ptgz_header_read(uint8_t *buf, uint16_t *nbytes, uint32_t *size);
static const uint8_t *ptgz_header_make(uint32_t ctm, uint32_t in_len, int16_t size);

// =============================================================================

static
size_t full_read(int fd, const void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;

    while (len > 0) {
        ssize_t w = read(fd, p, len);
        if (!w) break;
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("read");
            exit(-1);
        }
        p   += w;
        len -= w;
    }

    return p - (uint8_t *)buf;
}

static ALWAYS_INLINE
int full_sem_wait(sem_t *sem_ptr)
{
    int ret;

    do {
        _cpu_relax();
        ret = sem_wait(sem_ptr);
        if(!ret) return 0;
    } while(errno == EINTR || errno == EAGAIN); //EINVAL

    perror("sem_wait");
    return 1;
}

// -----------------------------------------------------------------------------
// Thread workers: elaborate a chunk directly to its output buffer
// -----------------------------------------------------------------------------

static ALWAYS_INLINE
void setcpu(unsigned idx)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(idx % _g_cpu_procs, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

static ALWAYS_INLINE
void chunk_dispose(chunk_t *c, uint8_t err)
{
    if (c->flags & b_flag_free)
    {
        if (is_outbuf_freeable(c))
        {
            free(c->out);
            c->out = NULL;
        }
        if (is_inbuf_freeable(c))
        {
            free(c->in);
            c->in = NULL;
        }
        memset(c, 0, sizeof(chunk_t));
    } else {
        c->thr = 0;
        c->state = 0;
    }
    c->error = err;
}

#define thread_inflate thread_zxflate
#define thread_deflate thread_zxflate

static void *thread_zxflate(void *arg)
{
    int ret;
    chunk_t *c = arg;
    _stream_t strm = {0};

#if _USE_CPUM == 1
    setcpu(c->idx);
#else
    /*_cpu_relax();*/
#endif

    if(!c->in_len) {
        c->state = 3; // no data, task completed as void
        goto release;
    }

    if (c->infd != STDIN_FILENO)
    {
        if(chunk_read(c))
        {
            if (!c->in_len) {
                //RAF: potentially useful but ending or aborting
                //c->flags |= b_flag_free;
                chunk_dispose(c, 8);
            }
            return NULL;
        }
        c->flags |= b_flag_read;
    }

    const bool dflt = (c->action[0] == 'd');

    /* 1. GZIP FORMAT: 15 + 16 is mandatory otherwise deflateInit() produces
     *    RFC-1950 zlib format, not RFC-1952 gzip. Only RFC-1952 output can
     *    be concatenated into a valid .gz file.
     */
    if(dflt)
        ret = _deflate_init2(&strm, _g_compression_level,
            Z_DEFLATED, 15 + 16, 7, Z_DEFAULT_STRATEGY);
    else
        ret = _inflate_init2(&strm, 15 + 16);
    if (ret != Z_OK)
    {
        fprintf(stderr, "%s_init2 failed: %d\n", c->action, ret);
        c->error = -3;
        goto release;
    }

#if _ZLIB_MEM
    /* 2. OUTPUT BUFFER: must be deflateBound(), never just c->in_len.
     *    Incompressible data EXPANDS by 0.1% + headers, circa. Hence,
     *    c->in_len alone guarantees a buffer overrun on random bytes.
     */
    if (!c->out)
    {
        /*
        * RAF: potentially the bound could be larger than the WBUF_MAX_SIZE
        *      in case the library differs from the current ones tested and
        *      in such a case there is a good canche that USE_MMAP would fail
        *      silently, in some corner cases when writing out of bond will
        *      corrupt data and thus creating a corrupted gzip archive or when
        *      the ending bound would be violated and thus the kernel SEGVDEF.
        */
        if(dflt)
            c->out_cap = _deflate_bound(&strm, c->in_len);
        else
            c->out_cap = _inflate_bound(&strm, c->in_len);
        if (posix_memalign((void **)&c->out, 64, c->out_cap))
            c->out = NULL;
        if (!c->out) {
            perror("malloc");
            c->error = -2;
            return NULL;
        }
    }
#endif
    strm.next_in   = c->in;
    strm.avail_in  = c->in_len;
    strm.next_out  = c->out;
    strm.avail_out = c->out_cap;

#if _USE_CPUM == 2
    setcpu(c->idx);
#endif

    /* 3. COMPRESSION LOOP:
     *    - Feed all input with Z_NO_FLUSH until avail_in == 0.
     *    - Then Z_FINISH until deflate returns Z_STREAM_END.
     */
#if _ONE_ZDF
#else
    do {
        if(dflt)
            ret = _deflate(&strm, Z_NO_FLUSH);
        else
            ret = _inflate(&strm, Z_NO_FLUSH);
    } while (ret == Z_OK);
    if (ret != Z_STREAM_END)
#endif
    if(dflt)
        ret = _deflate(&strm, Z_FINISH);
    else
        ret = _inflate(&strm, Z_FINISH);

#if _DEBUG & 0x01 // -----------------------------------------------------------
if(strm.total_out)
    fprintf(stderr, "%s> in: %u / %lu out: %lu / %lu\n", c->action,
        strm.avail_in,  c->in_len, strm.total_out, c->out_cap);
#endif // ----------------------------------------------------------------------

    c->out_len = strm.total_out;
    if (ret != Z_STREAM_END /*&& ret != Z_BUF_ERROR
    &&  ret != Z_DATA_ERROR*/ && ret != Z_OK
    ){
        fprintf(stderr, "%s failed: %d\n", c->action, ret);
        c->error = ret;
    }

    /* 4. CLEANUP: always call deflateEnd() to free internal buffers.
     *             Skipping to call it leaks several KiB per chunk.
     */
endfnc:
    if(dflt)
        _deflate_end(&strm);
    else
        _inflate_end(&strm);
    c->state = 2;
    if (c->ofd != STDOUT_FILENO
    && (!_g_out_mmap_base || !_USE_MMAP)
    ){
        if (c->sem_ptr){
            sem_post(c->sem_ptr);
            c->sem_ptr = NULL;
        }
        c->error |= chunk_write(c);
    }
    c->state = 3;

release:
    if (c->sem_ptr) {
        sem_post(c->sem_ptr);
        c->sem_ptr = NULL; // Always NULL at the end of the work
    }
    return NULL;
}

static ALWAYS_INLINE
bool chunk_read(chunk_t *c)
{
    if(!c->in_len) return 0;

    if (c->infd == STDIN_FILENO) {
        size_t len = c->read_len;
        if (len > c->in_len)
        {
            fprintf(stderr,
                "BUG: c->read_len > c->in_len: %lu vs %lu\n",
                    c->read_len, c->in_len);
            return 1;
        }
        if (len) {
            __builtin_memcpy(c->in, c->read, len);
            len += full_read(c->infd, &c->in[len], c->in_len - len);
            c->in_len = len;
        } else {
            c->in_len = full_read(c->infd, c->in, c->in_len);
        }
    } else // The operations below can be post-poned w/ a thread
    if(_g_out_mmap_base) {
        uint8_t *src = _g_read_mmap_base + c->in_off;
        __builtin_memcpy(c->in, src, c->in_len);
    } else {
        off_t off = c->in_off;
        size_t len = c->in_len;
        uint8_t *p = c->in;
        while (len > 0) {
            ssize_t w = pread(c->infd, p, len, off);
            if (!w) break;
            if (w < 0) {
                if (errno == EINTR) continue;
                perror("p/read");
                return -1;
            }
            p   += w;
            off += w;
            len -= w;
        }
        c->in_len -= len;
    }

    return 0;
}

static ALWAYS_INLINE
size_t full_pwrite(int ofd, uint8_t *p, size_t size, off_t off)
{
#if 0 // -----------------------------------------------------------------------
static unsigned idx = 0;
fprintf(stderr, "fpw> call: %u, sze: %lu, off: %lu\n", idx++, size, off);
#endif // ----------------------------------------------------------------------

    size_t len = size;

    while (len > 0) {
        ssize_t w = pwrite(ofd, p, len, off);
        if (w <= 0) {
            if (errno == EINTR) continue;
            perror("p/write");
            break;
        }
        p   += w;
        off += w;
        len -= w;
    }

    return size - len;
}

static ALWAYS_INLINE
bool chunk_write(chunk_t *c)
{
#if 0 // -----------------------------------------------------------------------
fprintf(stderr, "ckw>  idx: %u, sze: %lu, off: %lu\n",
    c->idx, c->out_len, c->out_off);
#endif // ----------------------------------------------------------------------

    if (!c->ofd) return 0;

    if (c->ofd == STDOUT_FILENO)
        return (full_write(c->ofd, c->out, c->out_len) < 0);

    if(0 && c->flags & b_flag_seek) {
        if (ftruncate(c->ofd, c->out_off + c->out_len) < 0) {
            perror("ftruncate wr");
            return 1;
        }
    }

    if(_g_out_mmap_base) {
        uint8_t *dst = _g_out_mmap_base + c->out_off;
        return !__builtin_memcpy(dst, c->out, c->out_len);
    } else {
        return !full_pwrite(c->ofd, c->out, c->out_len, c->out_off);
    }
}

static
size_t full_write(int ofd, const void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;

    if(!ofd) return len;

    while (len > 0) {
        ssize_t w = write(ofd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write");
            exit(-1);
        }
        p   += w;
        len -= w;
    }

    return p - (uint8_t *)buf;
}

static ALWAYS_INLINE
size_t full_rcopy(int ofd, off_t off_out, off_t off_in, size_t size)
{
    size_t len = size;

    if(!ofd) return size;

    while (len > 0) {
        ssize_t w = copy_file_range(ofd,
            &off_in, ofd, &off_out, len, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("copy_file_range");
            return -1;
        }
        len -= w;
    }

    return size - len;
}

static
void chunk_init(chunk_t *c, int idx, int ofd,
    int infd, sem_t *sem_ptr, size_t out_size)
{
    static ALIGNED4 chunk_t m = {0};

    if(m.state == 0) {
        m.state = 1;
        if (_USE_FREE) m.flags = b_flag_free;
        m.sem_ptr = _THR_WAIT ? sem_ptr : NULL;
        m.out_cap = out_size ?: WBUF_MAX_SIZE;
        m.out_off = out_size ? 0 : PTGZ_HEADER_CURSIZE;
        m.in_len = _g_chunk_size;
        m.infd = infd;
        m.ofd = ofd;
    }

    __builtin_memcpy(c, &m, sizeof(m));

    c->idx = idx;
    c->out_off += c->out_cap  * idx;
    c->in_off = _g_chunk_size * idx;

    if (infd != STDIN_FILENO && idx + 1 == _g_tot_chunks)
        c->in_len = (size_t)_g_read_file_size - c->in_off;
}

#define inflate_chunk_init zxflate_chunk_init
#define deflate_chunk_init zxflate_chunk_init

/*
 * RAF: currently the .out_cap exists but always set to WBUF_MAX_SIZE, which
 *      makes the .out_cap redundant while _g_out_mmap_base would be likely
 *      a more usful member of the chunk_t structure. A revision is needed
 *      after the development is completed to get rid off of global variables
 *      in favor of a data structure that can refers also to its own thread_t.
 */
static
int zxflate_chunk_init(chunk_t *c)
{
#if _USE_MMAP
    /* Assign output pointer inside mapped output file space */
    if (_g_out_mmap_base) {
        c->out = _g_out_mmap_base + c->out_off;
        c->flags |= b_flag_out;
    }
    else
#endif
#if _ZLIB_MEM
    c->out = NULL;
#else
    if (!c->out)
         if (posix_memalign((void **)&c->out, 64, c->out_cap))
              c->out = NULL;
    if (!c->out) {
        perror("posix_memalign");
        exit(-1);
    }
#endif

#if _USE_MMAP
    if (_g_read_mmap_base) {
        c->in = _g_read_mmap_base + c->in_off;
        c->flags |= b_flag_in;
    } else
#endif
    {
        if (!c->in)
             if (posix_memalign((void **)&c->in, 64, c->in_len))
                  c->in = NULL;
        if (!c->in) {
            perror("posix_memalign");
            exit(-1);
        }
        if (c->infd == STDIN_FILENO)
        {
            if (chunk_read(c))
                c->error |= 8;
            else
                c->flags |= b_flag_read;
        }
#if _DEBUG & 0x02 // -----------------------------------------------------------
fprintf(stderr, "inp2> thr(%04d): read = %lu, off: %lu, err: %d\n",
    c->idx, c->in_len, c->in_off, c->error);
#endif  // ---------------------------------------------------------------------
        if (!c->in_len) {
            //RAF: much probably EOF, free() is irrelevant here
            //c->flags |= b_flag_free;
            chunk_dispose(c, c->error);
            return 1;
        }
    }

    return 0;
}

static ALWAYS_INLINE
void chunk_deflate_start(pthread_t *p, chunk_t *c)
{
    c->action = act_deflt;
    if (!pthread_create(p, NULL, thread_deflate, c))
        return;

    perror("pthread_create");
    exit(1);
}

static ALWAYS_INLINE
void chunk_inflate_start(pthread_t *p, chunk_t *c)
{
    c->action = act_inflt;
    if (!pthread_create(p, NULL, thread_inflate, c))
        return;

    perror("pthread_create");
    exit(1);
}

static void *thread_chunk_write(void *arg)
{
    chunk_t *c = arg;

    #if 0 // _USE_CPUM // RAF: slower in this specific corner case
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(c->idx % _g_cpu_procs, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    #endif

    c->error |= chunk_write(c);
    c->state = 3;
    return NULL;
}

// =============================================================================
// gunzip
// =============================================================================

/* RAF ptgzip v0.5
 *******************************************************************************

  $ make _test-clean _test-basic speed-gunzp

  CURRENT TOP-SPEED (2710)

  Baseline sum-up 2479 on 4 speeds, top speed 2710 which is +9.3%:

  278937600 bytes (279 MB, 266 MiB) copied, 0.414000 s, 674 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.411370 s, 678 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.411199 s, 678 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.410411 s, 680 MB/s

  INPUT READ-AHEAD  (2680)

  278937600 bytes (279 MB, 266 MiB) copied, 0.414614 s, 673 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.421616 s, 662 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.413728 s, 674 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.415963 s, 671 MB/s

  CHUCKS SPLITTING

  #0 (2697)
  278937600 bytes (279 MB, 266 MiB) copied, 0.414493 s, 673 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.412130 s, 677 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.413649 s, 674 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.414243 s, 673 MB/s

  #1 (2383)
  278937600 bytes (279 MB, 266 MiB) copied, 0.466169 s, 598 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.463225 s, 602 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.463858 s, 601 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.478978 s, 582 MB/s

  #2 (2407)
  278937600 bytes (279 MB, 266 MiB) copied, 0.468027 s, 596 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.460823 s, 605 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.463540 s, 602 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.461731 s, 604 MB/s

  #3 (2485)
  278937600 bytes (279 MB, 266 MiB) copied, 0.447942 s, 623 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.448146 s, 622 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.447137 s, 624 MB/s
  278937600 bytes (279 MB, 266 MiB) copied, 0.452564 s, 616 MB/s

  NOTE:

  A quick confrontation of ptgzip (zlib-ng), plgzip (zlib) and pigz shows
  the current performance of the sequential inflate is almost totally due
  the adoption of the zlib-ng. Unsurprisingly, since no extra-parallelism
  has been introduced yet, apart from threading the writing.

  $ make _test-clean _test-basic speed-gunzp CMD2T=/bin/pigz
  $ make _test-clean _test-basic speed-gunzp CMD2T=./plgzip
  $ make _test-clean _test-basic speed-gunzp

  ptgzip -d is 1.7x faster than pigz, w/o PTGZ format support which will
  enable ptgzip -d to concurrently write on the inflating file also leveraging
  mmap() if enabled or available, while write on STDOUT will remain necessarily
  sequentially but the pthread_jon() can be demanded to a supervisor thread.

  While the inflate_stream() will continue to process chunksi in a sequential
  manner which is compatible with a STDIN input stream of data. Otherwise, also
  the chunks inflating could be parallelised as already compress() does.

  Finally, ptgzip -d w/o PTGZ is 3.0x faster than gzip, on i5-8365.

  TODO:
  - Stop seeking when a 2nd chunk isn't found in MAX_CHUNK_SIZE, isn't PTGZ

 *******************************************************************************
*/

#define size_by_blocks(_len) ((((_len) + 511) >> 9) << 9)
#define zread_max_size(_len) (zbuf_max_size(_len) + PTGZ_HEADER_SIZE + 4)
#define UNZIN_CHUNK_SIZE size_by_blocks(zread_max_size(MAX_CHUNK_SIZE))
#define UNOUT_CHUNK_SIZE                               MIN_CHUNK_SIZE

#if _USE_UNGZ //////////////////////////////////////////////////////////////////

#define UNGZ_IMPLEMENTATION
#include "ungz/ungz.h"

/* Context structure passed into the pigz_reader callback */
typedef struct {
    int fd;
    uint8_t *buffer;
    size_t buf_size;
    size_t pre_size;
} reader_ctx_t;

/* Callback function expected by ungz (pigz_reader) */
static const char* ungz_file_reader(void *opaque, uint64_t *len)
{
    reader_ctx_t *ctx = (reader_ctx_t *)opaque;
    ssize_t bytes_read = full_read(ctx->fd,
        ctx->buffer   + ctx->pre_size,
        ctx->buf_size - ctx->pre_size);
    bytes_read +=       ctx->pre_size ;
                        ctx->pre_size = 0;

    if (bytes_read < 0) {
        *len = 0;
        return NULL;
    }

    *len = (uint64_t)bytes_read;
    return (const char *)ctx->buffer;
}

static int ungz_inflate_stream(int infd, int ofd, size_t in_size,
    size_t out_size, int8_t *buf, size_t buf_size, bool seek, void *ptbl)
{
    int ret = 0;
    uint8_t *inbuf = NULL;
    uint8_t *outbuf = NULL;
    reader_ctx_t reader_ctx;
    pigz_state state;
    chunk_t c = {0};

    if (posix_memalign((void **)&inbuf,  64, UNZIN_CHUNK_SIZE) ||
        posix_memalign((void **)&outbuf, 64, UNOUT_CHUNK_SIZE)) {
        perror("malloc");
        return -1;
    }

    reader_ctx.fd       = infd;
    reader_ctx.buffer   = inbuf;
    reader_ctx.buf_size = UNZIN_CHUNK_SIZE;
    reader_ctx.pre_size = buf_size;
    if (buf_size)
        __builtin_memcpy(inbuf, buf, buf_size);

    /* Initialize chunk configuration */
    c.flags = b_flag_out | b_flag_in;
    if (!seek) c.flags |= b_flag_seek;
    c.out = outbuf;
    c.ofd = ofd;

    pigz_init(&state, &reader_ctx, ungz_file_reader);

    while (1) {
        /* Determine how many decompressed
           bytes are currently available */
        uint64_t avail = pigz_available(&state);

        if (state.status == PIGZ_STATUS_EOF)
            break;
        /* Check termination or error states */
        if (state.status < PIGZ_STATUS_EOF) {
            if (state.status == PIGZ_STATUS_BAD_HEADER && !avail)
                break;
            ret = state.status;
            fprintf(stderr, "inflate error: %d (avail: %lu)\n", ret, avail);
            break;
        }

        /* Clamp output chunk to worker buffer capacity */
        uint64_t chunk_len = (avail > UNOUT_CHUNK_SIZE)
                           ?  UNOUT_CHUNK_SIZE : avail;

        /* Retrieve pointer to decompressed data
           directly from ungz internal state */
        const char *decomp_ptr = pigz_consume(&state, chunk_len);
        if (!decomp_ptr) {
            ret = chunk_len;
            break;
        }

        if (ofd == STDOUT_FILENO) {
            ret = full_write(ofd, decomp_ptr, chunk_len);
            if(ret < 0) goto endfunc;
            else ret = 0;
        } else {
            if (c.thr) {
                pthread_join(c.thr, NULL);
                c.thr = 0;
            }
            if(chunk_len) {
                /* Copy available output to chunk buffer
                   for multi-threaded processing */
                __builtin_memcpy(outbuf, decomp_ptr, chunk_len);
                c.out_len = chunk_len;
                // RAF, TODO: check if __thread works as supposed
                //      otherwise create a duplicate of chunk_t c
                if (pthread_create(&c.thr, NULL,
                    thread_chunk_write, &c)
                ){
                    perror("pthread_create");
                    ret = -1;
                    goto endfunc;
                }
                c.out_off += chunk_len;
                c.idx++;
            }
        }
    }

endfunc:
    if (c.thr) {
        pthread_join(c.thr, NULL);
    }
#if _USE_FREE
    free(inbuf);
    free(outbuf);
#endif
    return ret;
}

#define _inflate_stream ungz_inflate_stream

#else //////////////////////////////////////////////////////////////////////////

#include <endian.h>

static ALWAYS_INLINE
uint32_t chunk_seeker(register uint8_t *p, const uint32_t r)
{
    p++;
    for (register uint32_t n = 1; n < r; n++, p++) {
#if __BYTE_ORDER == __BIG_ENDIAN
        if ((*(uint32_t *)p & 0xFFFFFF00) == 0x1F8B0800)
#else
        if ((*(uint32_t *)p & 0x00ffffff) == 0x00088b1f)
#endif
        return n;
    }
    return 0;
}

/**
 * AVX2-accelerated Gzip header scanner.
 * Searches for sequence: 0x1F 0x8B 0x08 starting from offset 1 up to 'r - 3'.
 * Returns offset 'n' if found, otherwise 0.
 */

static ALWAYS_INLINE __attribute__((target("avx2")))
uint32_t chunk_seeker_avx2(const uint8_t *p, const uint32_t r) {
    if (r < 4) return 0;

    const uint32_t maxn = r - 3;
    uint32_t n = 1;

    // 1. PROLOGUE: Scalar search until (p + n) reaches 32-byte alignment
    for (; n < maxn && (((uintptr_t)(p + n)) & 31) != 0; n++)
        if (p[n] == 0x1F && p[n + 1] == 0x8B && p[n + 2] == 0x08)
            return n;

    // 2. VECTOR SCAN: Aligned loads for maximum L1/L2 bandwidth
    const __m256i v_b0 = _mm256_set1_epi8(0x1F);
    const __m256i v_b1 = _mm256_set1_epi8(0x8B);
    const __m256i v_b2 = _mm256_set1_epi8(0x08);

    for (; n + 31 < maxn; n += 32) {
        // Aligned load on chunk0 and overlapping chunk1 and chunk2
        // use unaligned loads offset by 1 and 2 bytes relative to
        // the aligned chunk0 pointer
        __m256i chunk0 = _mm256_load_si256 ((const __m256i *)(p + n    ));
        __m256i chunk1 = _mm256_loadu_si256((const __m256i *)(p + n + 1));
        __m256i chunk2 = _mm256_loadu_si256((const __m256i *)(p + n + 2));

        __m256i m0 = _mm256_cmpeq_epi8(chunk0, v_b0);
        __m256i m1 = _mm256_cmpeq_epi8(chunk1, v_b1);
        __m256i m2 = _mm256_cmpeq_epi8(chunk2, v_b2);

        __m256i match = _mm256_and_si256(_mm256_and_si256(m0, m1), m2);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(match);

        if (mask != 0) return n + __builtin_ctz(mask);
    }

    // 3. EPILOGUE: Scalar tail loop for remaining unaligned trailing bytes
    for (; n < maxn; n++)
        if (p[n] == 0x1F && p[n + 1] == 0x8B && p[n + 2] == 0x08)
            return n;

    return 0;
}

#if 0 //_USE_MNZ
#define _SEEKER_FUNC  5
#define _READ_AHEAD   1
#else
#define _SEEKER_FUNC  0
#define _READ_AHEAD   0
#endif

static int zlib_inflate_stream(int infd, int ofd, size_t in_size,
    size_t out_size, int8_t *buf, size_t buf_size, bool seek, void *ptbl)
{
    size_t r, n, f, w = 0, set = 0, rmn = 0;
    uint8_t * inbuf = NULL;
    uint8_t *outbuf = NULL;
    int ret, eof = 0, nchunks = 0;
    _stream_t strm = {0};
    chunk_t c = {0};


    r = size_by_blocks(zread_max_size(MAX_CHUNK_SIZE));
    if (posix_memalign((void **)&inbuf,  64, r)
    ||  posix_memalign((void **)&outbuf, 64, r)
    ){
        perror("malloc");
        return -1;
    }
    if(buf_size) {
        __builtin_memcpy(inbuf, buf, buf_size);
        w = buf_size;
    }
#if 0 // -----------------------------------------------------------------------
fprintf(stderr, " buf: %p, buf_size: %lu, w: %lu\n", buf, buf_size, w);
#endif // ----------------------------------------------------------------------

    ret = _inflate_init2(&strm, 15 + 16);
    if (ret != Z_OK) {
        perror("inflateInit2");
        goto endfunc;
    }

    c.flags = b_flag_out | b_flag_in;
    if(seek) c.flags |= b_flag_seek;
    strm.next_in  = inbuf;
    strm.avail_in = w;
    c.ofd = ofd;

    while (1) {
        n = rmn + strm.avail_in;
        #if _READ_HEAD // read-ahead is not convenient, at the best match 1:1
        if ( ( ptr && _SEEKER_FUNC && 1) //RAF: not yet, chunk read is needed
        ||   (!eof && strm.avail_in < (in_size >> 1))
        ){ // feed the input buffer
        #else
        if (!strm.avail_in) { // feed the input buffer
        #endif
            if (n && set)
                __builtin_memmove(inbuf, &inbuf[set], n);
            r = n + full_read(infd, inbuf + n, in_size - n);

            if (!r) eof = 1; // EOF
            else strm.avail_in += r;

            set = 0;
            rmn = 0;
            strm.next_in = inbuf;
#if 0 // -----------------------------------------------------------------------
fprintf(stderr, "mgk: 0x%08x\n", *(uint32_t *)inbuf);
#endif // ----------------------------------------------------------------------
            #if   _SEEKER_FUNC == 1
            set = chunk_seeker(inbuf, r - 3);
            if (set) {
                strm.avail_in = set;
                rmn = r - set;
            } else {
                strm.avail_in = r;
            }
            #elif _SEEKER_FUNC == 2
            strm.avail_in = r;
            register uint8_t *p = inbuf + 1;
            r -= 3; // to avoid the buffer overflow
            for (register uint32_t n = 1; n < r; n++, p++) {
                #if __BYTE_ORDER == __BIG_ENDIAN
                if ((*(uint32_t *)p & 0xFFFFFF00) == 0x1F8B0800)
                #else
                if ((*(uint32_t *)p & 0x00ffffff) == 0x00088b1f)
                #endif
                {
                    strm.avail_in = n;
                    rmn = r - n + 3;
                    set = n;
                    break;
                }
            }
            #elif _SEEKER_FUNC == 3
            strm.avail_in = r;
            r -= 2; // to avoid the buffer overflow
            for (size_t n = 1; n < r; n++) {
                if (inbuf[n  ] == 0x1f
                &&  inbuf[n+1] == 0x8b
                &&  inbuf[n+2] == 0x08
                ){
                    strm.avail_in = n;
                    rmn = r - n + 2;
                    set = n;
                    break;
                }
            }
            #elif _SEEKER_FUNC == 4
            /* RAF
             * There is a chance that the 2nd chunk of two is found but this
             * isn't a such trouble rather than a equilibration of the work
             * among threads: similar loads provides a more balanced hurd.
             *
             * WARNING: for testing only, it is incompatible with PTGZ full
             *          format specification which includes furhter tables.
             *
             * The seeker function #4 is competitive with #5 even when AVX2
             * is available, therefore the #3 should be used when another
             * unmissable PTGZ header is expected to be found next.
             */
            strm.avail_in = r;
            w = (r < 4) ? 0 : (r >> 1) - 2;
            n = w + 1;
            f = 0;
            while(w) {
                if ((inbuf[w  ] == 0x1f
                &&   inbuf[w+1] == 0x8b
                &&   inbuf[w+2] == 0x08)) {
                    f = w;
                    break;
                }
                if ((inbuf[n  ] == 0x1f
                &&   inbuf[n+1] == 0x8b
                &&   inbuf[n+2] == 0x08)) {
                    f = n;
                    break;
                }
                w--;
                n++;
            }
            if(f) {
                strm.avail_in = f;
                rmn = r - f;
                set = f;
            }
            #elif _SEEKER_FUNC == 5
            /* RAF
             * The results indicate that AVX2 chuck seeker makes inflate 2%
             * slower compared to do not seek at all and process everything
             * sequentially. However, elaborating by chunks allows to use
             * libdeflate which is expected to be even faster than zlib-ng
             * and enables the ability to increase the parallelization of
             * the workload which currently isn't specifically leveraged
             * as it has been for deflate.
             */
            strm.avail_in = r;
            set = chunk_seeker_avx2(inbuf, r);
            if(set) {
                strm.avail_in = set;
                rmn = r - set;
            }
            #endif
        }
        if (eof && !strm.avail_in)
            break;

        strm.next_out  = outbuf;
        strm.avail_out = out_size;

        if(strm.avail_in || ret == Z_BUF_ERROR) {
            ret = _inflate(&strm, Z_NO_FLUSH);
            if (ret < 0 && ret != Z_BUF_ERROR) { // -5
                if (ret == Z_DATA_ERROR) { // -3
#if 0 // -----------------------------------------------------------------------
fprintf(stderr, "inflate mnz: %d (avail: %u, %u, write: %ld), zse: %d\n",
    ret, strm.avail_in, strm.avail_out, out_size - strm.avail_out, Z_DATA_ERROR);
#endif // ----------------------------------------------------------------------
#if _USE_MNZ
                    //ret = Z_STREAM_END;
                    //goto do_stream_end;
#endif
                    ret = 0;
                    break; /* ignore trailing junk/ptgz table */
                }
                fprintf(stderr, "inflate error: %d (avail: %u, %u)\n",
                    ret, strm.avail_in, strm.avail_out);
                break;
            }
        }


        w = out_size - strm.avail_out;
        if(1 || ofd == STDOUT_FILENO) {
            if (w && full_write(ofd, outbuf, w) < 0) {
                ret = -1;
                goto endfunc;
            }
        } else {
            if (c.thr) {
                pthread_join(c.thr, NULL);
                //fprintf(stderr, "state: %u\n", c.state);
                c.out_off += c.out_len;
                c.thr = 0;
                c.idx++;
            }
            if(w) {
                c.out_len = w;
                c.out = outbuf;
                // RAF, TODO: check if __thread works as supposed
                //      otherwise create a duplicate of chunk_t c
                if (pthread_create(&c.thr, NULL,
                    thread_chunk_write, &c)
                ){
                    perror("pthread_create");
                    return -1;
                }
            }
        }

        if (ret == Z_STREAM_END) {
do_stream_end:
            ret = 0;
            nchunks++;
            if(_USE_MNZ && !w)
                break;
            _inflate_end(&strm);
            ret = _inflate_init2(&strm, 15 + 16);
#if 0 // -----------------------------------------------------------------------
fprintf(stderr, "  init2 mnz: %d (avail: %u, %u, write: %ld)\n",
    ret, strm.avail_in, strm.avail_out, out_size - strm.avail_out);
#endif // ----------------------------------------------------------------------
            if (ret != Z_OK) {
                ret = 1;
                break;
            }
        }
    }

endfunc:
    if (c.thr) pthread_join(c.thr, NULL);
    #if _USE_FREE
    _inflate_end(&strm);
    free(inbuf);
    free(outbuf);
    #endif
    if (ret)
        _print2("%s error: %d\n", __func__, ret);
    return ret;
}

#define _inflate_stream zlib_inflate_stream

#endif /////////////////////////////////////////////////////////////////////////

// =============================================================================
// Prep
// =============================================================================

#define _mpceil(_x) (((_x) + 4095) >> 12)

#define TABLE_ITEMS ((uint32_t)_g_tot_chunks + 4)
#define TABLE_BSIZE ((TABLE_ITEMS) << 2)

/* RAF ptzip v0.5
 *******************************************************************************

  This structure implies a table which can map a certain range, which
  determines also the  max file size, by the following consideration:

  -> max size of chunk  * max number of words = max input  range
  -> max size of offset * max number of words = max output range

  typedef struct ALIGNED4 {
    uint32_t  align4 // the content should start at 32-bit aligned file address
    uint16_t  magic1 // the magic number is divided in two halves bc appending
    uint16_t  npages // the size of the read expressed 4KiB memory pages 2^12
    uint32_t *chunks // pointer to the list of the chunks addresses in bytes
    uint16_t  nwords // number - 2 of words the list contains: 0,1 not useful
    uint16_t  magic2 // the magic number is divided in two halves bc appending
  } __attribute__ ((packed)) pgunz_t

  Currently these two numbers are defined by the following choices:

  -> 2^16 * 4KiB * 2^16 = 2^44 =  16 TB
  -> 2^32        * 2^16 = 2^48 = 256 TB

  There is an evident unbalance about these two ranges and moreover
  the extention is way bigger than the common need. However in design
  a format, its ability to scale fitting future needs isn't optional.

  Using two 32-bit plain values, both ranges raise to 64-bit addressing space.

  Finally, when reading by STDIN, the size of the table cannot be determined
  beforehands, then a linked list is required but it will consume 3x memory
  compared a plain buffer which the max size is 16GB. While compressing N
  chunks at time implies a limited amount of RAM despite the file in input
  creating such table requires, in the worst case, write a separate file.

  For a 4GB (2^32) file in input, divided by 256KiB (2^18) chunks, the list
  size is about 2^14 words equivalent to 64KiB which is a volume of memory
  that it is fine to pre-allocate on end-users systems.

 *******************************************************************************
 */

typedef struct ALIGNED4 {
    uint32_t   *list;  // the pointer to the start of chunks addresses list
    uint32_t    size;  // allocated number of the list buffer in 4B words
    void       *next;  // pointer to the next buffer to extend the previous
} __attribute__ ((packed)) pgunz_link_t;

typedef struct ALIGNED4 {
    pgunz_link_t cur;  // pointer to the linked list
    uint32_t  chksum;  // the sum of all the words table records should be zero
    uint32_t  nwords;  // number of the words the list contains: 0,1 not useful
    uint32_t  bufsze;  // the input buffer (chunk) size expressed in bytes 2^32
    uint32_t  magicw;  // the magic word closing the format, but it can be 16_t
    /* list stats here */
} __attribute__ ((packed)) pgunz_t;

typedef struct ALIGNED4 {
    pgunz_t *ptbl;
    unsigned nthr;
    uint32_t nidx;
    size_t isze;
    size_t osze;
    size_t olen;
    size_t blen;
    int vlvl;
    int infd;
} __attribute__ ((packed)) vrbout_t;

const uint8_t ptgz_magic_str[4] = { "ptgz" };

static
pgunz_t *create_pgunz_table(uint32_t nwords)
{
    pgunz_t *p;
    uint8_t *u;
    uint32_t n, len;

    if(!nwords)
        nwords = PTGZ_LIST_MAX_WORDS;
    n   = _mpceil(nwords << 2);
    len = sizeof(pgunz_t) + n;
    p   = malloc(len);
    if(!p) {
        perror("malloc");
        return p;
    }

    memset(p, 0, len);
    p->nwords = nwords;
    p->cur.size = nwords;
    u = (uint8_t *)p;
    p->cur.list = (uint32_t *)(u + sizeof(pgunz_t));
    u = (uint8_t *)&p->magicw;
    for (int i = 0; i < 4; i++)
        u[i] = ptgz_magic_str[i];

    return p;
}

static ALWAYS_INLINE
uint8_t *finalize_pgunz_table(pgunz_t *ptbl, size_t *len)
{
    int i;
    uint32_t sum = 0;
    uint32_t *p = &ptbl->chksum;
    uint32_t *list = ptbl->cur.list;
    uint32_t nwords = ptbl->nwords;
    uint8_t *u = (uint8_t *)list;

    for (i = 1; i < 4; i++)
        list[nwords + i] = p[i];

    sum = 0;
    list[nwords] = 0;
    for(i = 0; i < nwords + 4; i++)
        sum += list[i];
    p[0] = -sum; // checking sum code
    list[nwords] = p[0];

#if 0 // ftruncate() does it for us
    *len = (4 - (*len & 3)) & 3;
    u -= *len; // 32-bit align
    *len += (nwords + 4) << 2;
#else
    *len = (nwords + 4) << 2;
#endif

#if _DEBUG & 0x04 // -----------------------------------------------------------
    sum = 0;
    for(i = 0; i < nwords + 4; i++)
        sum += list[i];
fprintf(stderr, ">>> table WR chksum: 0x%08x (0x%08x), len: %lu\n", *p, sum, *len);
#endif // ----------------------------------------------------------------------

    return u;
}

static ALWAYS_INLINE
pgunz_t *read_pgunz_table(int fd, int *err)
{
    int i;
    uint8_t *u, buf[16];
    uint32_t nwords, *list, sum = 0;
    pgunz_t *ptbl;
    size_t len;

    *err = 0;

    if (fd <= STDOUT_FILENO)
        return NULL;

    if (lseek(fd, -16, SEEK_END) < 0) {
        *err = -16;
        perror("lseek");
        return NULL;
    }

    u = &buf[12];
    full_read(fd, buf, 16); //RAF, TODO: better return -1 in case of error
    for (i = 0; i < 4; i++)
        if(u[i] != ptgz_magic_str[i])
            break;
    if (i != 4) {
        *err = -4;
        return NULL;
    }

    nwords = *(uint32_t *)&buf[4];

    ptbl = create_pgunz_table(nwords);
    if (!ptbl) {
        *err = -2;
        return NULL;
    }

    len = ((nwords + 4) << 2);
    if (lseek(fd, -len, SEEK_END) < 0) {
        *err = -1;
        perror("lseek");
        return NULL;
    }

    list = &ptbl->chksum;
    full_read(fd, list, len); //RAF, TODO: better return -1 in case of error

    sum = 0;
    for (i = 0; i < nwords + 4; i++)
        sum += list[i];
    if (sum) {
        *err = 1;
        free(ptbl);
        return NULL;
    }

#if _DEBUG & 0x08 // -----------------------------------------------------------
fprintf(stderr, ">>> table RD chksum: 0x%08x (0x%08x), len: %lu\n", sum, list[0], len);
#endif // ----------------------------------------------------------------------

    return ptbl;
}

static
void verbose_printout(vrbout_t *vo)
{
    if(vo->vlvl < 1)
        return;

    fprintf(stderr,
    "%s, nth:%u/%d, file: %dx[%zu + %4ld] -> %ld, gz: %lu (%0.1f%%,-%d)\n",
        _g_libz_name, vo->nthr, _g_cpu_procs, _g_tot_chunks, _g_chunk_size,
        (ssize_t)vo->osze - vo->isze, _g_read_file_size, vo->olen,
        opt_decompress ? (float) _g_read_file_size * 100 / vo->olen :
                         (float) vo->olen * 100 / _g_read_file_size ,
        _g_compression_level
    );

    if(vo->vlvl < 2 || !vo->ptbl)
        return;

    fprintf(stderr,
    "ptbl> magicw: 0x%08x, chksum: 0x%08x, nwords: %u, bufsze: %u\n",
        vo->ptbl->magicw, vo->ptbl->chksum,
        vo->ptbl->nwords, vo->ptbl->bufsze
    );

    uint32_t *list = vo->ptbl->cur.list;
    if(!list)
        return;

    float avg, sum = 0;
    uint32_t min = -1, max = 0;
    for(uint32_t i = 0; i < vo->nidx; i++) {
        uint32_t val = list[i];
        if(val < min) min = val;
        if(val > max) max = val;
        sum += val;
    }
    avg = sum / vo->nidx;
    fprintf(stderr,
    "ptbl> pages output: %u (%u), chunk: %u <%0.0f> %u (%u <%0.0f> %u)\n",
        MPAGES(sum), (uint32_t)sum, mpages(min),
        avg / 4096, MPAGES(max), min, avg, max
    );

    if(vo->vlvl < 3)
        return;

    for(uint32_t i = 0; i < vo->nidx; i++) {
        min = 1;
        max = 0;
        uint32_t val = list[i];
        for(uint32_t n = 0; n < vo->nidx; n++) {
            uint32_t cur = list[n];
            if(val < cur) min++;
            if(val > cur) max++;
        }
        fprintf(stderr,
        "  0x%08x: %8u %10u | <%10u >%10u\n",
            i, MPAGES(val), val, min, max
        );
    }
}

static
int output_finaliser(int ofd, vrbout_t *vo, off_t offset)
{
    uint32_t *list = vo->ptbl->cur.list;

    if(vo->nidx && _g_tot_chunks && vo->nidx != _g_tot_chunks) {
        _print2("ERROR: next_idx != _g_tot_chunks, %u vs %u\n",
            vo->nidx, _g_tot_chunks);
        return 1;
    }

    if (!ofd) return 0;

    if(vo->nidx < 2 || !list) {
        list = NULL;
        goto skip_reorgnz;
    }

    if (ofd == STDOUT_FILENO)
        goto write_table;

    /*
     * In-place file reorganization using kernel-Level zero-copy
     * Loop through all compressed chunk lengths stored in list[]
     */
    if(_g_out_mmap_base) {
        uint8_t *src = _g_out_mmap_base + offset;
        uint8_t *dst = src + list[0]; /* Skip chunk 0 */
        for (uint32_t i = 1; i < vo->nidx; i++) {
            size_t len = list[i];
            src += WBUF_MAX_SIZE;
            if (!len) continue; //RAF: it should never happens, by design
            __builtin_memmove(dst, src, len);
            dst += len;
        }
        vo->olen += dst - _g_out_mmap_base;
    } else {
        int i;
        off_t src = offset;
        off_t dst = src + list[0]; /* Start immediately after Chunk 0 */
        for (uint32_t i = 1; i < vo->nidx; i++) {
            size_t len = list[i];
            src += WBUF_MAX_SIZE;
            if (!len) continue; //RAF: it should never happens, by design
            if (full_rcopy(ofd, dst, src, len) != len)
                return 1;
            dst += len;
        }
        vo->olen = dst;
    }
    vo->olen += offset;

skip_reorgnz:
    /* Update vo->olen and truncate remaining sparse tail */
    if(list)
        vo->olen = ((vo->olen + 3) >> 2) << 2;
    if (ftruncate(ofd, vo->olen) < 0) {
        perror("ftruncate");
        return -1;
    }
    if(lseek(ofd, 0, SEEK_END) < 0) {
        perror("lseek");
        return -1;
    }

write_table:
    /*
     * Append metadata table as initially described at this link
     *  ~> https://github.com/robang74/uzpexec#parallel-ungzip
     */

    size_t len = 0;
    vo->ptbl->nwords = vo->nidx;
    vo->ptbl->bufsze = _g_chunk_size;
    uint8_t *u = finalize_pgunz_table(vo->ptbl, &len);

    if (_g_out_mmap_base) {
        if(_g_tot_chunks) { // write PTGZ list in the PTGZ header
            __builtin_memmove(_g_out_mmap_base + PTGZ_LIST_START_OFF,
                (const void *)list, _g_tot_chunks << 2);
            len = 0;
        } else { // append the full PTGZ table at the end of file
            __builtin_memmove(_g_out_mmap_base + vo->olen,
                (const void *)u, len);
            vo->olen += len;
        }
    } else
    if(ofd == STDOUT_FILENO) {
        // append the full PTGZ table at the end of file
        full_write(ofd, (const void *)u, len);
        vo->olen += len;
        len = 0;
    } else { // write PTGZ list in the PTGZ header
        full_pwrite(ofd, (void *)list,
            _g_tot_chunks << 2, PTGZ_LIST_START_OFF);
        len = 0;
    }

#if _DEBUG & 0x80 // -----------------------------------------------------------
    if(len)
    {
        int err;
        pgunz_t *pz = read_pgunz_table(ofd, &err);
        if(!pz || err)
            fprintf(stderr, ">>> ERR -- read_pgunz_table: %d\n", err);
        else
        if (memcmp(u, &pz->chksum, len)) {
            fprintf(stderr, ">>> ERR -- table mismatch, len: %lu\n", len);
        }
        return err;
    }
#endif // ----------------------------------------------------------------------

    return 0;
}

#define _verbout_init(_vo) {  \
    _vo.ptbl = ptbl;          \
    _vo.vlvl = opt_verbose;   \
    _vo.nthr = nthreads;      \
    _vo.nidx = next_idx;      \
    _vo.isze =  in_size;      \
    _vo.osze = out_size;      \
    _vo.blen = buf_size;      \
    _vo.olen = outlen;        \
    _vo.infd = infd;          \
}

/* RAF
 *******************************************************************************

  The main idea behind ptgz_header() is about using a standard GZIP header
  crafted on RFC-1952 specifications which can contains a table of chunks
  or when the input is from STDIN the size of the reading chunk which will
  be useful to efficiently find chunks by a just-in-time heuristic (STDIN).

$ printf ""   | gzip -c | wc -c
   20
$ { printf "" | gzip -c; gzip -c libz.tar; } | gzip -dt && echo OK
   OK

  Switching from appending a table to using the FEXTRA field in GZIP header,
  the simplest approach is to add that header as a void GZIP file which does
  not hurt the gzip inflating operations. The overhead is increased by an
  extra 20 bytes compared to the minimum needed, but it speeds-up devel/debug.

  +-----------------------------------------------------------------------+
  | Header FEXTRA (RFC-1952)                                              |
  |                                                                       |
  | [ XLEN (2B) ] = total size of the extra data                          |
  |  +-----------------------------------------------------------------+  |
  |  | Subfield PTGZ                                                   |  |
  |  |                                                                 |  |
  |  | [ ID  (2B: 'p','z') ]                                           |  |
  |  | [ LEN (2B: payload) ] = size of this subfield only, eq. XLEN-4  |  |
  |  +-----------------------------------------------------------------+  |
  +-----------------------------------------------------------------------+

  So, it seems that PTGZ  could be a 100% back-compatible format and also
  being embedded into a RFC-1952 header while the 64-bit coverage range
  and its memory burden spread among many PTGZ headers along the GZIP file,
  every time the current table run out of fields.

$ make _test-clean _test-basic test-ptgz
./ptgzip libz.tar -kv
PTGZ> magic: 0x04088b1f, size: 258280
zlib-ng, nth:8/36, file: 36 x 258280 = 9297920, gz: 2890152 [160] (31.1%), zl:6
head -c64 libz.tar.gz | hexdump -C
00000000 [1f 8b] 08 04 00 00 00 00  00 03 08 00 70 7a  04 00  |............pz..|
00000010  e8 f0  03 00 03 00 00 00  00 00 00 00 00 00 [1f 8b] |................|
00000020  08 00  00 00 00 00 00 03  ec 3d 6b 77 da 48  b2 f3  |.........=kw.H..|
00000030  59 bf  a2 57 66 63 f0 5a  18 b0 cd 24 93 65  76 30  |Y..Wfc.Z...$.ev0|
00000040

  The current table has 4 words (16 bytes) that are redundant when the PTGZ
  format is embedded in the GZIP header. The current overhead is 4 words plus
  a word for each record, in the embedded format would be 10 bytes + 3 bytes
  for each record (2^24 offset range is 2 x 16 MB x 16 cores = 512 MB RAM max).

 *******************************************************************************
*/

static ALWAYS_INLINE
const uint8_t *ptgz_header_make(uint32_t ctm, uint32_t in_len, int16_t size)
{
    uint8_t *buf = _g_ptgz_header;

    // 1. Fixed Header GZIP 10 bytes
    if (buf[0] == 0) {            // Set once, because fixed fields
        buf[0]  = 0x1f;           // Magic 2 bytes
        buf[1]  = 0x8b;
        buf[2]  = 0x08;           // Compression method: DEFLATE
        buf[3]  = 0x04;           // FLG: 0x04 = FEXTRA enabled
      //buf[8]  = 0x00;           // XFL, already =0 by init
        buf[9]  = 0x03;           // OS
    }

#if 0
    //RAF this function is used one-time only, and buf = {0}
    //*(uint32_t *)&buf[4] = 0;   // MTIME =0, 32-bit aligned
#else
    if (ctm) {                    // MTIME
        buf[4] = (uint8_t)(ctm      );
        buf[5] = (uint8_t)(ctm >>  8);
        buf[6] = (uint8_t)(ctm >> 16);
        buf[7] = (uint8_t)(ctm >> 24);
    }
#endif

    uint16_t plen = size + 4;
    // XLEN = Subfield ID (2B) + Subfield LEN (2B) + Payload Length
    uint16_t xlen = plen + 4;

    // FEXTRA field: XLEN 2 bytes
    buf[10] = (uint8_t)(xlen     );
    buf[11] = (uint8_t)(xlen >> 8);

    // PTGZ Subfield ID 2 bytes
    buf[12] = 'p';
    buf[13] = 'z';

    // Subfield Payload Length 2 bytes
    buf[14] = (uint8_t)(plen     );
    buf[15] = (uint8_t)(plen >> 8);

    // First field is the input chunk lenght
    buf[16] = (uint8_t)(in_len      );
    buf[17] = (uint8_t)(in_len >>  8);
    buf[18] = (uint8_t)(in_len >> 16);
    buf[19] = (uint8_t)(in_len >> 24);

    // Termination 10 bytes from buf[20]
    buf[20 + size] = 0x03;        // Raw DEFLATE void (BFINAL=1, BTYPE=00)
#if 0
    buf[21] = 0x00;
    buf[22] = 0x00; buf[23] = 0x00; buf[24] = 0x00; buf[25] = 0x00; // CRC32
    buf[26] = 0x00; buf[27] = 0x00; buf[28] = 0x00; buf[29] = 0x00; // ISIZE
#else
    //RAF this function is used one-time only, and buf = {0}
    //__builtin_memset(&buf[21], 0, PTGZ_HEADER_MAXSIZE - 21);
#endif

    return buf; // return a local value but it is fine
}

static ALWAYS_INLINE
uint8_t *ptgz_header_read(uint8_t *buf, uint16_t *nbytes, uint32_t *size)
{
    uint16_t plen;

    *size = 0;
    *nbytes = 0;
    if(!buf) return NULL;

    if ( buf[0] != 0x1f ||   buf[1] != 0x8b
    ||   buf[2] != 0x08 || !(buf[3]  & 0x04)
    ||  buf[12] != 'p'  ||  buf[13] != 'z'
    ){
        return NULL;
    } else {
//      xlen = ((uint16_t)buf[11] << 8) | buf[10];
        plen = ((uint16_t)buf[15] << 8) | buf[14];
    }
    if(plen < 4) return NULL;

    *nbytes = plen - 4;
    *size = ( ((uint32_t)buf[16])       )
          | ( ((uint32_t)buf[17]) <<  8 )
          | ( ((uint32_t)buf[18]) << 16 )
          | ( ((uint32_t)buf[19]) << 24 );

    return &buf[PTGZ_LIST_START_OFF];
}

static ALWAYS_INLINE
size_t ptgz_header_init(int infd, pgunz_t *ptbl)
{
    void *ptr = NULL;
    size_t len = 0, nwords = 0;
    uint32_t in_size = 0;
    uint16_t nbytes = 0;

    if (_g_read_mmap_base && _USE_MMAP) {
        ptr = _g_read_mmap_base;
    } else {
        if (_g_read_mmap_base) {
            if(_g_read_file_size < PTGZ_HEADER_SIZE)
                return _g_read_file_size;
            __builtin_memcpy(_g_ptgz_header,
                          _g_read_mmap_base, PTGZ_HEADER_SIZE);
        } else {
            len = full_read(infd, _g_ptgz_header, PTGZ_HEADER_SIZE);
            if (len != PTGZ_HEADER_SIZE)
                return len;
        }
        ptr = _g_ptgz_header;
    }
    ptr = ptgz_header_read(ptr, &nbytes, &in_size);
    if (!ptr)
        return PTGZ_HEADER_SIZE;

    if (_g_read_mmap_base) {
        if(_g_read_file_size < PTGZ_HEADER_SIZE + nbytes)
            return _g_read_file_size;
        __builtin_memcpy(&_g_ptgz_header[PTGZ_HEADER_SIZE],
                      &_g_read_mmap_base[PTGZ_HEADER_SIZE], nbytes);
    } else {
        len += full_read(infd, &_g_ptgz_header[PTGZ_HEADER_SIZE], nbytes);
        if (len != nbytes + PTGZ_HEADER_SIZE)
            return len;
    }

    nwords = nbytes >> 2;
    _g_tot_chunks = nwords;
    _g_ptgz_list_size = nbytes;
    _g_first_offeset = nbytes + PTGZ_HEADER_SIZE;
    __builtin_memset(ptbl, 0, sizeof(pgunz_t));
    ptbl->nwords = nwords;
    ptbl->cur.size = nwords;
    ptbl->cur.list = (uint32_t *)ptr;
    ptbl->bufsze = in_size;

    return 0;
}

// =============================================================================
// Core
// =============================================================================

static
void _cinit(chunk_t *c, int ofd, int infd, sem_t *sem_ptr,
    size_t out_size, uint32_t in_len)
{
    static ALIGNED4 chunk_t m = {0};
    static off_t out_offset = 0;
    static off_t  in_offset = 0;
    static unsigned idx     = 0;

    if(m.state == 0) {
        m.state = 1;
        if (_USE_FREE) m.flags = b_flag_free;
        m.sem_ptr = _THR_WAIT ? sem_ptr : NULL;
        m.out_cap = out_size ?: WBUF_MAX_SIZE;
        m.in_off = PTGZ_HEADER_CURSIZE;
        m.infd = infd;
        m.ofd = ofd;
    }

    __builtin_memcpy(c, &m, sizeof(m));

    c->idx      = idx;
    c->out_off  = out_offset;
    out_offset += _g_chunk_size; // RAF: using out_cap requires file-reorganiz.

    c->in_off += in_offset;
    c->in_len  = in_len;
    in_offset += in_len;

    idx++;
}

#define chunk_list_init(_c) \
    _cinit(_c, ofd, infd, &sem, out_size, ilst[current])

#define inflate_parallel zxflate_parallel
#define deflate_parallel zxflate_parallel

static int zxflate_parallel(int infd, int ofd, size_t in_size,
    size_t out_size, int8_t *buf, size_t buf_size, bool seek, pgunz_t *ptbl)
{
    sem_t sem;
    int err = 0;
    vrbout_t vo;
    size_t outlen = 0, offset = 0;
    uint32_t *ilst = ptbl->cur.list;
    uint32_t next_idx = 0, current = 0, nthreads = _g_cpu_procs;

    const bool dflt = !opt_decompress;

#if _DEBUG
fprintf(stderr, "\n>>> zpd, is: %lu, os: %lu, bs: %lu, tot: %u\n",
    in_size, out_size, buf_size, _g_tot_chunks);
#endif

    _g_chunk_size = in_size;
    chunk_t chunks[2][MAX_THREADS];
    memset(chunks, 0, sizeof(chunks));
    sem_init(&sem, 0, 0);

    /* setup chunk descriptors and output buffers, spawn worker threads */
    for (uint32_t i = 0; i < nthreads; i++, current++)
    {
        chunk_t *c = &chunks[0][i];

        if (dflt) {
            chunk_init(c, current, ofd, infd, &sem, 0);
            if (deflate_chunk_init(c)) {
                nthreads = i;
                break;
            }
            chunk_deflate_start(&c->thr, c);
        } else {
            chunk_list_init(c);
            if (buf_size && buf) {
                c->read = buf;
                c->read_len = buf_size;
                buf_size = 0;
            }
            if (inflate_chunk_init(c)) {
                nthreads = i;
                break;
            }
            chunk_inflate_start(&c->thr, c);
        }
//      pthread_detach(c->thr);
        //RAF: the bottleneck is the next one in the ordered list
        //     since after the first the father starts to write
        //     then the bottleneck is the first one, let it go!
        if(!i) _cpu_relax();
    }

do_a_thread_wait:
#if _THR_WAIT
//RAF: one thread completed, at least as
// long as, at least, one thread exists.
    if (current != _g_tot_chunks)
        if (full_sem_wait(&sem))
            return -1;
#else
    _cpu_relax();
#endif
    //fprintf(stderr, "o\n");

do_another_loop:
    for(uint32_t a = 0; a < 2; a++)
    for(uint32_t i = 0, b = !a; i < nthreads; i++)
    {
        chunk_t *c  = &chunks[a][i];
        chunk_t *cb = &chunks[b][i];

        if (c->error) {
            _print2("\nERROR: %s failed on chunk %d, size: %lu -> %lu,"
                " state: %d, ofd: %d, error: %d\n", c->action, c->idx,
                c->in_len, c->out_len, c->state, c->ofd, c->error);
            return c->error;
        }

        if (c->state < 2)
            continue;

        if (!c->thr)
            goto skip_do_new_thread;

#if _DEBUG & 0x20 // -----------------------------------------------------------
fprintf(stderr, "%s> pid: %lu, ofd: %d, idx: %d vs %d / %d, outlen: %lu / %lu\n",
    c->action, c->thr, ofd, c->idx, next_idx, _g_tot_chunks, c->out_len, outlen);
#endif // ----------------------------------------------------------------------

        if (!c->in_len && c->state == 3)
        {
            current--;
            goto dispose;
        }

        /* create another thread to do work */
        if (!cb->thr && !cb->state
        && (_g_tot_chunks ? (current < _g_tot_chunks) : 1)
        ){
            if (dflt) {
                chunk_init(cb, current, ofd, infd, &sem, 0);
                if (deflate_chunk_init(cb)) {
                    c->thr = 0;
                } else {
                    current++;
                    chunk_deflate_start(&cb->thr, cb);
    //              pthread_detach(cb->thr);
                }
            } else {
                chunk_list_init(cb);
                if (inflate_chunk_init(cb)) {
                    c->thr = 0;
                } else {
                    current++;
                    chunk_inflate_start(&cb->thr, cb);
    //              pthread_detach(cb->thr);
                }
            }
        }

skip_do_new_thread:

#if _DEBUG & 0x40 // -----------------------------------------------------------
if (ofd > STDOUT_FILENO || c->idx == next_idx)
fprintf(stderr, "%s> cur: %2d / %2d (%d), idx: %2d vs %2d (ofd: %d), pth: %lu/%d\n",
    c->action, current, _g_tot_chunks, nthreads, c->idx, next_idx,
    ofd, c->thr, chunks[b][i].state);
#endif // ----------------------------------------------------------------------

        /* thread write done or ready to */
        if (c->state != 3)
            continue;

        /* ordered writing on STDOUT, only */
        if (ofd == STDOUT_FILENO
        &&  c->idx != next_idx
        ){
            continue;
        }

        /* granting the correct order */
        next_idx++;
        if(infd == STDIN_FILENO)
            _g_read_file_size += c->in_len;
        outlen += (ofd == STDOUT_FILENO)
                ? full_write(ofd, c->out, c->out_len)
                : c->out_len
                ;
        if(dflt && ptbl->cur.list)
            ptbl->cur.list[ c->idx ] = c->out_len;

#if _DEBUG & 0x20 // -----------------------------------------------------------
//if (ofd > STDOUT_FILENO || c->idx == next_idx)
fprintf(stderr, ">>> cur: %2d / %2d (%d), idx: %2d vs %2d (ofd: %d), pth: %lu/%d\n",
    current, _g_tot_chunks, nthreads, c->idx, next_idx, ofd, cb->thr, cb->state);
#endif // ----------------------------------------------------------------------

dispose:
        chunk_dispose(c, 0);

        // RAF: another pending work-done might be available
        goto do_another_loop;
    }

    // RAF: no pending work-done available
    // is there something else to wait for?
    if (next_idx < current)
        goto do_a_thread_wait;

// === Ending ==================================================================

    _verbout_init(vo);
    verbose_printout(&vo);
if (dflt) {
    err = output_finaliser(ofd, &vo, PTGZ_HEADER_CURSIZE);
}

do_free_n_return:
    #if _USE_FREE // RAF: the Linux kernel does it for us at exit(), redundant
    sem_destroy(&sem);
    #endif

    return err;
}

// =============================================================================
// Main
// =============================================================================

static
size_t do_output_mmap(int ofd)
{
    size_t max;

    if (!ofd || opt_stdout)
        return 0;

    // RAF, TODO: the PTGZ determines the output file size
    max  = (opt_decompress ? 0 : WBUF_MAX_SIZE) * _g_tot_chunks;
    max += PTGZ_HEADER_CURSIZE;

    /* 1. Pre-allocate max size for output mmap */
    if (ftruncate(ofd, max) < 0) {
        perror("ftruncate");
        return 0;
    }

    if(_DNT_MMAP || !max)
        return 0;

    /* 2. Map output file into virtual memory */
    _g_out_mmap_base = mmap(NULL, max,
        PROT_READ  | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE, ofd, 0);
    if (_g_out_mmap_base == MAP_FAILED) {
        _g_out_mmap_base = NULL;
        perror("mmap");
        return 0;
    }

    return max;
}

int main(int argc, char **argv)
{
    char *filename = NULL;
    int ofd = STDOUT_FILENO;
    int infd = STDIN_FILENO;
    size_t max_out_size = 0;
    unsigned nthreads;
    vrbout_t vo;
    sem_t sem;

    sem_init(&sem, 0, 0);

    #if (_DO_OPTL & 1) //RAF: optional code
    struct rlimit lim;
    // Get current CPU limit (in seconds)
    prlimit(0, RLIMIT_CPU, NULL, &lim);
    // Bump soft limit up to the hard limit
    if (lim.rlim_cur < lim.rlim_max) {
        lim.rlim_cur = lim.rlim_max;
        if (prlimit(0, RLIMIT_CPU, &lim, NULL))
          perror("prlimit");
    }
    #endif

#if _USE_OPT
    static struct option longopts[] = {
        {"stdout",      no_argument,       NULL, 'c'},
        {"to-stdout",   no_argument,       NULL, 'c'},
        {"decompress",  no_argument,       NULL, 'd'},
        {"help",        no_argument,       NULL, 'h'},
        {"quiet",       no_argument,       NULL, 'q'},
        {"force",       no_argument,       NULL, 'f'},
        {"fast",        no_argument,       NULL, '1'},
        {"best",        no_argument,       NULL, '9'},
        {"test",        no_argument,       NULL, 't'},
        {"keep",        no_argument,       NULL, 'k'},
        {"no-name",     no_argument,       NULL, 'n'},
        {"verbose",     no_argument,       NULL, 'v'},
        {"license",     no_argument,       NULL, 'L'},
        {"memory",      required_argument, NULL, 'm'},
        {"processes",   required_argument, NULL, 'p'},
        {NULL, 0, NULL, 0}
    };

    while (1) {
        int ch = getopt_long(argc, argv, "cdfhnvqtk123456789m:p:", longopts, NULL);
        if(ch == -1) { filename = argv[optind]; break; }
        switch (ch) {
        case 'c':
            opt_stdout = 1;
            break;
        case 'd':
            opt_decompress = 1;
            break;
        case 'f':
            opt_force = 1;
            break;
        case 't':
            opt_test = 1;
            break;
        case '?':
        case 'h':
            opt_help = 1;
            break;
        case 'L':
            fprintf(stderr, "%s\n", LICENSE);
            return 0;
        case 'q':
            opt_quiet = 1;
            break;
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            _g_compression_level = ch - '0';
            break;
        case 'k':
            opt_keep = 1;
            break;
        case 'v':
            opt_verbose++;
            break;
        case 'm':
            opt_memory = (int)strtoul(optarg, NULL, 0);
            break;
        case 'p':
            opt_processes = (int)strtoul(optarg, NULL, 0);
            break;
        default:
            break;
        }
    }

#else // RAF: this branch was kept for testing _USE_OPT=1 performance impact
    opt_help = (argc < 2);
#endif

    if(opt_quiet)
        opt_verbose = 0;

    if (opt_help) {
        opt_quiet = 0;
        _print2("\n    Usage: %s [opts] <file>"
                "\n     opts: -d, -#, -v, -q, -c, -h\n\n",
                    basename(argv[0]));
        return 0;
    }

    if(!opt_processes)
        opt_processes = MAX_THREADS;

    _g_cpu_procs = sysconf(_SC_NPROCESSORS_ONLN);
    nthreads = _g_cpu_procs;
    if (nthreads > opt_processes)
        nthreads = opt_processes;
    else
    if (nthreads < 1)
        nthreads = 1;

    signal(SIGPIPE, SIG_IGN);

// === open input file =========================================================

    while (filename && (filename[0] != '-' || filename[1]))
    {
        infd = open(filename, O_RDONLY);
        if (infd < 0) {
            perror("open");
            return 1;
        }

        struct stat st;
        if (fstat(infd, &st) < 0) {
            perror("fstat");
            return 1;
        }
        if (!S_ISREG(st.st_mode)) {
            _print2("error: not a regular file\n");
            return 1;
        }

        if(!st.st_size || st.st_size < 0) {
            _print2("warning: zero lenght file\n");
            return 0;
        }
        _g_read_file_size = st.st_size;

        #if (_DO_OPTL & 2) //RAF: optional code
        posix_fadvise(infd, 0, 0, POSIX_FADV_SEQUENTIAL);  // sequential access
        posix_fadvise(infd, 0, 0, POSIX_FADV_WILLNEED);    // will need all of it
        #endif

        if(_DNT_MMAP || !_g_read_file_size)
            break;

        // mmap entire file (zero-copy input for all threads)
        _g_read_mmap_base = mmap(NULL, _g_read_file_size,
            PROT_READ, MAP_SHARED | MAP_POPULATE, infd, 0);
        if (_g_read_mmap_base == MAP_FAILED) {
            _g_read_mmap_base = NULL;
            perror("mmap");
        } /*else {
            // kernel keeps the mapping via vnode reference
            close(infd);
            infd = -1;
        }*/

        break;
    }

// === input chunks split ======================================================

    // decide chunk size and total number of chunks
    size_t outlen = 0;
    _g_tot_chunks = 0;

    if (!_g_read_file_size) {
        // RAF, TODO: other values are failing
        _g_chunk_size = MAX_CHUNK_SIZE;
    }
    else
    if (_g_read_file_size <= MIN_CHUNK_SIZE) {
        nthreads = 1;
        _g_tot_chunks = 1;
        _g_chunk_size = _g_read_file_size;
    } else {
        for(int i = nthreads; i > 1; i--) {
            _g_chunk_size = _int_div(_g_read_file_size, i);
            if (_g_chunk_size >  MAX_CHUNK_SIZE) {
                break; // multi rounds
            }
            if (_g_chunk_size >= MIN_CHUNK_SIZE) {
                nthreads = i;
                break; // 1-round split
            }
        }

        /* Target: split evenly across all CPUs */
        _g_chunk_size = _int_div(_g_read_file_size, nthreads);

        /* Clamp to the design limits */
        if (_g_chunk_size < MIN_CHUNK_SIZE)
            _g_chunk_size = MIN_CHUNK_SIZE;
        else
        if (_g_chunk_size > MAX_CHUNK_SIZE)
            _g_chunk_size = MAX_CHUNK_SIZE;

        /* Page-align for I/O efficiency */
        _g_chunk_size = _mpceil(_g_chunk_size) << 12;
        _g_tot_chunks = _int_div(_g_read_file_size, _g_chunk_size);

#if _DO_WRST
        if (_DO_WRST > 1
        || (_g_cpu_procs << 1) > _g_tot_chunks) {
            size_t wrst = _g_read_file_size % _g_chunk_size;
            if(wrst > (_g_tot_chunks << 1))
                _g_chunk_size -= (_g_chunk_size - wrst) / _g_tot_chunks;
            // RAF: below 64-bit alignment isn't convenient
            _g_chunk_size = ((_g_chunk_size + 7) >> 3) << 3;
            _g_tot_chunks = _int_div(_g_read_file_size, _g_chunk_size);
        }
#endif

        /* Never keep more threads than chunks */
        if (nthreads > _g_tot_chunks)
            nthreads = _g_tot_chunks;
    }

#if _DEBUG & 0x10 // -----------------------------------------------------------
float x = ((float)100*(_g_read_file_size%_g_chunk_size))/_g_chunk_size;
if(x && x < 50)
fprintf(stderr, "reading rst: %3.0f%%, from fd=%d: '%s'\n",
    x, infd, filename?:"(NULL)");
#endif // ----------------------------------------------------------------------

// === open output file ========================================================

    if (opt_decompress && opt_test)
    {
        ofd = open("/dev/null", O_RDWR, 0);
        if (ofd < 0) {
            ofd = 0;
            perror("open");
        }
    }
    else
    if (ofd && !opt_stdout)
    {
        ssize_t len = strlen(filename) + (opt_decompress ? -3 : 4);
        char *str = malloc(len);
        if(!str) {
            perror("malloc");
            return 1;
        }
        if(opt_decompress) {
            if(len < 0 || !strstr(".gz", &filename[len])) {
                fprintf(stderr, "Fatal: not a '.gz' terminated file name\n");
                return 1;
            }
            strncpy(str, filename, len);
        } else {
            snprintf(str, len, "%s.gz", filename);
        }
        str[len] = 0;

        //RAF, TODO: to check the original file permissions, if any than STDIN
        ofd = open(str, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP);
        if (ofd < 0) {
            perror("open");
            return 1;
        }
        #if _USE_FREE
        free(str); //RAF: this can be left at the exit() as well
        str = NULL;
        #endif
    }

    if (ofd == STDOUT_FILENO)
    {
    #if (_DO_OPTL & 4) //RAF: optional code
        int pipesz = fcntl(STDOUT_FILENO, F_GETPIPE_SZ);
        if (pipesz > 0 && pipesz < (1 << 20)) {
            for (int target = 1 << 20; target >= pipesz; target >>= 1)
                if (fcntl(STDOUT_FILENO, F_SETPIPE_SZ, target) == target)
                    break;
        }
    #elif (_DO_OPTL & 8) //RAF: optional code
        static char stdout_buf[1 << 20];  // 1MB buffer
        setvbuf(stdout, stdout_buf, _IOFBF, sizeof(stdout_buf));
    #endif
    }

// =============================================================================

    pgunz_t tbl, *ptbl = &tbl;
    size_t buf_size = 0;
    uint8_t *ptr = NULL;
    uint32_t *list = NULL;
    uint32_t next_idx = 0, current = 0;

    chunk_t chunks[2][MAX_THREADS];
    memset(chunks, 0, sizeof(chunks));

    if (!opt_decompress)
        goto set_ptbl_list;

// === inflate =================================================================

    int ret = 0;
    uint32_t *ilst = NULL;
    uint32_t out_size = 0, in_size = 0;
    uint16_t nbytes = 0;

    buf_size = ptgz_header_init(infd, &tbl);
    if(buf_size && buf_size < PTGZ_HEADER_SIZE)
        return 1; // nothing to do

    if(buf_size || infd == STDIN_FILENO) {
        // what has been read should from STDIN be passed to the first chunk
        goto set_default_values;
    } else {
        // at this point, tbl and the first chunk offset are initialisated
        next_idx = tbl.nwords;
        in_size = tbl.bufsze;
        ilst = tbl.cur.list;
        buf_size = 0;
    }

    max_out_size = do_output_mmap(ofd);

#if _DEBUG // ------------------------------------------------------------------
fprintf(stderr, "PTGZ1> ptr: %p, size: %u, lsze: %lu, 1off: %lu, nchk: %d, mxos: %lu\n",
    ptr, in_size, _g_ptgz_list_size, _g_first_offeset, _g_tot_chunks, max_out_size);
fprintf(stderr, "      ilst: 0x%08x 0x%08x | 0x%08x 0x%08x 0x%08x 0x%08x\n",
    ilst[-2], ilst[-1], ilst[0], ilst[1], ilst[2], ilst[3]);
#endif // ----------------------------------------------------------------------

    if (in_size  < MIN_CHUNK_SIZE) {
set_default_values:
        ptr      = _g_ptgz_header;
        out_size = UNOUT_CHUNK_SIZE;
        in_size  = UNZIN_CHUNK_SIZE;
        ret = _inflate_stream(infd, ofd, in_size,
            out_size, ptr, buf_size, !max_out_size, &tbl);
    } else
    if (_g_ptgz_list_size < sizeof(uint32_t) || !ilst[0]) {
do_inflate_stream:
        in_size  = size_by_blocks(zread_max_size(in_size));
        out_size = size_by_blocks(in_size >> 1);
        if(out_size < UNOUT_CHUNK_SIZE)
           out_size = UNOUT_CHUNK_SIZE;
        ret = _inflate_stream(infd, ofd, in_size,
            out_size, ptr, buf_size, !max_out_size, &tbl);
    } else {
        out_size = size_by_blocks(zread_max_size(in_size));
        ret = inflate_parallel(infd, ofd, in_size,
            out_size, ptr, buf_size, !max_out_size, &tbl);
        buf_size = 0;
    }

    if(buf_size) {
        _verbout_init(vo);
        verbose_printout(&vo);
    }

//  fprintf(stderr, ">>> ret: %d, ptr: %p, sze: %u\n", ret, ptr, in_size);
    if (!ret && !opt_keep && !opt_test && !opt_stdout) {
        if(unlink(filename))
            perror("unlink");
    }

    goto do_free;

// === deflate =================================================================

set_ptbl_list:
    max_out_size = do_output_mmap(ofd);
    ptbl = create_pgunz_table(_g_tot_chunks);
    list = ptbl->cur.list;

    _g_ptgz_list_size = _g_tot_chunks;
    if(_g_ptgz_list_size > PTGZ_LIST_MAX_WORDS)
       _g_ptgz_list_size = PTGZ_LIST_MAX_WORDS;
    _g_ptgz_list_size = _g_ptgz_list_size << 2;

    /* RAF
     * The GZIP modify-time is a 32-bit unsigned value and therefore
     * it will not overflow in the year 2038 but in 2106. So, we can
     * use time() output as-is without worrying too much about 2038.
     */
    time_t utc = time(NULL);
    /* RAF
     * When the data is read from STDIN the _g_tot_chunks is zero and
     * the size of the PTGZ table into the header reaches the minimum
     * to contain just the helpful value of the _g_chunk_size used.
     * However, the list list of chunks can be appended, to be read
     * when .gz is provided as a seekable file. Moreover, when the .gz
     * is also writeable (or by a specific option) the appended table
     * can be transferred into the header, like it happens during the
     * creation from a seekable file as data input source.
     */
    ptr = (void *)ptgz_header_make(utc, _g_chunk_size, _g_ptgz_list_size);
    full_write(ofd, ptr, PTGZ_HEADER_CURSIZE);

#if _DEBUG // ------------------------------------------------------------------
fprintf(stderr, "PTGZ1> ptr: %p, size: %u, lsze: %lu, 1off: %lu, nchk: %d, mxos: %lu\n",
    ptr, in_size, _g_ptgz_list_size, _g_first_offeset, _g_tot_chunks, max_out_size);
fprintf(stderr, "      list: 0x%08x 0x%08x | 0x%08x 0x%08x 0x%08x 0x%08x\n",
    ilst[-2], ilst[-1], ilst[0], ilst[1], ilst[2], ilst[3]);
#endif // ----------------------------------------------------------------------

    ret = deflate_parallel(infd, ofd, _g_chunk_size,
        WBUF_MAX_SIZE, 0, 0, !max_out_size, ptbl);

do_free:
    #if _USE_FREE // RAF: the Linux kernel does it for us at exit(), redundant
    sem_destroy(&sem);
    if(_g_read_mmap_base)
        munmap(_g_read_mmap_base, _g_read_file_size);
    if(_g_out_mmap_base) {
        msync(_g_out_mmap_base, outlen, MS_SYNC);
        munmap(_g_out_mmap_base, max_out_size);
    }
    if(ofd) close(ofd);
    free(ptbl);
    free(list);
    #endif

    return ret;
}
