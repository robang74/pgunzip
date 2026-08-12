// (c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, GPL v2

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <pthread.h>

#define MAX_SEGMENTS    6
#define MAX_TARGET     (1UL << 20)     /* max target size per segment */

#ifndef _BE_VERBOSE
#define _BE_VERBOSE     0
#endif

typedef struct {
    off_t   offset;
    size_t  len;
    unsigned char *in;      /* pointer into mmap */
    unsigned char *out;
    size_t  out_cap;
    size_t  out_len;
    int     error;
} chunk_t;

/* ------------------------------------------------------------------ */
/* Exact upper bound for a gzip chunk without keeping a stream alive   */
/* ------------------------------------------------------------------ */
#if 0
static size_t gzip_bound_size(size_t src_len)
{
    z_stream strm = {0};
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return src_len + (src_len >> 9) + 256;
    size_t bound = deflateBound(&strm, src_len);
    deflateEnd(&strm);
    return bound;
}
#endif
/* ------------------------------------------------------------------ */
/* Thread worker: compress one chunk directly to its output buffer     */
/* ------------------------------------------------------------------ */
#ifndef _USE_ZNG
#define _USE_ZNG 0
#endif
#if _USE_ZNG
#include "zlib-ng.h"
#define _deflate_init2 zng_deflateInit2
#define _deflate_bound zng_deflateBound
#define _deflate_end   zng_deflateEnd
#define _deflate       zng_deflate
#define _stream_t      zng_stream
#else
#include <zlib.h>
#define _deflate_init2     deflateInit2
#define _deflate_bound     deflateBound
#define _deflate_end       deflateEnd
#define _deflate           deflate
#define _stream_t        z_stream
#endif
static void *thread_compress(void *arg)
{
    chunk_t *c = arg;
    _stream_t strm = {0};
    int ret;

    /* 1. GZIP FORMAT: 15 + 16 is mandatory.
     *    deflateInit() produces RFC-1950 zlib format, not RFC-1952 gzip.
     *    Without +16 the output cannot be concatenated into a valid .gz file.
     */
    ret = _deflate_init2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                    15 + 16, 7, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        c->error = 1;
        return NULL;
    }

    /* 2. OUTPUT BUFFER: must be deflateBound(), never c->len.
     *    Incompressible data EXPANDS by ~0.1 % + headers.
     *    c->len alone guarantees a buffer overrun on random bytes.
     */
    c->out_cap = _deflate_bound(&strm, c->len);
    c->out = malloc(c->out_cap);
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
    if(c->out_cap >= c->len)
        ret = _deflate(&strm, Z_FINISH);
#if 0 //RAF, TODO: code to be completed
    else
    do {
        if (strm.avail_in == 0) {
            ret = deflate(&strm, Z_FINISH);
        } else {
            ret = deflate(&strm, Z_NO_FLUSH);
            /* move the output out of the next_out buffer */
        }
    } while (ret == Z_OK);
#endif
    if (ret != Z_STREAM_END) {
        free(c->out);
        c->out = NULL;
reterr:
        c->error = 1;
        goto endfnc;
    }

    c->out_len = strm.total_out;

    /* 4. CLEANUP: always call deflateEnd() to free internal buffers.
     *    Skipping it leaks several KiB per chunk.
     */
    c->error = 0;
endfnc:
    _deflate_end(&strm);
    return NULL;
}

/* ================================================================== */
/* Main                                                               */
/* ================================================================== */
int main(int argc, char **argv)
{
    int ret = 0;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int infd = open(argv[1], O_RDONLY);
    if (infd < 0) {
        perror("open");
        return 1;
    }

    struct stat st;
    if (fstat(infd, &st) < 0) {
        perror("fstat");
        //close(infd);
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: not a regular file\n");
        //close(infd);
        return 1;
    }

    off_t total = st.st_size;
    if (total == 0) {
        //close(infd);
        return 0;
    }

    /* ---- mmap entire file (zero-copy input for all threads) ---- */
    unsigned char *mmap_base = mmap(NULL, total, PROT_READ, MAP_PRIVATE, infd, 0);
    if (mmap_base == MAP_FAILED) {
        perror("mmap");
        //close(infd);
        return 1;
    }
    close(infd);   /* kernel keeps the mapping via vnode reference */

    /* ---- decide chunk size and total number of chunks (multiples of 6) ---- */
    size_t chunk_size = total;
    int nseg = 0;
    do {
        nseg += MAX_SEGMENTS;
        chunk_size = (total + (nseg - 1)) / nseg;
    } while (chunk_size > MAX_TARGET);
    chunk_size = ((chunk_size + 4095) >> 12) << 12; // 4KB units

#if _BE_VERBOSE
    fprintf(stderr, "chunks: %d x %zu = %ld / %d\n",
            MAX_SEGMENTS, chunk_size, total, nseg);
#endif
    chunk_t chunks[MAX_SEGMENTS];

    /* ---- process in batches of exactly MAX_SEGMENTS ---- */
    for (int batch_start = 0; batch_start < nseg; batch_start += MAX_SEGMENTS) {
        int batch_end = batch_start + MAX_SEGMENTS;
        if (batch_end > nseg)
            batch_end = nseg;
        int nbatch = batch_end - batch_start;

        /* setup chunk descriptors and output buffers */
        for (int i = 0; i < nbatch; i++) {
            int idx = batch_start + i;
            chunks[i].offset = (off_t)idx * chunk_size;
            chunks[i].len = (idx == nseg - 1)
                          ? (size_t)(total - chunks[i].offset)
                          : chunk_size;
            chunks[i].in = mmap_base + chunks[i].offset;
            chunks[i].error = 0;
        }

        /* spawn worker threads */
        pthread_t threads[MAX_SEGMENTS];
        for (int i = 0; i < nbatch; i++) {
            if (pthread_create(&threads[i], NULL, thread_compress, &chunks[i]) != 0) {
                perror("pthread_create");
                /*
                for (int j = 0; j < nbatch; j++)
                    free(chunks[j].out);
                munmap(mmap_base, total);
                */
                return 1;
            }
        }

        /* reap all threads in this batch */
        for (int i = 0; i < nbatch; i++)
            pthread_join(threads[i], NULL);

        /* write compressed chunks to stdout in strict segment order */
        for (int i = 0; i < nbatch; i++) {
            if (chunks[i].error) {
                fprintf(stderr, "compression failed on chunk %d\n", batch_start + i);
                /*
                for (int j = 0; j < nbatch; j++)
                    free(chunks[j].out);
                munmap(mmap_base, total);
                */
                return 1;
            }

            size_t left = chunks[i].out_len;
            unsigned char *p = chunks[i].out;
            while (left > 0) {
                ssize_t w = write(STDOUT_FILENO, p, left);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    perror("write");
                    /*
                    for (int j = i; j < nbatch; j++)
                        free(chunks[j].out);
                    munmap(mmap_base, total);
                    */
                    return 1;
                }
                p += w;
                left -= w;
            }
            free(chunks[i].out);
        }
    }

/*
    for (int j = 0; j < nbatch; j++)
        free(chunks[j].out);
    munmap(mmap_base, total);
*/
    return ret;
}
