// (c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, GPL v2

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>

#define MAX_TARGET     (1UL << 20)     /* max target size per segment */
#define MAX_SEGMENTS    6
#define IO_BUFSZ       (64 * 1024)

typedef struct {
    off_t   offset;
    size_t  len;
    int     pout;       /* memfd: child -> parent (gzip stdout) */
    pid_t   pid;
} Chunk;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */
static int create_memfd(const char *name)
{
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) perror("memfd_create");
    return fd;
}

static void chunk_destroy(Chunk *c)
{
    if (c->pout >= 0) {
        close(c->pout);
        c->pout = -1;
    }
    if (c->pid > 0) {
        int st;
        if (waitpid(c->pid, &st, WNOHANG) == 0)
            kill(c->pid, SIGTERM);
        c->pid = -1;
    }

}

/* ------------------------------------------------------------------ */
/* Fork gzip; infd is a ready-to-read fd (e.g. memfd at offset 0)    */
/* ------------------------------------------------------------------ */
static int spawn_gzip(int infd, Chunk *c)
{
    c->pout = create_memfd("gz_out");
    if (c->pout < 0) {
        close(infd);
        return -1;
    }

    pid_t p = fork();
    if (p < 0) {
        close(infd);
        close(c->pout);
        return -1;
    }

    if (p == 0) {
        /* child */
        close(STDIN_FILENO);
        if (dup(infd) != STDIN_FILENO)
            _exit(126);
        close(infd);

        close(STDOUT_FILENO);
        if (dup(c->pout) != STDOUT_FILENO)
            _exit(126);
        close(c->pout);

        execlp("/bin/gzip", "gzip", "-c", (char *)NULL);
        _exit(127);
    }

    /* parent keeps pout; gives up infd */
    close(infd);
    c->pid = p;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Copy compressed chunk memfd to stdout                              */
/* ------------------------------------------------------------------ */
static int dump_chunk_to_stdout(Chunk *c)
{
    char buf[IO_BUFSZ];

    if (lseek(c->pout, 0, SEEK_SET) == (off_t)-1)
        return -1;

    while (1) {
        ssize_t n = read(c->pout, buf, IO_BUFSZ);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0)
            break;

        size_t done = 0;
        while (done < (size_t)n) {
            ssize_t w = write(STDOUT_FILENO, buf + done, n - done);
            if (w < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            done += w;
        }
    }
    return 0;
}

/* ================================================================== */
/* Main                                                               */
/* ================================================================== */
int main(int argc, char **argv)
{
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
        close(infd);
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: not a regular file\n");
        close(infd);
        return 1;
    }

    off_t total = st.st_size;
    if (total == 0) {
        close(infd);
        return 0;
    }

    /* ---- mmap the whole file (zero-copy read source) ---- */
    unsigned char *mmap_base = mmap(NULL, total, PROT_READ, MAP_PRIVATE, infd, 0);
    if (mmap_base == MAP_FAILED) {
        perror("mmap");
        close(infd);
        return 1;
    }
    close(infd);   /* fd no longer needed; mapping stays valid */

    /* ---- decide chunk size and total number of chunks ---- */
    int nseg = MAX_SEGMENTS, i = 1;
    size_t chunk_size = total;
    do {
        chunk_size = (total + (nseg * i - 1)) / (nseg * i);
        i++;
    } while (chunk_size > MAX_TARGET);
    i--; /* actual multiplier */
    fprintf(stderr, "chunks: %d x %zu = %ld / %d\n",
            nseg, chunk_size, total, nseg * i);

    off_t pos = 0;
    while (pos < total) {
        Chunk chunks[MAX_SEGMENTS];
        int in_fds[MAX_SEGMENTS];
        int batch_chunks = 0;

        /* ---- define batch boundaries ---- */
        off_t off = pos;
        for (int j = 0; j < nseg && off < total; j++) {
            chunks[j].offset = off;
            chunks[j].len = (off + (off_t)chunk_size > total)
                          ? (size_t)(total - off) : chunk_size;
            chunks[j].pout = -1;
            chunks[j].pid  = -1;
            in_fds[j] = -1;
            off += (off_t)chunks[j].len;
            batch_chunks++;
        }

        /* ---- STAGE: copy each chunk into its own memfd ---- */
        for (int j = 0; j < batch_chunks; j++) {
            in_fds[j] = create_memfd("chunk_in");
            if (in_fds[j] < 0)
                goto cleanup;

            size_t left = chunks[j].len;
            unsigned char *src = mmap_base + chunks[j].offset;
            while (left > 0) {
                ssize_t w = write(in_fds[j], src, left);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    perror("write memfd");
                    goto cleanup;
                }
                src += w;
                left -= w;
            }
            if (lseek(in_fds[j], 0, SEEK_SET) == (off_t)-1) {
                perror("lseek memfd");
                goto cleanup;
            }
        }

        /* ---- SPAWN ALL children at once ---- */
        for (int j = 0; j < batch_chunks; j++) {
            if (spawn_gzip(in_fds[j], &chunks[j]) < 0) {
                in_fds[j] = -1; /* spawn closed it on failure */
                goto cleanup;
            }
            in_fds[j] = -1; /* ownership transferred to child */
        }


        for (int j = 0; j < batch_chunks; j++) {
            int status;
            /* ---- WAIT children ---- */
            if (waitpid(chunks[j].pid, &status, 0) < 0)
                perror("waitpid");
            chunks[j].pid = -1;
            /* ---- DUMP in strict segment order ---- */
            if (dump_chunk_to_stdout(&chunks[j]) < 0) {
                perror("reassembly");
                goto cleanup;
            }
            if (in_fds[j] >= 0) {
                close(in_fds[j]);
                in_fds[j] = -1;
            }
            chunk_destroy(&chunks[j]);
        }

        pos += off - pos;
        continue;

    cleanup:
        for (int j = 0; j < batch_chunks; j++) {
            if (in_fds[j] >= 0) close(in_fds[j]);
            chunk_destroy(&chunks[j]);
        }
        munmap(mmap_base, total);
        return 1;
    }

    munmap(mmap_base, total);
    return 0;
}
