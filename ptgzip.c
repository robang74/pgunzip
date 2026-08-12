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
#include <zlib.h>
#include <pthread.h>

#define MAX_TARGET     (1UL << 20)     /* max target size per segment */
#define MAX_SEGMENTS    6

typedef struct {
    off_t   offset;
    size_t  len;
    unsigned char *in;      /* pointer into mmap */
    unsigned char *out;
    size_t  out_cap;
    size_t  out_len;
    int     error;
} Chunk;

/* ------------------------------------------------------------------ */
/* Exact upper bound for a gzip chunk without keeping a stream alive   */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* Thread worker: compress one chunk directly to its output buffer     */
/* ------------------------------------------------------------------ */
static void *thread_compress(void *arg)
{
    Chunk *c = arg;
    z_stream strm = {0};

    int ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                           15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        c->error = 1;
        return NULL;
    }

    strm.next_in  = c->in;
    strm.avail_in = c->len;
    strm.next_out = c->out;
    strm.avail_out = c->out_cap;

    ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        c->error = 1;
        deflateEnd(&strm);
        return NULL;
    }

    c->out_len = strm.total_out;
    deflateEnd(&strm);
    c->error = 0;
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
    int total_chunks;
    size_t chunk_size;
    {
        int mult = 1;
        do {
            total_chunks = MAX_SEGMENTS * mult++;
            chunk_size = (total + total_chunks - 1) / total_chunks;
        } while (chunk_size > MAX_TARGET);
    }

    fprintf(stderr, "chunks: %d x %zu = %ld / %d\n",
            MAX_SEGMENTS, chunk_size, total, total_chunks);

    Chunk chunks[MAX_SEGMENTS];

    /* ---- process in batches of exactly MAX_SEGMENTS ---- */
    for (int batch_start = 0; batch_start < total_chunks; batch_start += MAX_SEGMENTS) {
        int batch_end = batch_start + MAX_SEGMENTS;
        if (batch_end > total_chunks)
            batch_end = total_chunks;
        int nbatch = batch_end - batch_start;

        /* setup chunk descriptors and output buffers */
        for (int i = 0; i < nbatch; i++) {
            int idx = batch_start + i;
            chunks[i].offset = (off_t)idx * chunk_size;
            chunks[i].len = (idx == total_chunks - 1)
                          ? (size_t)(total - chunks[i].offset)
                          : chunk_size;
            chunks[i].in = mmap_base + chunks[i].offset;
            chunks[i].out_cap = gzip_bound_size(chunks[i].len);
            chunks[i].out = malloc(chunks[i].out_cap);
            if (!chunks[i].out) {
                perror("malloc");
                //munmap(mmap_base, total);
                return 1;
            }
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
