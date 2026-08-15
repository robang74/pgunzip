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
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>

#define ALWAYS_INLINE __attribute__ ((always_inline)) inline

#define MAX_SEGMENTS    6
#define MAX_TARGET     (1UL << 20)     /* max target size per segment */

#ifndef _BE_VERBOSE
#define _BE_VERBOSE     0
#endif

typedef struct {
    size_t   len;
    off_t    offset;
    unsigned char *in;      /* pointer into mmap */
    unsigned char *out;
    size_t   out_cap;
    size_t   out_len;
    int      idx;
    int      ofd;
    char     map;
    char     state;
    char     error;
} chunk_t __attribute((aligned(4)));

/* ------------------------------------------------------------------ */
/* Thread worker: compress one chunk directly to its output buffer    */
/* ------------------------------------------------------------------ */
#ifndef _THR_WAIT
#define _THR_WAIT 1
#endif
#ifndef _USE_MMAP
#define _USE_MMAP 0
#define _USE_FREE 0
#endif
#ifndef _USE_FREE
#define _USE_FREE 0
#endif
#ifndef _USE_OPT
#define _USE_OPT 1
#endif
#ifndef _ONE_ZDF
#define _ONE_ZDF 1
#endif
#ifndef _USE_ZNG
#define _USE_ZNG 0
#endif
#ifndef _USE_MNZ
#define _USE_MNZ 0
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
#define _deflate_init2     deflateInit2
#define _deflate_bound     deflateBound
#define _deflate_end       deflateEnd
#define _deflate           deflate
#define _stream_t        z_stream
#endif

#define zbuf_max_size(_len) (_len + (_len >> 9) + 256)
#define ZBUF_MAX_SIZE zbuf_max_size(MAX_TARGET)

static int compression_level = Z_DEFAULT_COMPRESSION;
static int chunk_write(chunk_t *c);

static void *thread_compress(void *arg)
{
    chunk_t *c = arg;
    _stream_t strm = {0};
    int ret;

    /* 1. GZIP FORMAT: 15 + 16 is mandatory.
     *    deflateInit() produces RFC-1950 zlib format, not RFC-1952 gzip.
     *    Without +16 the output cannot be concatenated into a valid .gz file.
     */
    ret = _deflate_init2(&strm, compression_level, Z_DEFLATED,
                    15 + 16, 7, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        c->error = 1;
        goto endfnc;
    }

    /* 2. OUTPUT BUFFER: must be deflateBound(), never c->len.
     *    Incompressible data EXPANDS by ~0.1 % + headers.
     *    c->len alone guarantees a buffer overrun on random bytes.
     */
    if (!c->out) {
        /*
        * RAF: potentially the bound could be larger than the zbuf_max_size()
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
            perror("malloc");
            goto reterr;
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
     ret = _deflate(&strm, Z_FINISH);
#else
    do {
        if (strm.avail_in == 0) {
            ret = deflate(&strm, Z_FINISH);
        } else {
            ret = deflate(&strm, Z_NO_FLUSH);
        }
    } while (ret == Z_OK);
#endif
    c->out_len = strm.total_out;
    if (ret != Z_STREAM_END) {
reterr:
        c->error = 1;
//      goto endfnc;
    }

    /* 4. CLEANUP: always call deflateEnd() to free internal buffers.
     *    Skipping it leaks several KiB per chunk.
     */
endfnc:
    _deflate_end(&strm);
    c->state = 2;
    if(c->ofd != STDOUT_FILENO && c->out)
        c->error |= chunk_write(c);
#if _USE_FREE
    if (is_chunk_freeable(c)) {
        free(c->out);
        c->out = NULL;
    }
#endif
    c->state = 3;
    return NULL;
}

static int chunk_write(chunk_t *c)
{
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

#define is_chunk_freeable(_c) (_c->out && !_c->map)

static unsigned char *mmap_base;
static off_t input_filesize;
static size_t chunk_size;
static int tot_nseg;

static ALWAYS_INLINE
chunk_t *chunk_init(chunk_t *c, int idx, int ofd)
{
    size_t len;
    off_t offset;

    offset = (off_t)idx * chunk_size;
    len    = (idx == tot_nseg - 1)
           ? (size_t)(input_filesize - offset)
           : chunk_size;
    c->in  = mmap_base + offset;
    c->out_cap = zbuf_max_size(chunk_size);
    c->map = 0;
#if _USE_MMAP
    /* Assign output pointer inside mapped output file space */
    if (ofd != STDOUT_FILENO) {
        c->out = out_mmap_base + ((off_t)idx * c->out_cap);
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
    c->offset = offset;
    c->len = len;
    c->state = 1;
    c->error = 0;
    c->idx = idx;
    c->ofd = ofd;

    return c;
}

static ALWAYS_INLINE
int chunk_work_start(pthread_t *p, chunk_t *c, int idx, int ofd)
{
    if (pthread_create(p, NULL, thread_compress, chunk_init(c, idx, ofd)))
    {
        perror("pthread_create");
        return 1;
    }
//  idx = pthread_tryjoin_np(*p, NULL);
//  return (!idx || idx == EBUSY);
    return 0;
}

/* ========================================================================== */
/* Main                                                                       */
/* ========================================================================== */
#include <getopt.h>
#define list_enabled 1
#define print2(fmt...) while(!opt_quiet) { fprintf(stderr, fmt); break; }
#define _cpu_relax() do { if(sched_yield()) usleep(1); } while(0)

static int opt_stdout    = 0;    /* -c, --stdout, --to-stdout */
static int opt_help      = 0;    /* -h, --help */
static int opt_quiet     = 0;    /* -q, --quiet */
       // compression_level ;    /* -#, --fast (=1), --best (=9) */
static int opt_keep      = 0;    /* -k, --keep */
static int opt_memory    = 0;    /* -m, --memory (KiB) */
static int opt_verbose   = 0;    /* -v, --verbose */

/* --- file list --- */
static int   nfiles = 0;
static char **names = NULL;

static unsigned char *out_mmap_base = NULL;

#define TABLE_ITEMS ((uint32_t)tot_nseg + 4)
#define TABLE_BSIZE ((TABLE_ITEMS) << 2)
#define PGZ_MAGIC_1 0x6274
#define PGZ_MAGIC_2 0x7a70

#define LICENSE \
    "(c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, GPL v2"

int main(int argc, char **argv)
{
    int ofd = STDOUT_FILENO;
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
        {NULL, 0, NULL, 0}
    };

    while (1) {
        int ch = getopt_long(argc, argv, "chvqk123456789m:", longopts, NULL);
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
            opt_memory = (size_t)strtoul(optarg, NULL, 0);
            break;
        default:
            break;
        }
    }

    /* collect remaining arguments as filenames */
    names = &argv[optind];
    nfiles += argc;
#else
    names = &argv[1];
    nfiles = (names != NULL);
    opt_help = (argc < 2);
#endif

    if (opt_help || !nfiles) {
        opt_quiet = 0;
        print2("\n    Usage: %s [opts] <file>"
               "\n     opts: -#, -v, -q, -c, -h\n\n",
               basename(argv[0]));
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);

    int infd = open(names[0], O_RDONLY);
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
        print2("error: not a regular file\n");
        return 1;
    }

    input_filesize = st.st_size;
    if (input_filesize == 0) {
        return 0;
    }

    /* ---- mmap entire file (zero-copy input for all threads) ---- */
    mmap_base = mmap(NULL, input_filesize, PROT_READ, MAP_PRIVATE, infd, 0);
    if (mmap_base == MAP_FAILED) {
        perror("mmap infd");
        return 1;
    }
    close(infd);   /* kernel keeps the mapping via vnode reference */

    /* ---- decide chunk size and total number of chunks (multiples of 6) ---- */
    size_t outlen = 0;
    chunk_size = input_filesize;
    tot_nseg = 0;
    do {
        tot_nseg += MAX_SEGMENTS;
        chunk_size = (input_filesize + (tot_nseg - 1)) / tot_nseg;
    } while (chunk_size > MAX_TARGET);
    chunk_size = ((chunk_size + 4095) >> 12) << 12; // 4KB units

    chunk_t chunks[2][MAX_SEGMENTS];
    memset(chunks, 0, sizeof(chunks));
    uint32_t *list = malloc(TABLE_ITEMS << 2);
    if(!list) {
        perror("malloc");
        return 1;
    }
    list[0] =  0;
    list[1] = (PGZ_MAGIC_1 << 16) | (chunk_size >> 12);

    /* ---- deal with the output file, when '-c' isn't among arguments ---- */
    if(!opt_stdout) {
        size_t len = strlen(names[0]) + 4;
        char *str = malloc(len);
        if(!str) {
            perror("malloc");
            return 1;
        }
        snprintf(str, len, "%s.gz", names[0]);
        //RAF, TODO: to check the original file permissions, if any than STDIN
        ofd = open(str, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP);
        if (ofd < 0) {
            perror("open");
            return 1;
        }
        free(str);
#if _USE_MMAP
        size_t max_out_size = ((size_t)tot_nseg * ZBUF_MAX_SIZE) + TABLE_BSIZE;

        /* 1. Pre-allocate max size for output mmap */
        if (ftruncate(ofd, max_out_size) < 0) {
            perror("ftruncate");
            return 1;
        }

        /* 2. Map output file into virtual memory */
        out_mmap_base = mmap(NULL, max_out_size,
            PROT_READ | PROT_WRITE, MAP_SHARED, ofd, 0);
        if (out_mmap_base == MAP_FAILED) {
            perror("mmap out");
            return 1;
        }
#endif
    }

    /* ---- process in batches of exactly MAX_SEGMENTS ---- */
    int a = 0, next_idx = 0, current = 0, nbatch = MAX_SEGMENTS;

    /* setup chunk descriptors and output buffers, spawn worker threads */
    pthread_t threads[2][MAX_SEGMENTS];
    memset(threads, 0, sizeof(threads));
    for (int i = 0; i < nbatch; i++) {
        if (chunk_work_start(&threads[a][i],
            &chunks[a][i], current++, ofd))
            return 1;
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
    while(next_idx < tot_nseg)
    {
        for (int i = 0, b = !a; i < nbatch; i++)
        {
#if _THR_WAIT
            pthread_join(threads[a][i], NULL);
#else
            _cpu_relax();
#endif
            chunk_t *c = &chunks[a][i];
            if (c->error) {
                print2("compression failed on chunk %d, size: %lu\n",
                    current + i, c->out_len);
                return 1;
            }
            if (c->state != 3)
                continue;

#if 0
if (ofd != STDOUT_FILENO || c->idx == next_idx)
fprintf(stderr, ">>> cur: %2d / %2d, idx: %2d vs %2d (ofd: %d), pth: %lu/%d\n",
    current, tot_nseg, c->idx, next_idx, ofd,
    threads[a][i], chunks[b][i].state);
#endif

            /* create another thread to do work */
            if (!chunks[b][i].state
            && !threads[b][i]
            && current < tot_nseg
            ){
                if (chunk_work_start(&threads[b][i],
                    &chunks[b][i], current++, ofd))
                    return 1;
#if _THR_WAIT
#else
                pthread_detach(threads[a][i]);
#endif
            }

            /* ordered writing on STDOUT, only */
            if (ofd == STDOUT_FILENO) {
                if (c->idx != next_idx)
                    continue;
            }

#if 0
fprintf(stderr, ">>> pid: %lu, ofd: %d, nxt: %d / %d \n",
    threads[a][i], ofd, next_idx, tot_nseg);
#endif

            /* disposing the thread */
            threads[a][i] = 0;

            /* granting the correct order */
            next_idx++;
            outlen += c->out_len;
            list[ 2+c->idx ] = (ofd == STDOUT_FILENO)
                             ? full_write(ofd, c->out, c->out_len)
                             : c->out_len
                             ;

            /* disposing the chunk and its buffer */
            void  *buf = c->out;
            size_t cap = c->out_cap;
            memset(c, 0, sizeof(chunk_t));

#if _USE_FREE
            free(buf);
#else
            if(i+1 < nbatch)
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

    /*
     * In-place File Reorganization using Kernel-Level Zero-Copy
     */
    /* Loop through all compressed chunk lengths stored in list[]  */
    off_t write_pos = list[2]; /* Start immediately after Chunk 0 */
    for (int i = 1; i < tot_nseg; i++) {
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
    /* Update outlen and truncate remaining sparse tail */
    outlen = write_pos;
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
     * https://github.com/robang74/uzpexec#parallel-ungzip
     */
    if(list_enabled) {
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
        outlen += full_write(ofd, p, TABLE_BSIZE);
    }

    if(opt_verbose) {
        fprintf(stderr, "file: %d x %zu = %ld, size: %lu (%0.1f%%, -%d)\n",
            tot_nseg, chunk_size, input_filesize, outlen,
            (float)outlen*100/input_filesize,
            compression_level);
    }

    return 0;
}
