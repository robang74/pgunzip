// (c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, GPL v2

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

//#include <linux/fs.h>

#define MAX_SEGMENTS    6
#define MAX_TARGET     (1UL << 20)     /* max target size per segment */

#define ALWAYS_INLINE __attribute__((always_inline)) inline

#ifndef _USE_SENDFILE
#define _USE_SENDFILE 1
#endif
#ifndef _USE_MMPWRITE
#define _USE_MMPWRITE 1
#endif

typedef struct {
    off_t   offset;
    size_t  len;
    int     pout;       /* memfd: child -> parent (gzip stdout) */
    int     infd;
    pid_t   pid;
} chunk_t;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */
static ALWAYS_INLINE int create_memfd(const char *name)
{
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) perror("memfd_create");
    return fd;
}

static void chunk_destroy(chunk_t *c)
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
    if (c->infd >= 0) {
        close(c->infd);
        c->infd = -1;
    }

}

/* ------------------------------------------------------------------ */
/* Zero-copy staging: kernel copies from src_fd to dst_fd             */
/* ------------------------------------------------------------------ */
static int stage_chunk(int src_fd, size_t total, chunk_t *c)
{
  off_t src_off = c->offset;
  size_t left = c->len;
  int dst_fd = c->infd;
#if 0 //copy_file_range: Invalid cross-device link
    /* Try copy_file_range first (Linux 4.5+, pure kernel copy) */
    //fprintf(stderr, "1\n");
    while (left > 0) {
        off_t off_out = 0;
        ssize_t n = copy_file_range(src_fd, &src_off, dst_fd, &off_out, left, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EINVAL || errno == ENOSYS || errno == EXDEV) {
                //perror("copy_file_range");
                break;          /* fall through to sendfile */
            }
            return -1;
        }

        if (n == 0)
            break;
        left -= n;
    }
    if (left == 0)
        goto seek_reset;
    if (ftruncate(dst_fd, 0) < 0)
       return -1;              /* wipe partial copy_file_range data */
#endif
#if _USE_SENDFILE
    /* Fallback: sendfile (Linux 2.6.33+, also kernel-internal copy) */
    //fprintf(stderr, "2\n");
    while (left > 0) {
        ssize_t n = sendfile(dst_fd, src_fd, &src_off, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EINVAL || errno == ENOSYS || errno == EXDEV) {
                //perror("sendfile");
                break;           /* fall through to mmap+write */
            }
            return -1;
        }
        if (n == 0)
            break;
        left -= n;
    }
    if (left == 0)
        goto seek_reset;
#endif
#if _USE_MMPWRITE
    //fprintf(stderr, "3, left: %lu\n", left);
    /* ---- mmap the whole file (zero-copy read source) ---- */
    static unsigned char *mmap_base = NULL;
    if(!mmap_base) {
        mmap_base = mmap(NULL, total, PROT_READ, MAP_PRIVATE, src_fd, 0);
        if (mmap_base == MAP_FAILED) {
            perror("mmap");
            close(src_fd);
            return 1;
        }
        close(src_fd);   /* fd no longer needed; mapping stays valid */
        src_fd = -1;
    }
    /* Ultimate fallback: mmap + write (what you have now) */
    unsigned char *src = mmap_base + src_off;
    while (left > 0) {
        ssize_t w = write(dst_fd, src, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            perror("write");
            return -1;
        }
        src += w;
        left -= w;
    }
#endif
seek_reset:
    if (lseek(dst_fd, 0, SEEK_SET) == (off_t)-1)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Fork gzip; infd is a ready-to-read fd (e.g. memfd at offset 0)    */
/* ------------------------------------------------------------------ */
static int spawn_gzip(int infd, chunk_t *c)
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

static int dump_chunk_to_stdout(chunk_t *c)
{
    static char *buf = NULL;
    if (lseek(c->pout, 0, SEEK_SET) == (off_t)-1)
        return -1;
    if(!buf) buf = malloc(MAX_TARGET);
    if(!buf) return -1;
    while (1) {
        ssize_t n = read(c->pout, buf, MAX_TARGET);
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
    int ret = 0;
    off_t total;

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
    if (st.st_size == 0) {
        close(infd);
        return 0;
    }
    total = st.st_size;

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
        chunk_t chunks[MAX_SEGMENTS];
        int batch_chunks = 0;

        /* ---- define batch boundaries ---- */
        off_t off = pos;
        for (int j = 0; j < nseg && off < total; j++) {
            chunks[j].offset = off;
            chunks[j].len = (off + (off_t)chunk_size > total)
                          ? (size_t)(total - off) : chunk_size;
            chunks[j].pout = -1;
            chunks[j].pid  = -1;
            chunks[j].infd = -1;
            off += (off_t)chunks[j].len;
            batch_chunks++;
        }

        /* ---- STAGE: copy each chunk into its own memfd ---- */
        for (int j = 0; j < batch_chunks; j++) {
            chunks[j].infd = create_memfd("chunk_in");
            if (chunks[j].infd < 0) {
                perror("create_memfd");
                goto cleanup;
            }
            if (stage_chunk(infd, total, &chunks[j]) < 0) {
                perror("stage_chunk");
                goto cleanup;
            }
        }

        /* ---- SPAWN ALL children at once ---- */
        for (int j = 0; j < batch_chunks; j++) {
            if (spawn_gzip(chunks[j].infd, &chunks[j]) < 0) {
                chunks[j].infd = -1; /* spawn closed it on failure */
                goto cleanup;
            }
        }

        for (int j = 0; j < batch_chunks; j++) {
            int status;
            fdatasync(chunks[j].infd);
            chunks[j].infd = -1; /* ownership transferred to child */
            /* ---- WAIT children ---- */
            if (waitpid(chunks[j].pid, &status, 0) < 0)
                perror("waitpid");
            chunks[j].pid = -1;
            /* ---- DUMP in strict segment order ---- */
            if (dump_chunk_to_stdout(&chunks[j]) < 0) {
                perror("reassembly");
                goto cleanup;
            }
            chunk_destroy(&chunks[j]);
        }

        pos += off - pos;
        continue;

    cleanup:
    /* exit() is going to clean everything
        for (int j = 0; j < batch_chunks; j++) {
            chunk_destroy(&chunks[j]);
        }
    */
        ret = 1;
        break;
    }

    //munmap(mmap_base, total);
    //fdatasync(STDOUT_FILENO);
    return ret;
}
