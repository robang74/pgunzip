/*
 * (c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, GPL v2
 *
 * I wrote a canvas of the desiderated function and I asked Gemini to complete
 * the C-language code. The result below has been further edited by me. This
 * is a way to write fast code, let the AI doing the boring work and keeping
 * the control about the architecture. Simple as example, but a valid example.
 *
 * - https://share.gemini.google/3lZU2K71iWhd
 *
 * In the canvas, I made three mistakes that Gemini was unable to indetify:
 * - 1. put `-n` in do_stuff() instead of chunk_seeker() and
 * - 2. the `for(n)` can stop for `break` of `n >= large_size`
 * - 3. malloc() is to short and can generate a buffer overflow
 * The first bug, Gemini had no information to correct it because both the
 * functions involved are unkown for her. The others two, should have had to.
 *
 * - https://share.gemini.google/G5Spd1O9IXEW
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <semaphore.h>
#include <pthread.h>

extern size_t xfull_read(int fd, const void *buf, size_t len);
extern size_t search_magic(uint8_t *buf, size_t n, size_t cur);

typedef struct {
    uint8_t *buf;  /* Pointer to the shared buffer */
    size_t sze;    /* Total size capacity of buf */
    size_t cur;    /* Current valid readable/searched offset updated by master */
    bool end;      /* Termination flag set by master on EOF or read complete */
    sem_t *smp;    /* Pointer to semaphore signaling new data available */
} seek_t;

static ALWAYS_INLINE
void do_stuff(uint8_t *buf, size_t len)
{
    fprintf(stderr, ">> do_stuff: %p, val: 0x%08x, len: %8lu\n",
        buf, *(uint32_t *)(buf + len), len);
}

/* Thread worker performing magic search on incoming read data */
static void *thread_seeker(void *arg)
{
    seek_t *s = (seek_t *)arg;
    size_t n = 0;
    size_t f;

    while (!s->end)
    {
        sem_wait(s->smp);
//      fprintf(stderr, "2> n: %8lu, cur: %8lu\n", n, s->cur);
        if (s->end) break;

        /* Search in range [n+1, s->cur-1]  */
        if (s->cur > n) {
            f = chunk_seeker(&s->buf[n], s->cur - n); // -n: bugfix
            if (f) {                                        //
                do_stuff(&s->buf[n], f); // -n: bugfix  //////
                n += f;
            } else {
                n = s->cur; // Not found yet
            }
        }
    }

    return NULL;
}

#define READ_SIZE (1UL << 16)

void xread_and_split(int fd, size_t large_size)
{
    seek_t s = {0};
    pthread_t tid;
    sem_t sem;
    size_t len;

    sem_init(&sem, 0, 0);

    s.buf = malloc(large_size + READ_SIZE); // bugfix, avoid buffer overflow
    s.sze = large_size;
    s.smp = &sem;
    s.cur = 0;
    s.end = false;

    if (!s.buf) {
        perror("malloc");
        exit(-1);
    }

    /* Spawn background seeker thread */
    if (pthread_create(&tid, NULL, thread_seeker, &s) != 0) {
        perror("pthread_create");
        exit(-1);
    }

    for (size_t n = 0; n < large_size; ) {
        len = xfull_read(fd, &s.buf[n], READ_SIZE);
//      fprintf(stderr, "1> n: %8lu, cur: %8lu\n", n, s.cur);
        if (!len) break; // blocking-bug fix  ////
                                                //
        n += len;                               //
        s.cur = n;                              //
        sem_post(s.smp);                        //
    }                                           //
                                                //
    // blocking-bug fix  /////////////////////////
    s.end = true;
    sem_post(s.smp);
    pthread_join(tid, NULL);

    sem_destroy(&sem);
}
