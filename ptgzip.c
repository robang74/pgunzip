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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

#define MAX_THREADS         16
#define MAX_CHUNK_SIZE     (1UL << 18)     /* max target size per segment */
#define MIN_CHUNK_SIZE     (1UL << 16)     /* 2x min gzip compress window */

static unsigned cpu_procs = 0;

typedef struct {
    pthread_t thr;
    sem_t    *sem_ptr;
    uint8_t  *in;      /* pointer into mmap */
    uint8_t  *out;
    size_t   out_cap;
    int      infd;
    int      ofd;
    uint8_t  map;
//RAF: part that requires to be completely reset //
    int      idx;                                //
    uint8_t  state;                              //
    char     error;                              //
    size_t   in_len;                             //
    size_t   out_len;                            //
    off_t    out_off;                            //
    off_t    in_off;                             //
//RAF: part that requires to be completely reset //
    uint8_t  end;
} chunk_t ALIGNED4;

enum {
    b_mmap_none = 0,
    b_mmap_out  = 1,
    b_mmap_in   = 2,
    b_mmap_seek = 8,
};

#ifndef _DEBUG
#define _DEBUG    0
#endif
#ifndef _USE_OPT
#define _USE_OPT  1 //RAF: no difference in gz speed
#endif
#ifndef _DNT_MMAP
#define _DNT_MMAP 1 //RAF: =1 to test mmap() failure, also file faster
#endif

#ifndef _DO_WRST
#define _DO_WRST  2 // 0: last run can be shorter than 1/2 chunk_size
#endif              // 2: impose the same rule also to 2+ cycles runs
#ifndef _THR_WAIT
#define _THR_WAIT 1 // 1: wait for any of threads completes, 0: polling
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
#define libz_name "zlib-ng"
#endif
#ifndef _USE_MNZ
#define _USE_MNZ  0 //RAF: libz/-ng by linker, miniz by compiler also
#endif

#if   _USE_ZNG
  #include "zlib-ng.h"
  #define _deflate_init2 zng_deflateInit2
  #define _deflate_bound zng_deflateBound
  #define _deflate_end   zng_deflateEnd
  #define _deflate       zng_deflate
  #define _stream_t      zng_stream
#elif _USE_MNZ
  #include <miniz.h>
  #define libz_name       "miniz"
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
  #ifndef libz_name
  #define libz_name          "zlib"
  #endif
  #define _deflate_init2     deflateInit2
  #define _deflate_bound     deflateBound
  #define _deflate_end       deflateEnd
  #define _deflate           deflate
  #define _stream_t        z_stream
#endif
#ifndef libz_name
#define libz_name          "none"
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
#define WBUF_MAX_SIZE zbuf_max_size(chunk_size)

#define is_outbuf_freeable(_c) (_c->out && !(_c->map & b_mmap_out))
#define  is_inbuf_freeable(_c) (_c->in  && !(_c->map & b_mmap_in ))

#define _print2(fmt...) while(!opt_quiet) { fprintf(stderr, fmt); break; }
#define _cpu_relax() do { if(sched_yield()) usleep(1); } while(0)
#define _int_div(_a, _b) (((_a) + (_b) - 1) / (_b))

static uint8_t *read_mmap_base = NULL;
static uint8_t *out_mmap_base = NULL;
static off_t read_filesize = 0;
static size_t chunk_size   = 0;
static int tot_chunks      = 0;

static int compression_level = 6;
static int chunk_write(chunk_t *c);

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
// Thread worker: compress one chunk directly to its output buffer
// -----------------------------------------------------------------------------

static void *thread_compress(void *arg)
{
    int ret;
    chunk_t *c = arg;
    _stream_t strm = {0};

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(c->idx % cpu_procs, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);

    if(!c->in_len) {
        c->state = 3; // no data, task completed as void
        goto release;
    }

    /* 1. GZIP FORMAT: 15 + 16 is mandatory.
     *    deflateInit() produces RFC-1950 zlib format, not RFC-1952 gzip.
     *    Without +16 the output cannot be concatenated into a valid .gz file.
     */
    ret = _deflate_init2(&strm, compression_level, Z_DEFLATED,
                    15 + 16, 7, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        perror("deflate_init2");
        c->error = -3;
        goto endfnc;
    }
#if _ZLIB_MEM
    /* 2. OUTPUT BUFFER: must be deflateBound(), never c->in_len.
     *    Incompressible data EXPANDS by ~0.1 % + headers.
     *    c->in_len alone guarantees a buffer overrun on random bytes.
     */
    if (!c->out) {
        /*
        * RAF: potentially the bound could be larger than the WBUF_MAX_SIZE
        *      in case the library differs from the current ones tested and
        *      in such a case there is a good canche that USE_MMAP would fail
        *      silently, in some corner cases when writing out of bond will
        *      corrupt data and thus creating a corrupted gzip archive or when
        *      the ending bound would be violated and thus the kernel SEGVDEF.
        */
        c->out_cap = _deflate_bound(&strm, c->in_len);
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

    /* 3. COMPRESSION LOOP:
     *    - Feed all input with Z_NO_FLUSH until avail_in == 0.
     *    - Then Z_FINISH until deflate returns Z_STREAM_END.
     *    Your old loop called Z_NO_FLUSH forever and never finished the stream.
     */
#if _ONE_ZDF
#else
    do {
        ret = deflate(&strm, Z_NO_FLUSH);
    } while (ret == Z_OK);
    if (ret != Z_STREAM_END)
#endif
    ret = _deflate(&strm, Z_FINISH);

#if _DEBUG & 0x01 // -----------------------------------------------------------
if(strm.total_out)
    fprintf(stderr, ">>> tot: %ld, max: %ld\n", strm.total_out, WBUF_MAX_SIZE);
#endif // ----------------------------------------------------------------------

    c->out_len = strm.total_out;
    if (ret != Z_STREAM_END /* && ret != Z_BUF_ERROR && ret != Z_OK */) {
        perror("deflate");
        c->error = ret; //RAF: rarely it returns Z_OK = 0
    }

    /* 4. CLEANUP: always call deflateEnd() to free internal buffers.
     *    Skipping it leaks several KiB per chunk.
     */
endfnc:
    _deflate_end(&strm);
    c->state = 2;
    if (c->ofd != STDOUT_FILENO
    && (!out_mmap_base || !_USE_MMAP)
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
        //c->sem_ptr = NULL;
    }
    return NULL;
}

static
int chunk_write(chunk_t *c)
{
    if(c->map & b_mmap_seek) {
        if (ftruncate(c->ofd, c->out_off + c->out_len) < 0) {
            perror("ftruncate");
            return 1;
        }
    }

    if(out_mmap_base) {
        uint8_t *dst = out_mmap_base + c->out_off;
        return !__builtin_memmove(dst, c->out, c->out_len);
    } else {
        off_t off = c->out_off;
        size_t len = c->out_len;
        uint8_t *p = c->out;

        while (len > 0) {
            ssize_t w = pwrite(c->ofd, p, len, off);
            if (w < 0) {
                if (errno == EINTR) continue;
                perror("p/write");
                break;
            }
            p   += w;
            off += w;
            len -= w;
        }
        c->in_len = c->out_len - len;
        return !!len;
    }
}

static
size_t full_write(int fd, const void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;

    while (len > 0) {
        ssize_t w = write(fd, p, len);
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

/*
 * RAF: currently the .out_cap exists but always set to WBUF_MAX_SIZE, which
 *      makes the .out_cap redundant while out_mmap_base would be likely a
 *      more usful member of the chunk_t structure. A revision is needed
 *      after the development is completed to get rid off of global variables
 *      in favor of a data structure that can refers also to its own thread_t.
 */
static ALWAYS_INLINE
void chunk_init(chunk_t *c, int idx, int ofd, int infd, sem_t *sem_ptr)
{
    c->state = 1;
    c->idx = idx;
    c->error = 0;
    c->sem_ptr = _THR_WAIT ? sem_ptr : NULL;
    c->out_cap = zbuf_max_size(chunk_size);
    c->out_off = WBUF_MAX_SIZE * idx; //(_USE_MMAP ? c->out_cap : chunk_size) * idx;
    c->in_off  =                           chunk_size  * idx;
    if(infd != STDIN_FILENO) {
        c->in_len = (idx + 1 == tot_chunks)
                  ? (size_t)(read_filesize - c->in_off)
                  : chunk_size;
    } else {
        c->in_len = chunk_size;
    }
    c->infd = infd;
    c->ofd = ofd;
    c->map = 0;

#if _USE_MMAP
    /* Assign output pointer inside mapped output file space */
    if (out_mmap_base) {
        c->out = out_mmap_base + c->out_off;
        c->map |= b_mmap_out;
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
        perror("malloc in buf");
        exit(-1);
    }
#endif

    if (read_mmap_base) {
        c->in = read_mmap_base + c->in_off;
        c->map |= b_mmap_in;
    } else {
        if (!c->in)
             if (posix_memalign((void **)&c->in, 64, c->in_len))
                  c->in = NULL;
        if (!c->in) {
            perror("malloc in buf");
            exit(-1);
        }
        c->in_len = full_read(c->infd, c->in, c->in_len);
        if (!c->in_len) c->state = 3;

#if _DEBUG & 0x02 // -----------------------------------------------------------
fprintf(stderr, ">>> thr(%04d): read = %lu\n", idx, c->in_len);
#endif  // ---------------------------------------------------------------------
    }
}

static ALWAYS_INLINE
void chunk_work_start(pthread_t *p, chunk_t *c)
{
    if (!pthread_create(p, NULL, thread_compress, c))
        return;

    perror("pthread_create");
    exit(1);
}

// =============================================================================
// gunzip
// =============================================================================

/* RAF
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

static void *thread_chunk_write(void *arg)
{
    chunk_t *c = arg;
    c->error |= chunk_write(c);
    return NULL;
}

#define UNZIN_CHUNK_SIZE  MAX_CHUNK_SIZE
#define UNOUT_CHUNK_SIZE  MIN_CHUNK_SIZE

#define _SEEKER_FUNC  0
#define _READ_AHEAD  (1 && !_SEEKER_FUNC)

static int inflate_stream(int infd, int ofd, size_t len)
{
    size_t w, r, set = 0, rmn = 0;
    uint8_t *inbuf  = NULL;
    uint8_t *outbuf = NULL;
    int ret, eof = 0, nchunks = 0;
    _stream_t strm = {0};
    chunk_t c = {0};

    if (posix_memalign((void **)&inbuf,  64, UNZIN_CHUNK_SIZE)
    ||  posix_memalign((void **)&outbuf, 64, UNOUT_CHUNK_SIZE)) {
        perror("malloc");
        return -1;
    }

    ret = _inflate_init2(&strm, 15 + 16);
    if (ret != Z_OK) {
        perror("inflateInit2");
        goto endfunc;
    }

    c.map = b_mmap_out | b_mmap_in;
    if(!len) c.map |= b_mmap_seek;
#if _READ_AHEAD
    strm.next_in  = inbuf;
    strm.avail_in = 0;
#endif
    c.out = outbuf;
    c.ofd = ofd;

    while (1) {
#if _SEEKER_FUNC == 0
        #if _READ_HEAD // read-ahead is not convenient, at the best match 1:1
        r = strm.avail_in;
        if (!eof && r < MIN_CHUNK_SIZE) { // feed the input buffer
            if (r) __builtin_memmove(inbuf, strm.next_in, r);
            w = full_read(infd, inbuf + r, UNZIN_CHUNK_SIZE - r);
            if (!w) eof = 1; // EOF
            else strm.avail_in += w;
            strm.next_in = inbuf;
        }
        if (eof && !strm.avail_in)
            break;
        #else
        if (!strm.avail_in) { // feed the input buffer
            r = full_read(infd, inbuf, UNZIN_CHUNK_SIZE);
            if (!r) break; // EOF
            strm.next_in = inbuf;
            strm.avail_in = r;
        }
        #endif
#else // chunks splitting
        if (!strm.avail_in) { // feed the input buffer
            if (rmn) __builtin_memmove(inbuf, &inbuf[set], rmn);
            r = rmn + full_read(infd, inbuf + rmn, UNZIN_CHUNK_SIZE - rmn);
            if (!r) break; // EOF

            strm.next_in = inbuf;
//fprintf(stderr, "mgk: 0x%08x\n", *(uint32_t *)inbuf);
            #if   _SEEKER_FUNC == 1
            set = chunk_seeker(inbuf, r - 3);
            if (set) {
                strm.avail_in = set;
                rmn = r - set;
            } else {
                strm.avail_in = r;
                rmn = 0;
            }
            #elif _SEEKER_FUNC == 2
            set = 0;
            rmn = 0;
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
            set = 0;
            rmn = 0;
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
            #endif
        }
#endif
        strm.next_out  = outbuf;
        strm.avail_out = UNOUT_CHUNK_SIZE;

        ret = _inflate(&strm, Z_NO_FLUSH);
        if (ret < 0 && ret != Z_BUF_ERROR) {
            if (ret == Z_DATA_ERROR && nchunks)
                break; /* ignore trailing junk/ptgz table */
            fprintf(stderr, "inflate error: %d\n", ret);
            break;
        }

        w = UNOUT_CHUNK_SIZE - strm.avail_out;
        if(ofd == STDOUT_FILENO) {
            if (full_write(ofd, outbuf, w) < 0)
                goto endfunc;
        } else {
            if (c.thr) {
                pthread_join(c.thr, NULL);
                c.thr = 0;
            }
            c.out_len = w;
            chunk_work_start(&c.thr, &c);
            c.out_off += w;
            c.idx++;
        }

        if (ret == Z_STREAM_END) {
            nchunks++;
            _inflate_end(&strm);
            if (_inflate_init2(&strm, 15 + 16) != Z_OK) {
                ret = Z_STREAM_ERROR;
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
    return !(ret == Z_STREAM_END || nchunks);
}

// =============================================================================
// Prep
// =============================================================================

#define _mpceil(_x) (((_x) + 4095) >> 12)

#define TABLE_ITEMS ((uint32_t)tot_chunks + 4)
#define TABLE_BSIZE ((TABLE_ITEMS) << 2)

/* RAF
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
    uint32_t  bufsze;  // the size of the read expressed 4KiB memory pages 2^12
    uint32_t  magicw;  // the magic word closing the format, but it can be 16_t
    /* list stats here */
} __attribute__ ((packed)) pgunz_t;

const uint8_t ptgz_magic_str[4] = { "ptgz" };

static
pgunz_t *create_pgunz_table(uint32_t nwords)
{
    pgunz_t *p;
    uint8_t *u;
    uint32_t n, len;

    if(!nwords) nwords = 1U << 14;
    n   = _mpceil(nwords << 2);
    len = sizeof(pgunz_t) + n;
    p   = malloc(len);
    if(!p) {
        perror("malloc");
        return p;
    }

    memset(p, 0, len);
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

#if 0 // ftrucate does it for us
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

    if(lseek(fd, -16, SEEK_END) < 0) {
        *err = -16;
        perror("lseek");
        return NULL;
    }

    u = &buf[12];
    full_read(fd, buf, 16); //RAF, TODO: better return -1 in case of error
    for (i = 0; i < 4; i++)
        if(u[i] != ptgz_magic_str[i])
            break;
    if(i != 4) {
        *err = -4;
        return NULL;
    }

    nwords = *(uint32_t *)&buf[4];

    ptbl = create_pgunz_table(nwords);
    if(!ptbl) {
        *err = -2;
        return NULL;
    }

    len = ((nwords + 4) << 2);
    if(lseek(fd, -len, SEEK_END) < 0) {
        *err = -1;
        perror("lseek");
        return NULL;
    }

    list = &ptbl->chksum;
    full_read(fd, list, len); //RAF, TODO: better return -1 in case of error

    sum = 0;
    for(i = 0; i < nwords + 4; i++)
        sum += list[i];
    if(sum) {
        *err = 1;
        free(ptbl);
        return NULL;
    }

#if _DEBUG & 0x08 // -----------------------------------------------------------
fprintf(stderr, ">>> table RD chksum: 0x%08x (0x%08x), len: %lu\n", sum, list[0], len);
#endif // ----------------------------------------------------------------------

    return ptbl;
}

// =============================================================================
// Main
// =============================================================================

static int opt_stdout     = 0;    /* -c, --stdout, --to-stdout */
static int opt_help       = 0;    /* -h, --help */
static int opt_quiet      = 0;    /* -q, --quiet */
        // compression_level ;    /* -#, --fast (=1), --best (=9) */
static int opt_keep       = 0;    /* -k, --keep */
static int opt_force      = 0;    /* -f, --force */
static int opt_memory     = 0;    /* -m, --memory (KiB) */
static int opt_processes  = 0;    /* -p, --processes */
static int opt_verbose    = 0;    /* -v, --verbose */
static int opt_decompress = 0;    /* -d, --decompress */

int main(int argc, char **argv)
{
    char *filename = NULL;
    int ofd = STDOUT_FILENO;
    int infd = STDIN_FILENO;
    size_t max_out_size = 0;
    int nthreads;
    sem_t sem;

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
        {"keep",        no_argument,       NULL, 'k'},
        {"verbose",     no_argument,       NULL, 'v'},
        {"license",     no_argument,       NULL, 'L'},
        {"memory",      required_argument, NULL, 'm'},
        {"processes",   required_argument, NULL, 'p'},
        {NULL, 0, NULL, 0}
    };

    while (1) {
        int ch = getopt_long(argc, argv, "cdfhvqk123456789m:p:", longopts, NULL);
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
            compression_level = ch - '0';
            break;
        case 'k':
            opt_keep = 1;
            break;
        case 'v':
            opt_verbose = 1;
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

    if (opt_help) {
        opt_quiet = 0;
        _print2("\n    Usage: %s [opts] <file>"
                "\n     opts: -d, -#, -v, -q, -c, -h\n\n",
                    basename(argv[0]));
        return 0;
    }

    if(!opt_processes)
        opt_processes = MAX_THREADS;

    cpu_procs = sysconf(_SC_NPROCESSORS_ONLN);
    nthreads = cpu_procs;
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
        read_filesize = st.st_size;

        if(_DNT_MMAP || !read_filesize)
            break;

        // mmap entire file (zero-copy input for all threads)
        read_mmap_base = mmap(NULL, read_filesize,
            PROT_READ, MAP_SHARED | MAP_POPULATE, infd, 0);
        if (read_mmap_base == MAP_FAILED) {
            read_mmap_base = NULL;
            perror("mmap");
        } else {
            // kernel keeps the mapping via vnode reference
            close(infd);
            infd = -1;
        }

        break;
    }

// === input chunks split ======================================================

    // decide chunk size and total number of chunks
    size_t outlen = 0;
    tot_chunks = 0;

    if (!read_filesize) {
        // RAF, TODO: other values are failing
        chunk_size = MAX_CHUNK_SIZE;
    }
    else
    if (read_filesize <= MIN_CHUNK_SIZE) {
        nthreads = 1;
        tot_chunks = 1;
        chunk_size = read_filesize;
    } else {
        for(int i = nthreads; i > 1; i--) {
            chunk_size = _int_div(read_filesize, i);
            if (chunk_size >  MAX_CHUNK_SIZE) {
                break; // multi rounds
            }
            if (chunk_size >= MIN_CHUNK_SIZE) {
                nthreads = i;
                break; // 1-round split
            }
        }

        /* Target: split evenly across all CPUs */
        chunk_size = _int_div(read_filesize, nthreads);

        /* Clamp to the design limits */
        if (chunk_size < MIN_CHUNK_SIZE)
            chunk_size = MIN_CHUNK_SIZE;
        else
        if (chunk_size > MAX_CHUNK_SIZE)
            chunk_size = MAX_CHUNK_SIZE;

        /* Page-align for I/O efficiency */
        chunk_size = _mpceil(chunk_size) << 12;
        tot_chunks = _int_div(read_filesize, chunk_size);

#if _DO_WRST
        if (_DO_WRST > 1
        || (cpu_procs << 1) > tot_chunks) {
            size_t wrst = read_filesize % chunk_size;
            if(wrst > (tot_chunks << 1))
                chunk_size -= (chunk_size - wrst) / tot_chunks;
            // RAF: below 64-bit alignment isn't convenient
            chunk_size = ((chunk_size + 7) >> 3) << 3;
            tot_chunks = _int_div(read_filesize, chunk_size);
        }
#endif

        /* Never keep more threads than chunks */
        if (nthreads > tot_chunks)
            nthreads = tot_chunks;
    }

#if _DEBUG & 0x10 // -----------------------------------------------------------
float x = ((float)100*(read_filesize%chunk_size))/chunk_size;
if(x && x < 50)
fprintf(stderr, "reading rst: %3.0f%%, from fd=%d: '%s'\n",
    x, infd, filename?:"(NULL)");
#endif // ----------------------------------------------------------------------

// === open output file ========================================================

    while (!opt_stdout) {
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

        // RAF, TODO: the PTGZ determines the output file size
        max_out_size = (opt_decompress ? 0 : WBUF_MAX_SIZE) * tot_chunks;

        /* 1. Pre-allocate max size for output mmap */
        if (ftruncate(ofd, max_out_size) < 0) {
            perror("ftruncate");
            break;
        }

        if(_DNT_MMAP || !max_out_size)
            break;

        /* 2. Map output file into virtual memory */
        out_mmap_base = mmap(NULL, max_out_size,
            PROT_READ  | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE, ofd, 0);
        if (out_mmap_base == MAP_FAILED) {
            out_mmap_base = NULL;
            perror("mmap");
            break;
        }

        break;
    }

// === sequential gunzip =======================================================

    /* stdin fallback, but table can be available on shorts files */
    if (opt_decompress) {
        int ret = inflate_stream(infd, ofd, max_out_size);
        if(!ret && !opt_keep)
        {
            if(unlink(filename))
                perror("unlink");
        }
        return ret;
    }

// =============================================================================
// Threads
// =============================================================================

    int a = 0, next_idx = 0, current = 0;

    chunk_t chunks[2][MAX_THREADS];
    memset(chunks, 0, sizeof(chunks));

    pgunz_t *ptbl = create_pgunz_table(tot_chunks);
    uint32_t *list = ptbl->cur.list;

    /* setup chunk descriptors and output buffers, spawn worker threads */
#if _THR_WAIT
    sem_init(&sem, 0, 0);
#endif
    for (int i = 0; i < nthreads; i++, current++) {
        chunk_t *c = &chunks[a][i];
        chunk_init(c, current, ofd, infd, &sem);
        if(c->state == 3) {
            c->state = 0;
            nthreads = i;
            break;
        }
        chunk_work_start(&c->thr, c);
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
    if (current != tot_chunks)
        if (full_sem_wait(&sem))
            return -1;
#else
    _cpu_relax();
#endif
do_another_loop:
    for(int a = 0; a < 2; a++)
    for(int i = 0, b = !a; i < nthreads; i++)
    {
        chunk_t *c  = &chunks[a][i];
        chunk_t *cb = &chunks[b][i];

        if (c->error) {
            _print2("file: '%s'\n    compression failed on chunk %d,"
                " size: %lu, err: %d\n", filename, current + i,
                    c->out_len, c->error);
            return c->error;
        }
        if (c->state < 2)
            continue;

        if (c->thr)
        {
            if (!c->in_len && c->state == 3)
            {
                current--;
                goto dispose;
            }
            else
            /* create another thread to do work */
            if (!cb->thr && !cb->state
            && (tot_chunks ? (current < tot_chunks) : 1)
            ){
                chunk_init(cb, current, ofd, infd, &sem);
                if(cb->state == 3) {
                    #if _USE_FREE
                    if(is_outbuf_freeable(cb)) free(cb->out);
                    if( is_inbuf_freeable(cb)) free(cb->in);
                    memset(cb, 0, sizeof(chunk_t));
                    #else
                    cb->state = 0;
                    //cb->in_len = 0;
                    #endif
                    c->thr = 0;
                } else {
                    current++;
                    chunk_work_start(&cb->thr, cb);
//                  pthread_detach(cb->thr);
                }
            }
        }

#if _DEBUG & 0x20 // -----------------------------------------------------------
if (ofd != STDOUT_FILENO || c->idx == next_idx)
fprintf(stderr, ">>> cur: %2d / %2d (%d), idx: %2d vs %2d (ofd: %d), pth: %lu/%d\n",
    current, tot_chunks, nthreads, c->idx, next_idx,
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

#if _DEBUG & 0x40 // -----------------------------------------------------------
fprintf(stderr, ">>> pid: %lu, ofd: %d, nxt: %d / %d \n",
    c->thr, ofd, next_idx, tot_chunks);
#endif // ----------------------------------------------------------------------

        /* granting the correct order */
        next_idx++;
        if(infd == STDIN_FILENO)
            read_filesize += c->in_len;
        outlen += (ofd == STDOUT_FILENO)
                ? full_write(ofd, c->out, c->out_len)
                : c->out_len
                ;
        if(list) list[ c->idx ] = c->out_len;

dispose:
        #if _USE_FREE
        /* disposing the chunk and its buffer */
        if (is_outbuf_freeable(c)) {
            free(c->out);
            c->out = NULL;
        }
        if (is_inbuf_freeable(c)) {
            free(c->in);
            c->in = NULL;
        }
        memset(&c->idx, 0, (size_t)&(c->end) - (size_t)&(c->idx));
        #else
        c->state = 0;
        //c->in_len = 0;
        #endif

#if 0 /* RAF, TODO: moving resources on the next one doesn't work properly
       *            under this circumstances is better to keep them into
       *            the current chuck which will be reused only by large
       *            files while the smaller wouldn't.
       */
        memset(&c->idx, 0, (size_t)&(c->end) - (size_t)&(c->idx));
        chunk_t *cb; //RAF: it will be the next one, if any
        if (i+1 < nthreads)
            cb = &chunks[b][i+1];
        else
            cb = &chunks[a][ 0 ];
        if(!cb->state)
            __builtin_memcpy(cb, c, sizeof(chunk_t));
#endif

        /* disposing the thread */
//      pthread_join(c->thr, NULL);
        c->thr = 0;

        // RAF: another pending work-done might be available
        goto do_another_loop;
    }

    // RAF: no pending work-done available
    // is there something else to wait for?
    if (next_idx < current)
        goto do_a_thread_wait;

// =============================================================================
// Ending
// =============================================================================

    if (ofd == STDOUT_FILENO)
        goto write_table;

    if(next_idx < 2) {
        list = NULL;
        goto skip_reorgnz;
    }

    /*
     * In-place file reorganization using kernel-Level zero-copy
     * Loop through all compressed chunk lengths stored in list[]
     */
    if(out_mmap_base) {
        uint8_t *src = out_mmap_base;
        uint8_t *dst = out_mmap_base + list[0]; /* Skip chunk 0 */
        for (int i = 1; i < next_idx; i++) {
            size_t len = list[i];
            src += WBUF_MAX_SIZE;
            if (!len) continue; //RAF: it should never happens, by design
            __builtin_memmove(dst, src, len);
            dst += len;
        }
        outlen += dst - out_mmap_base;
    } else {
        int i;
        off_t src = 0;
        off_t dst = list[0]; /* Start immediately after Chunk 0 */
        for (int i = 1; i < next_idx; i++) {
            size_t len = list[i];
            src += WBUF_MAX_SIZE;
            if (!len) continue; //RAF: it should never happens, by design

            off_t off_in = src;
            while (len > 0) {
                ssize_t ret = copy_file_range(ofd,
                    &off_in, ofd, &dst, len, 0);
                if (!ret) break;
                if (ret < 0) {
                    if (errno == EINTR) continue;
                    perror("copy_file_range");
                    return 1;
                }
                len -= ret;
            }
        }
        outlen = dst;
    }

skip_reorgnz:
    /* Update outlen and truncate remaining sparse tail */
    if(list)
        outlen = ((outlen + 3) >> 2) << 2;
    if (ftruncate(ofd, outlen) < 0) {
        perror("ftruncate");
        return 1;
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
    if(!list) goto do_verbose;

    size_t len = 0;
    ptbl->nwords = next_idx;
    ptbl->bufsze = chunk_size;
    uint8_t *u = finalize_pgunz_table(ptbl, &len);
    if (out_mmap_base) {
        if(!__builtin_memmove(out_mmap_base + outlen, (const void *)u, len))
            outlen += len;
    } else {
        outlen += full_write(ofd, (const void *)u, len);
    }

#if _DEBUG & 0x80 // -----------------------------------------------------------
    if(ofd != STDOUT_FILENO)
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

do_verbose:
    if(opt_verbose) {
        fprintf(stderr, "%s, nth:%u/%d, file: %d x %zu = %ld, gz: %lu [%lu]"
            " (%0.1f%%), zl:%d\n", libz_name, nthreads, tot_chunks,
            next_idx, chunk_size, read_filesize, outlen, len, (float)
            outlen * 100 / read_filesize, compression_level);
    }

    #if _USE_FREE // RAF: the Linux kernel does it for us at exit(), redundant
    sem_destroy(&sem);
    if(read_mmap_base)
        munmap(read_mmap_base, read_filesize);
    if(out_mmap_base) {
        msync(out_mmap_base, outlen, MS_SYNC);
        munmap(out_mmap_base, max_out_size);
    }
    close(ofd);
    free(ptbl);
    #endif

    return 0;
}
