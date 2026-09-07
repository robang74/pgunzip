/*
 * (c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, GPL v2
 *
 * I wrote a canvas of the desiderated function and I asked Gemini to complete
 * the C-language code. The result below has been further edited by me. This
 * is a way to write fast code, let the AI doing the boring work and keeping
 * the control about the architecture. Simple as example, but a valid example.
 *
 * https://share.gemini.google/3lZU2K71iWhd
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
extern void do_stuff(uint8_t *buf, size_t len);

typedef struct {
    uint8_t *buf;  /* Pointer to the shared buffer */
    size_t sze;    /* Total size capacity of buf */
    size_t cur;    /* Current valid readable/searched offset updated by master */
    bool end;      /* Termination flag set by master on EOF or read complete */
    sem_t *smp;    /* Pointer to semaphore signaling new data available */
} seek_t;

seek_t s = {0};

/* Thread worker performing magic search on incoming read data */
void *seeker(void *arg)
{
    seek_t *s = (seek_t *)arg;
    size_t n = 0;
    size_t f;

    while (1) {
        if (s->end) break;
        sem_wait(s->smp);
        if (s->end) break;

        /* Search in range [n+1, s->cur-1]  */
        if (s->cur > n + 1) {
            f = search_magic(&s->buf[n], n, s->cur); 
            if (f) {
                do_stuff(&s->buf[n], f - n);
                n += f;
            } else {
                n = s->cur; // Not found yet
            }
        }
    }

    return NULL;
}

#define READ_SIZE 4096

void xread_and_split(void)
{
    pthread_t tid;
    sem_t sem;
    size_t large_size = 1 << 20; // Example buffer size limit
    int fd = 0;                  // Target file descriptor
    size_t len;

    sem_init(&sem, 0, 0);

    s.buf = malloc(large_size);
    s.sze = large_size;
    s.smp = &sem;
    s.cur = 0;
    s.end = false;

    /* Spawn background seeker thread */
    if (pthread_create(&tid, NULL, seeker, &s) != 0) {
        perror("pthread_create");
        exit(-1);
    }

    for (size_t n = 0; n < large_size; ) {
        len = xfull_read(fd, &s.buf[n], READ_SIZE);

        if (!len) {
            s.end = true;
            sem_post(s.smp);
            break;
        }

        n += len;
        s.cur = n;
        sem_post(s.smp);
    }

    pthread_join(tid, NULL);

    sem_destroy(&sem);
}
