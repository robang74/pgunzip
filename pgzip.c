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
#define MIN_PARALLEL   (64 * 1024)     /* < 64 KiB: avoid parallelism */
#define IO_BUFSZ       (64 * 1024)     /* bounce buffer size */

typedef struct {
    off_t   offset;     /* start position in input file (lseek) */
    size_t  len;        /* bytes to feed this child */
    int     pin[2];     /* pipe: parent -> child (gzip stdin) */
    int     pout;       /* memfd/tmpfile: child -> parent (gzip stdout) */
    pid_t   pid;
    int     active;
} Chunk;

/* ------------------------------------------------------------------ */
/* Create a seekable, growable output sink for a child process.       */
/* Linux: memfd_create.  Others: unnamed tmpfile via dup().          */
/* ------------------------------------------------------------------ */
static int create_capture_fd(void)
{
    int fd = memfd_create("gz_out", MFD_CLOEXEC);
    if (fd >= 0)
        return fd;

    /* Portable fallback */
    FILE *tmp = tmpfile();
    if (!tmp)
        return -1;
    fd = dup(fileno(tmp));   /* keep fd after fclose */
    fclose(tmp);
    return fd;
}

/* ------------------------------------------------------------------ */
/* Close all resources belonging to a chunk; kill child if still alive */
/* ------------------------------------------------------------------ */
static void chunk_destroy(Chunk *c)
{
    if (c->pin[1] >= 0) close(c->pin[1]);
    if (c->pin[0] >= 0) close(c->pin[0]);
    if (c->pout >= 0)   close(c->pout);

    if (c->pid > 0) {
        int st;
        if (waitpid(c->pid, &st, WNOHANG) == 0)
            kill(c->pid, SIGTERM);
    }
    c->active = 0;
}

/* ------------------------------------------------------------------ */
/* Fork a gzip child; wire pin[0] to stdin and pout to stdout via dup */
/* ------------------------------------------------------------------ */
static int spawn_gzip(Chunk *c)
{
    if (pipe(c->pin) < 0)
        return -1;

    c->pout = create_capture_fd();
    if (c->pout < 0)
        goto fail;

    pid_t p = fork();
    if (p < 0)
        goto fail;

    if (p == 0) {
        /* -------- child -------- */
        close(c->pin[1]);

        /* dup() to lowest available fd: after close(0) this is stdin */
        close(STDIN_FILENO);
        if (dup(c->pin[0]) != STDIN_FILENO)
            _exit(126);
        close(c->pin[0]);

        /* after close(1) this becomes stdout */
        close(STDOUT_FILENO);
        if (dup(c->pout) != STDOUT_FILENO)
            _exit(126);

        /* stderr left untouched so errors are visible */
        execlp("gzip", "gzip", "-c", (char *)NULL);
        _exit(127);
    }

    /* -------- parent -------- */
    close(c->pin[0]);
    c->pin[0] = -1;
    c->pid   = p;
    c->active = 1;
    return 0;

fail:
    if (c->pin[0] >= 0) close(c->pin[0]);
    if (c->pin[1] >= 0) close(c->pin[1]);
    if (c->pout >= 0)   close(c->pout);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Parent: lseek to chunk offset and pump its bytes into gzip stdin   */
/* ------------------------------------------------------------------ */
static int feed_chunk(int infd, Chunk *c)
{
    char buf[IO_BUFSZ];

    if (lseek(infd, c->offset, SEEK_SET) == (off_t)-1)
        return -1;

    size_t left = c->len;
    while (left > 0) {
        size_t want = left > IO_BUFSZ ? IO_BUFSZ : left;
        ssize_t n = read(infd, buf, want);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0)          /* unexpected EOF */
            break;

        size_t done = 0;
        while (done < (size_t)n) {
            ssize_t w = write(c->pin[1], buf + done, n - done);
            if (w < 0) {
                if (errno == EINTR) continue;
                return -1;   /* broken pipe or I/O error */
            }
            done += w;
        }
        left -= n;
    }

    close(c->pin[1]);
    c->pin[1] = -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parent: lseek capture fd to 0 and copy to program stdout           */
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

    /* Prevent SIGPIPE from killing us if a child dies early */
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
    off_t pos   = 0;
    int n, nseg = MAX_SEGMENTS, i = 1;
    size_t chunk_size = total;
    do {
      n = nseg * i++;
      chunk_size = (total + (n-1)) / n;
    } while (chunk_size > MAX_TARGET);
    fprintf(stderr, "chunks: %d x %lu = %lu / %lu\n",
      nseg, chunk_size, total, (total + chunk_size-1)/chunk_size);

    /* -------------------------------------------------------------- */
    /* Outer loop: one batch = up to 6 MiB                            */
    /* -------------------------------------------------------------- */
    while (pos < total) {

        off_t remain = total - pos;
        size_t base = chunk_size;

        Chunk chunks[MAX_SEGMENTS];
        memset(chunks, 0, sizeof(chunks));

        off_t off = pos;
        for (int i = 0; i < nseg; i++) {
            chunks[i].offset = off;
            chunks[i].len    = base;
            chunks[i].pin[0] = -1;
            chunks[i].pin[1] = -1;
            chunks[i].pout   = -1;
            off += (off_t)chunks[i].len;
        }

        for (int i = 0; i < nseg; i++) {
            /* ---- fork all gzip workers for this batch ---- */
            if (spawn_gzip(&chunks[i]) < 0) {
                for (int j = 0; j < nseg; j++)
                    chunk_destroy(&chunks[j]);
                close(infd);
                return 1;
            }
            /* ---- feed each segment via lseek + pipe ---- */
            if (feed_chunk(infd, &chunks[i]) < 0) {
                for (int j = 0; j < nseg; j++)
                    chunk_destroy(&chunks[j]);
                close(infd);
                return 1;
            }
        }

        for (int i = 0; i < nseg; i++) {
            int status;
            /* ---- wait children ---- */
            if (waitpid(chunks[i].pid, &status, 0) < 0)
                perror("waitpid");
            chunks[i].pid = -1;
            /* ---- combine outputs in strict segment order ---- */
            if (dump_chunk_to_stdout(&chunks[i]) < 0) {
                perror("reassembly");
                for (int j = i; j < nseg; j++)
                    chunk_destroy(&chunks[j]);
                close(infd);
                return 1;
            }
            chunk_destroy(&chunks[i]);
            pos += base;
        }
    }

    close(infd);
    return 0;
}
