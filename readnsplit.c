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
 * The code has been refined with a three-letter variable for role identification
 * this increases code clairity, and clarity highlights the need to have `str`
 * as starting point for `do_stuff()` on the complete chunk. Meanwhile another
 * subtle bug emerged: ``s->cur` can be updated between a reading and another
 * therefore it should be cached in a local variable.
 *
 * In this peculiar case an ALIGNED4 volatile uint32_t access seems safe enough,
 * without dealing with the overhead of atomic pragmas or the mutex additional
 * complexity. Moreover, the same applies to s->end but a bool type is uint8_t,
 * therefore it is atomic as every minimum grained storage variable, but using
 * still using volatile because the value can be cached instead of being re-read.
 *
 * On this topic, it is useless to ask Gemini, a pedantic AI always confabulates
 * especially when it realises that it totally overlooked the synchronisation
 * of the shared data access. However, feel free to continue the conversation
 * above providing this new version of the file. It can be educational, anyway.
 *
 * Compile test and symbols check:
 *
 * - gcc -O2 -Wall -c readnsplit.c -o readnsplit.o && nm readnsplit.o
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <semaphore.h>
#include <pthread.h>

#define ALIGNED4 __attribute__ ((aligned(4)))

extern uint32_t search_magic(uint8_t *buf, uint32_t len);
extern uint32_t xfull_read(int fd, const void *buf, uint32_t len);

// The field ordering should respect the min. alignement even when packed
typedef struct {
    uint8_t *buf;  /* Pointer to the shared buffer */
    uint32_t sze;  /* Total size capacity of buf */
    uint32_t cur;  /* Current valid readable/searched offset updated by master */
    sem_t  *smp;   /* Pointer to semaphore signaling new data available */
    uint8_t end;   /* Termination flag set by master on EOF or read complete */
} seek_t ALIGNED4; // Needed by volatile access instead using atomic or mutex

static inline
void do_stuff(const uint8_t *buf, uint32_t srt, uint32_t end)
{
    buf += srt;
    fprintf(stderr, ">> do_stuff: %p, val: 0x%08x, len: %8u\n",
        buf, *(uint32_t *)buf, end - srt);
}

#ifndef _VOL_YUP
#define _VOL_YUP  1 // =1: serependity, and the risk of chaos
#endif
#define    QUOTA 20 // the size of a GZIP header with no data

/* Thread worker performing magic search on incoming read data */
static void *thread_seeker(void *arg)
{
    seek_t *s = (seek_t *)arg;
    uint32_t len, fnd, pos = 0, srt = 0;

    while (*(volatile uint8_t *)&s->end == 0)
    {
        sem_wait(s->smp);
        // bugfix: s->end can be set, yet to find the last chunk
        // bugfix: s->cur can change in parallel, volatile yup!
#if _VOL_YUP
        len = *(volatile uint32_t *)&s->cur;
#else
        // well, well, well... here we are finally, right?
#endif
        if (len > pos + QUOTA) {
            len -= pos; // Search in [1, len-1] from buf + pos
            // s->buf never changes, it is set once and forever
            fnd = search_magic(s->buf + pos, len); // -n: bugfix
            if (fnd) {                            //
                pos += fnd;                      //
                do_stuff(s->buf, srt, pos); ////// -n: bugfix
                srt  = pos; // The new start
            } else {
                pos += len; // Not found yet
            }
        }
    }

    return NULL;
}

// arbitrary value, choose the fastest,
// here 32KiB is the GZIP max window
#define READ_SIZE (1UL << 15)

void xread_and_split(int fd, uint32_t large_size)
{
    seek_t s = {0};
    pthread_t tid;
    sem_t sem;
    uint32_t len;

    sem_init(&sem, 0, 0);

    // bugfix: +READ_SIZE avoids buffer overflow
    s.buf = malloc(large_size + READ_SIZE);
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

    for (uint32_t n = 0; n < large_size; ) {
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
