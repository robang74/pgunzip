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

#define _GZ_WRITE (!out_mmap_base)

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

static ALWAYS_INLINE
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
        if (posix_memalign((void **)&c->out, 8, c->out_cap))
            c->out = NULL;
        if (!c->out) {
            perror("malloc out buf");
            c->error = -2;
            return NULL;
        }
    }
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

#if _DEBUG // ------------------------------------------------------------------
if(strm.total_out)
    fprintf(stderr, ">>> tot: %ld, max: %ld\n", strm.total_out, WBUF_MAX_SIZE);
#endif // ----------------------------------------------------------------------

    c->out_len = strm.total_out;
    if (ret != Z_STREAM_END) {
        perror("deflate");
        c->error = ret | (1<<9); //RAF: rarely it returns Z_OK = 0
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

static ALWAYS_INLINE
int chunk_write(chunk_t *c)
{
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

static ALWAYS_INLINE
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
    if (!c->out)
         if (posix_memalign((void **)&c->out, 8, c->out_cap))
              c->out = NULL;
    if (!c->out) {
        perror("malloc in buf");
        exit(-1);
    }

    if (read_mmap_base) {
        c->in = read_mmap_base + c->in_off;
        c->map |= b_mmap_in;
    } else {
        if (!c->in)
             if (posix_memalign((void **)&c->in, 8, c->in_len))
                  c->in = NULL;
        if (!c->in) {
            perror("malloc in buf");
            exit(-1);
        }
        c->in_len = full_read(c->infd, c->in, c->in_len);
#if _DEBUG // ------------------------------------------------------------------
fprintf(stderr, ">>> thr(%04d): read = %lu\n", idx, c->in_len);
#endif  // ---------------------------------------------------------------------
        if (!c->in_len)
            c->state = 3;
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
// Main
// =============================================================================

static int opt_stdout    = 0;    /* -c, --stdout, --to-stdout */
static int opt_help      = 0;    /* -h, --help */
static int opt_quiet     = 0;    /* -q, --quiet */
       // compression_level ;    /* -#, --fast (=1), --best (=9) */
static int opt_keep      = 0;    /* -k, --keep */
static int opt_memory    = 0;    /* -m, --memory (KiB) */
static int opt_processes = 0;    /* -p, --processes */
static int opt_verbose   = 0;    /* -v, --verbose */

/* --- file list --- */
static int   nfiles = 0;
static char **names = NULL;

#define TABLE_ITEMS ((uint32_t)tot_chunks + 4)
#define TABLE_BSIZE ((TABLE_ITEMS) << 2)
#define PGZ_MAGIC_1 0x6274
#define PGZ_MAGIC_2 0x7a70

#define LICENSE \
    "(c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, GPL v2"

int main(int argc, char **argv)
{
    uint32_t *list = NULL;
    int ofd = STDOUT_FILENO;
    int infd = STDIN_FILENO;
    size_t max_out_size = 0;
    int nthreads;
    sem_t sem;

#if _USE_OPT
    static struct option longopts[] = {
        {"stdout",      no_argument,       NULL, 'c'},
        {"to-stdout",   no_argument,       NULL, 'c'},
        {"help",        no_argument,       NULL, 'h'},
        {"quiet",       no_argument,       NULL, 'q'},
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
        int ch = getopt_long(argc, argv, "chvqk123456789m:p:", longopts, NULL);
        if(ch == -1) break; else nfiles--;
        switch (ch) {
        case 'c':
            opt_stdout = 1;
            break;
        case 'h':
        case '?':
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

    /* collect remaining arguments as filenames */
    names = &argv[optind];
    nfiles += argc;
#else // RAF: this branch was kept for testing _USE_OPT=1 performance impact
    names = &argv[1];
    nfiles = (names != NULL);
    opt_help = (argc < 2);
#endif

    if (opt_help || !nfiles) {
        opt_quiet = 0;
        _print2("\n    Usage: %s [opts] <file>"
                "\n     opts: -#, -v, -q, -c, -h\n\n",
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

    while (names[0] && (names[0][0] != '-' || names[0][1]))
    {
        infd = open(names[0], O_RDONLY);
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

        if(_DNT_MMAP)
            break;

        // mmap entire file (zero-copy input for all threads)
        read_mmap_base = mmap(NULL, read_filesize,
            PROT_READ, MAP_SHARED | MAP_POPULATE, infd, 0);
        if (read_mmap_base == MAP_FAILED) {
            read_mmap_base = NULL;
            perror("mmap infd");
        } else {
            // kernel keeps the mapping via vnode reference
            close(infd);
            infd = -1;
        }

        break;
    }

    signal(SIGPIPE, SIG_IGN);

// =============================================================================
// Chunks
// =============================================================================

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
        chunk_size = ((chunk_size + 4095) >> 12) << 12;
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

#if _DEBUG // ------------------------------------------------------------------
float x = ((float)100*(read_filesize%chunk_size))/chunk_size;
if(x && x < 50)
fprintf(stderr, "reading rst: %3.0f%%, from fd=%d: '%s'\n",
    x, infd, names[0]?:"(NULL)");
#endif // ----------------------------------------------------------------------

    chunk_t chunks[2][MAX_THREADS];
    memset(chunks, 0, sizeof(chunks));

    if (posix_memalign((void **)&list, 8, TABLE_ITEMS << 2))
          list = NULL;
    if (!list) {
        perror("malloc list");
    } else { //RAF: possible fallback without list
        list[0] =  0;
        list[1] = (PGZ_MAGIC_1 << 16) | (chunk_size >> 12);
    }

    /* ---- deal with the output file, when '-c' isn't among arguments ---- */
    while (!opt_stdout) {
        size_t len = strlen(names[0]) + 4;
        char *str = malloc(len);
        if(!str) {
            perror("malloc strn");
            return 1;
        }
        snprintf(str, len, "%s.gz", names[0]);
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

        max_out_size = WBUF_MAX_SIZE * tot_chunks;
        if(!max_out_size)
            break;

        /* 1. Pre-allocate max size for output mmap */
        if (ftruncate(ofd, max_out_size) < 0) {
            perror("ftruncate");
            break;
        }

        if(_DNT_MMAP)
            break;

        /* 2. Map output file into virtual memory */
        out_mmap_base = mmap(NULL, max_out_size,
            PROT_READ  | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE, ofd, 0);
        if (out_mmap_base == MAP_FAILED) {
            out_mmap_base = NULL;
            perror("mmap out");
            break;
        }

        break;
    }

// =============================================================================
// Threads
// =============================================================================

    int a = 0, next_idx = 0, current = 0;

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
                " size: %lu, err: %d\n", names[0], current + i,
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

#if _DEBUG // ------------------------------------------------------------------
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

#if _DEBUG // ------------------------------------------------------------------
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
        if(list) list[ 2+c->idx ] = c->out_len;

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

    if(next_idx < 2)
        goto skip_table;

    /*
     * In-place File Reorganization using Kernel-Level Zero-Copy
     * Loop through all compressed chunk lengths stored in list[]
     */
    if(out_mmap_base) {
        uint8_t *src = out_mmap_base;
        uint8_t *dst = out_mmap_base + list[2]; /* Skip chunk 0 */
        for (int i = 1; i < next_idx; i++) {
            size_t len = list[i + 2];
            src += WBUF_MAX_SIZE;
            if (!len) continue; //RAF: it should never happens, by design
            __builtin_memmove(dst, src, len);
            dst += len;
        }
        outlen += dst - out_mmap_base;
    } else {
        off_t src = 0;
        off_t dst = list[2]; /* Start immediately after Chunk 0 */
        for (int i = 1; i < next_idx; i++) {
            size_t len = list[i + 2];
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

skip_table:
    /* Update outlen and truncate remaining sparse tail */
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

    size_t sum = 0, left = TABLE_BSIZE;
    list[TABLE_ITEMS-1]  = ((TABLE_ITEMS-4) << 16);
    list[TABLE_ITEMS-1] |=   PGZ_MAGIC_2; // items + magic
    for(uint32_t *u = &list[1]; left > 0; left-=4) {
        sum += *u++;
    };  sum += list[TABLE_ITEMS-1];
    left = TABLE_BSIZE;
    list[TABLE_ITEMS-2] = -sum; // sum checking code

    uint8_t *p = (uint8_t *)list;
    uint32_t r = outlen & 3; // 32-bit align
    left -= 4-r;
    p  = &p[4-r];
    if (out_mmap_base) {
        __builtin_memmove(out_mmap_base + outlen, p, TABLE_BSIZE);
        outlen += TABLE_BSIZE;
    } else {
        outlen += full_write(ofd, p, TABLE_BSIZE);
    }

do_verbose:
    if(opt_verbose) {
        fprintf(stderr, "%s, nthr: %u, split: %d x %zu = %ld, size: %lu (%0.1f%%),"
            " zlvl: %d\n", libz_name, nthreads, next_idx, chunk_size, read_filesize,
                outlen, (float)outlen*100/read_filesize, compression_level);
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
    free(list);
    #endif

    return 0;
}
