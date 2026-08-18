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

#define MAX_THREADS         16
#define MAX_CHUNK_SIZE     (1UL << 18)     /* max target size per segment */
#define MIN_CHUNK_SIZE     (1UL << 16)     /* 2x min gzip compress window */

typedef struct {
    size_t   len;
    off_t    offset;
    sem_t    *sem_ptr;
    unsigned char *in;      /* pointer into mmap */
    unsigned char *out;
    size_t   out_cap;
    size_t   out_len;
    int      infd;
    int      idx;
    int      ofd;
    char     map;
    char     state;
    char     error;
} chunk_t __attribute((aligned(4)));

#ifndef _DEBUG
#define _DEBUG    0
#endif

#ifndef _GZ_WRITE
#define _GZ_WRITE 1 // deflate() + write() decouple CPU and I/O workloads
#endif
#if     _GZ_WRITE
#else
#define _USE_MMAP 1 // the forced alternative to write() is using mmap()
#endif

#ifndef _THR_WAIT
#define _THR_WAIT 0 // 1: wait for a specific thread, 0: anyone ready
#endif
#ifndef _USE_MMAP
#define _USE_MMAP 1 // mmap() is performed by default, but it can fail
#endif
#ifndef _USE_FREE
#define _USE_FREE 0 // free() isn't strictly necessary, but do testing
#endif

#ifndef _USE_OPT
#define _USE_OPT  1 //RAF: no difference in gz speed
#endif
#ifndef _ONE_ZDF
#define _ONE_ZDF  1 //RAF: no difference in .gz size
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

#define zbuf_max_size(_len) (_len + (_len >> 9) + 256)
#define ZBUF_MAX_SIZE zbuf_max_size(chunk_size)

#define is_chunk_freeable(_c) (_c->out && !_c->map)

#define _print2(fmt...) while(!opt_quiet) { fprintf(stderr, fmt); break; }
#define _cpu_relax() do { if(sched_yield()) usleep(1); } while(0)
#define _int_div(_a, _b) (((_a) + (_b) - 1) / (_b))

static unsigned char *read_mmap_base = NULL;
static unsigned char *out_mmap_base = NULL;
static off_t read_filesize = 0;
static size_t max_out_size = 0;
static size_t chunk_size   = 0;
static int tot_chunks      = 0;

static int compression_level = 6;
static int chunk_write(chunk_t *c);

static ALWAYS_INLINE
size_t full_read(int fd, const void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;

    while (len > 0) {
        ssize_t w = read(fd, p, len);
        if (!w) break;
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("read");
            return -1;
        }
        p   += w;
        len -= w;
    }

    return p - (unsigned char *)buf;
}

static ALWAYS_INLINE
void sem_wait_or_exit(sem_t *sem_ptr)
{
    if(!sem_ptr) return;

    int ret = sem_wait(sem_ptr);
    while(ret == EINTR || ret == EAGAIN) {
        _cpu_relax();
        ret = sem_wait(sem_ptr);
    }

    if(!ret) return;

    perror("sem_wait");
    exit(ret);
}

/* ------------------------------------------------------------------ */
/* Thread worker: compress one chunk directly to its output buffer    */
/* ------------------------------------------------------------------ */

static void *thread_compress(void *arg)
{
    chunk_t *c = arg;
    _stream_t strm = {0};
    int ret;

    sem_wait_or_exit(c->sem_ptr);

    if(c->infd == STDIN_FILENO)
    {
        if(!c->in)
            c->in  = malloc(c->len);
        if(!c->in) {
            perror("malloc in buf");
            c->error = -4;
            return NULL;
        }
        c->len = full_read(c->infd, c->in, c->len);
if(_DEBUG) fprintf(stderr, ">>> thr(%04d): read = %lu\n", c->idx, c->len);

        if(c->len == 0) {
if(_DEBUG) fprintf(stderr, "thr(%04d): end of stdin\n", c->idx);
            goto eofile;
        }
        if(c->len  < 0) {
            c->error = -3;
            goto eofile;
        }
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

    /* 2. OUTPUT BUFFER: must be deflateBound(), never c->len.
     *    Incompressible data EXPANDS by ~0.1 % + headers.
     *    c->len alone guarantees a buffer overrun on random bytes.
     */
    if (!c->out) {
        /*
        * RAF: potentially the bound could be larger than the ZBUF_MAX_SIZE
        *      in case the library differs from the current ones tested and
        *      in such a case there is a good canche that USE_MMAP would fail
        *      silently, in some corner cases when writing out of bond will
        *      corrupt data and thus creating a corrupted gzip archive or when
        *      the ending bound would be violated and thus the kernel SEGVDEF.
        */
        size_t cap = _deflate_bound(&strm, c->len);
        if (cap > c->out_cap) c->out_cap = cap;
        c->out = malloc(c->out_cap);
        if (!c->out) {
            perror("malloc out buf");
            c->error = -2;
            return NULL;
        }
    }
    strm.next_in   = c->in;
    strm.avail_in  = c->len;
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
#if 0//_DEBUG
    if(strm.total_out >= ZBUF_MAX_SIZE) {
        fprintf(stderr, "tot: %ld, max: %ld\n",
            strm.total_out, ZBUF_MAX_SIZE);
    }
#endif
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
    if (c->sem_ptr && c->out
    && (c->ofd == STDOUT_FILENO || _GZ_WRITE)
    ){
        sem_post(c->sem_ptr);
        c->sem_ptr = NULL;
    }
#if _GZ_WRITE
    if(c->ofd != STDOUT_FILENO && c->out) {
        c->error |= chunk_write(c);
    }
    #if _USE_FREE
    if (is_chunk_freeable(c)) {
        free(c->out);
        c->out = NULL;
    }
    #endif
#endif
eofile:
    c->state = 3;
    if (c->sem_ptr) {
        sem_post(c->sem_ptr);
        c->sem_ptr = NULL;
    }
    return NULL;
}

static ALWAYS_INLINE
int chunk_write(chunk_t *c)
{
    if(out_mmap_base) {
        unsigned char *dst_ptr = out_mmap_base + c->offset;
        return !__builtin_memmove(dst_ptr, c->out, c->out_len);
    } else {
        int ofd = c->ofd;
        off_t off = c->offset;
        size_t len = c->out_len;
        unsigned char *p = c->out;

        while (len > 0) {
            ssize_t w = pwrite(ofd, p, len, off);
            if (w < 0) {
                if (errno == EINTR) continue;
                perror("p/write");
                break;
            }
            p   += w;
            off += w;
            len -= w;
        }
        c->len = c->out_len - len;
        return !!len;
    }
}

static ALWAYS_INLINE
size_t full_write(int fd, const void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;

    while (len > 0) {
        ssize_t w = write(fd, p, len); 
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write");
            return -1;
        }
        p   += w;
        len -= w;
    }

    return p - (unsigned char *)buf;
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
 * RAF: currently the .out_cap exists but always set to ZBUF_MAX_SIZE, which
 *      makes the .out_cap redundant while out_mmap_base would be likely a
 *      more usful member of the chunk_t structure. A revision is needed
 *      after the development is completed to get rid off of global variables
 *      in favor of a data structure that can refers also to its own thread_t.
 */
static ALWAYS_INLINE
void chunk_init(chunk_t *c, int idx, int ofd, int infd, sem_t *sem_ptr)
{
    size_t len;
    off_t offset;

    c->sem_ptr = _THR_WAIT ? NULL : sem_ptr;
    c->out_cap = ZBUF_MAX_SIZE;
#if _GZ_WRITE
    offset = (off_t)idx * chunk_size;
#else
    offset = (off_t)idx * c->out_cap;
#endif
    len    = (idx == tot_chunks - 1)
           ? (size_t)(read_filesize - offset)
           : chunk_size
           ;
    c->in  = NULL;
    if(read_mmap_base)
        c->in = read_mmap_base + offset;
    c->offset = offset;
    c->infd = infd;
    c->idx = idx;
    c->map = 0;
#if _GZ_WRITE
#else
    /* Assign output pointer inside mapped output file space */
    if (out_mmap_base) {
        c->out = out_mmap_base + offset;
        c->map = 1;
    }
    else
#endif
    if (is_chunk_freeable(c)
    #if _USE_FREE
    #else /* RAF: free() here is a corner case that "should" never happen by
           *      design but if it would happen, it is better deal than oops
           */
    &&  c->len && len > c->len
    #endif
    ){
        free(c->out);
        c->out = NULL;
    }
    c->len = len;
    c->state = 1;
    c->error = 0;
    c->ofd = ofd;
}

static ALWAYS_INLINE
void chunk_work_start(pthread_t *p, chunk_t *c_ptr)
{
    if (!pthread_create(p, NULL, thread_compress, c_ptr))
        return;

    perror("pthread_create");
    exit(1);
}

/* ========================================================================== */
/* Main                                                                       */
/* ========================================================================== */

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

    nthreads = sysconf(_SC_NPROCESSORS_ONLN);
    if (nthreads > opt_processes)
        nthreads = opt_processes;
    else
    if (nthreads < 1)
        nthreads = 1;

    signal(SIGPIPE, SIG_IGN);

    if(names[0] && (names[0][0] != '-' || names[0][1]))
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

        if(_USE_MMAP) {
            // mmap entire file (zero-copy input for all threads)
            read_mmap_base = mmap(NULL, read_filesize,
                PROT_READ, MAP_PRIVATE, infd, 0);
            if (read_mmap_base == MAP_FAILED) {
                read_mmap_base = NULL;
                perror("mmap infd");
            } else {
                // kernel keeps the mapping via vnode reference
                close(infd);
                infd = -1;
            }
        }
    }
#if _DEBUG
fprintf(stderr, "reading from fd=%d: '%s'\n", infd, names[0]?:"(NULL)");
#endif

    // decide chunk size and total number of chunks
    size_t outlen = 0;
    tot_chunks = 0;

    if (read_filesize < 1) {
        chunk_size = MAX_CHUNK_SIZE >> 1;
    }
    else
    if (read_filesize <= MIN_CHUNK_SIZE) {
        nthreads = 1;
        tot_chunks = 1;
        chunk_size = read_filesize;
    } else {
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

        /* Never keep more threads than chunks */
        if (nthreads > tot_chunks)
            nthreads = tot_chunks;
    }

    chunk_t chunks[2][MAX_THREADS];
    memset(chunks, 0, sizeof(chunks));

    list = malloc(TABLE_ITEMS << 2);
    if(!list) {
        perror("malloc list");
    } else { //RAF: possible fallback without list
        list[0] =  0;
        list[1] = (PGZ_MAGIC_1 << 16) | (chunk_size >> 12);
    }

    /* ---- deal with the output file, when '-c' isn't among arguments ---- */
    if(!opt_stdout) {
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
        #endif

        if(!_USE_MMAP || !max_out_size)
            goto not_use_mmap;

#if _GZ_WRITE
        max_out_size = ((size_t)tot_chunks * chunk_size);
#else
        max_out_size = ((size_t)tot_chunks * ZBUF_MAX_SIZE);
#endif
        /* 1. Pre-allocate max size for output mmap */
        if (ftruncate(ofd, max_out_size) < 0) {
            perror("ftruncate");
            goto not_use_mmap;
        }

        /* 2. Map output file into virtual memory */
        out_mmap_base = mmap(NULL, max_out_size,
            PROT_READ | PROT_WRITE, MAP_SHARED, ofd, 0);
        if (out_mmap_base == MAP_FAILED) {
            out_mmap_base = NULL;
            perror("mmap out");
            goto not_use_mmap;
        }
    }

not_use_mmap:
    int a = 0, next_idx = 0, current = 0;

    /* setup chunk descriptors and output buffers, spawn worker threads */
    sem_init(&sem, 0, nthreads);
    pthread_t threads[2][MAX_THREADS];
    memset(threads, 0, sizeof(threads));
    for (int i = 0; i < nthreads; i++) {
        chunk_init(&chunks[a][i], current++, ofd, infd, &sem);
        chunk_work_start(&threads[a][i], &chunks[a][i]);
        //RAF: the bottleneck is the next one in the ordered list
        //     since after the first the father starts to write
        //     then the bottleneck is the first one, let it go!
#if _THR_WAIT
        if(!i) _cpu_relax();
#else
        pthread_detach(threads[a][i]);
#endif
    }

    /* write compressed chunks to stdout in strict segment order */
    while (next_idx < current)
    {
        for (int i = 0, b = !a; i < nthreads; i++)
        {
#if _THR_WAIT
            pthread_join(threads[a][i], NULL);
#else
            _cpu_relax();
            sem_wait_or_exit(&sem); //RAF: one thread completed, at least
            sem_post(&sem);
#endif
            chunk_t *c = &chunks[a][i];
            if (c->error) {
                _print2("file: '%s'\n    compression failed on chunk %d,"
                    " size: %lu, err: %d\n", names[0], current + i,
                        c->out_len, c->error);
                return 1;
            }
            if (c->state < 2)
                continue;

#if _DEBUG
if (ofd != STDOUT_FILENO || c->idx == next_idx)
fprintf(stderr, ">>> cur: %2d / %2d, idx: %2d vs %2d (ofd: %d), pth: %lu/%d\n",
    current, tot_chunks, c->idx, next_idx, ofd,
    threads[a][i], chunks[b][i].state);
#endif
            if (threads[a][i] && !chunks[a][i].len)
            {
                threads[a][i] = 0;
                current--;
                continue;
            }

            /* create another thread to do work */
            if ((!tot_chunks ?: (current < tot_chunks))
            && !chunks[b][i].state
            && threads[a][i]
            ){
                chunk_init(&chunks[b][i], current++, ofd, infd, &sem);
                chunk_work_start(&threads[b][i], &chunks[b][i]);
            }
            _cpu_relax();

            /* ordered writing on STDOUT, only */
            if (ofd == STDOUT_FILENO) {
                if (c->idx != next_idx)
                    continue;
            }
            /* thread write done or ready to */
            if (c->state != 3)
                continue;

#if 0//_DEBUG
fprintf(stderr, ">>> pid: %lu, ofd: %d, nxt: %d / %d \n",
    threads[a][i], ofd, next_idx, tot_chunks);
#endif

            /* disposing the thread */
            threads[a][i] = 0;

            /* granting the correct order */
            next_idx++;
            if(infd == STDIN_FILENO)
                read_filesize += c->len;
            if(ofd == STDOUT_FILENO)
                full_write(ofd, c->out, c->out_len);
            outlen += c->out_len;
            if(list) list[ 2+c->idx ] = c->out_len;

            /* disposing the chunk and its buffer */
            void  *buf = c->out;
            size_t cap = c->out_cap;
            memset(c, 0, sizeof(chunk_t));

            if(out_mmap_base)
                continue;
            #if _USE_FREE
            free(buf);
            #else
            if(i+1 < nthreads)
                c = &chunks[b][i+1];
            else
                c = &chunks[a][ 0 ]; //RAF: it will be the next one
            if(c->out) {
                free(buf);
            } else {
                c->out_cap = cap;
                c->out = buf;
            }
            #endif
        }
        a = !a;
    }

out_of_loop:
    if (ofd == STDOUT_FILENO)
        goto write_table;
    if (infd == STDIN_FILENO)
        next_idx++;

    /*
     * In-place File Reorganization using Kernel-Level Zero-Copy
     */
    if(next_idx < 2)
        goto skip_table;
    if(out_mmap_base) {
        unsigned char *write_ptr = out_mmap_base + list[2]; /* Skip chunk 0 */
        for (int i = 1; i < next_idx; i++) {
#if _GZ_WRITE
            unsigned char *read_ptr = out_mmap_base + ((off_t)i * chunk_size);
#else
            unsigned char *read_ptr = out_mmap_base + ((off_t)i * ZBUF_MAX_SIZE);
#endif
            size_t c_len = list[i + 2];
            if (!c_len) continue; //RAF: it should never happens, by design
            __builtin_memmove(write_ptr, read_ptr, c_len);
            write_ptr += c_len;
        }
        outlen += write_ptr - out_mmap_base;
    } else {
        /* Loop through all compressed chunk lengths stored in list[]  */
        off_t write_pos = list[2]; /* Start immediately after Chunk 0 */
        for (int i = 1; i < next_idx; i++) {
            off_t read_pos = (off_t)i * chunk_size;
            size_t write_size = list[i + 2];

            if (read_pos != write_pos) {
                off_t off_in  = read_pos;
                off_t off_out = write_pos;
                size_t bytes  = write_size;

                while (bytes > 0) {
                    ssize_t ret = copy_file_range(ofd,
                        &off_in, ofd, &off_out, bytes, 0);
                    if (ret < 0) {
                        if (errno == EINTR) continue;
                        perror("copy_file_range");
                        return 1;
                    }
                    bytes -= ret;
                }
            }
            write_pos += write_size;
        }
        outlen = write_pos;
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

    unsigned char *p = (uint8_t *)list;
    unsigned r = outlen & 3; // 32-bit align
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
