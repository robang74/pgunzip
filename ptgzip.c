// (c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, GPL v2

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
    char     state;
    char     error;
    int      idx;
} chunk_t __attribute((aligned(4)));

/* ------------------------------------------------------------------ */
/* Thread worker: compress one chunk directly to its output buffer    */
/* ------------------------------------------------------------------ */
#ifndef _OUT_FREE
#define _OUT_FREE 0
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
#define ZBUF_MAX_SIZE (MAX_TARGET + (MAX_TARGET >> 9) + 256)
static int compression_level = Z_DEFAULT_COMPRESSION;
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
    if(_OUT_FREE || !c->out) {
        c->out_cap = _deflate_bound(&strm, c->len);
        c->out = malloc(c->out_cap);
    }
    if (!c->out) {
        goto reterr;
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
#if _OUT_FREE
        free(c->out);
        c->out = NULL;
#endif
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
    return NULL;
}

static size_t full_write(int fd, const void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;

    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write");
            return 1;
        }
        p   += w;
        len -= w;
    }

    return p - (unsigned char *)buf;
}

static unsigned char *mmap_base;
static off_t input_filesize;
static size_t chunk_size;
static int tot_nseg;

static ALWAYS_INLINE
chunk_t *chunk_init(chunk_t *c, int idx)
{
    size_t len;
init_retray:
    c->offset = (off_t)idx * chunk_size;
    len = (idx == tot_nseg - 1)
           ? (size_t)(input_filesize - c->offset)
           : chunk_size;
#if _OUT_FREE
#else
    if(c->out && c->len && len > c->len) {
        fprintf(stderr, "DBG> chunk_init() c->len mess!\n");
        exit(127); //RAF,TODO: debug only
        free(c->out);
        memset(c, 0, sizeof(chunk_t));
        goto init_retray;
    }
#endif
    c->len = len;
    c->in = mmap_base + c->offset;
    c->state = 1;
    c->error = 0;
    c->idx = idx;
    return c;
}

static ALWAYS_INLINE
int chunk_work_start(pthread_t *p, chunk_t *c, int idx)
{
    if (pthread_create(p, NULL, thread_compress, chunk_init(c, idx)))
    {
        perror("pthread_create");
        return 1;
    }
    //pthread_join(*p, NULL);
    return 0;
}

/* ========================================================================== */
/* Main                                                                       */
/* ========================================================================== */
#include <getopt.h>
#define print2(fmt...) while(!opt_quiet) { fprintf(stderr, fmt); break; }

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

#define TABLE_ITEMS ((uint32_t)tot_nseg + 4)
#define TABLE_BSIZE ((TABLE_ITEMS) << 2)
#define PGZ_MAGIC_1 0x6274
#define PGZ_MAGIC_2 0x7a70

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
        perror("mmap");
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
    uint32_t n = 0, *list = malloc(TABLE_ITEMS << 2);
    if(list) {
        list[n++] =  0;
        list[n++] = (PGZ_MAGIC_1 << 16) | (chunk_size >> 12);
    }

    /* ---- process in batches of exactly MAX_SEGMENTS ---- */
    int a = 0, next_idx = 0, current = 0, nbatch = MAX_SEGMENTS;

    /* setup chunk descriptors and output buffers, spawn worker threads */
    pthread_t threads[2][MAX_SEGMENTS];
    memset(threads, 0, sizeof(threads));
    for (int i = 0; i < nbatch; i++) {
        if (chunk_work_start(&threads[a][i],
            &chunks[a][i], current++))
            return 1;
        //RAF: the bottleneck is the next one in the ordered list
        //     since after the first the father starts to write
        //     then the bottleneck is the first one, let it go!
        if(!i) sched_yield();
    }

    /* write compressed chunks to stdout in strict segment order */
    while(next_idx < tot_nseg)
    {
        for (int i = 0, b = !a; i < nbatch; i++)
        {
            pthread_join(threads[a][i], NULL);
            chunk_t *c = &chunks[a][i];
            if (c->error) {
                print2("compression failed on chunk %d, size: %lu\n",
                    current + i, c->out_len);
                return 1;
            }

            void *buf  = c->out;
            size_t len = c->out_len, cap = c->out_cap;
            memset(c, 0, sizeof(chunk_t));
/*
            c->out = NULL;
            c->state = 0;
            c->len = 0;
*/
            if (current < tot_nseg)
                if (chunk_work_start(&threads[b][i],
                    &chunks[b][i], current++))
                    return 1;

            if(list) list[n++] = len;
            outlen += full_write(ofd, buf, len);
            /* granting the correct order */
            next_idx++;
#if _OUT_FREE
            free(buf);
#else
            if(i+1 >= nbatch)
                c = &chunks[a][ 0 ]; //RAF: it will be the next one
            else
                c = &chunks[b][i+1];
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

    /*
     * https://github.com/robang74/uzpexec#parallel-ungzip
     */
    if(list) {
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

/*
    free(list);
    for (int j = 0; j < nbatch; j++)
        free(chunks[j].out);
    munmap(mmap_base, input_filesize);
*/
    return 0;
}
