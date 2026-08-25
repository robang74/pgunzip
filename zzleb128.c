/*
 * (c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, GPL v2
 *  Just-in-time pushable, delta + varint 32-bit, de/compression example
 *  Algorithm family and class: ZigZag LEB128, O(1) 
 */

#include <stdint.h>
#include <stddef.h>

// 1. Mappa int32_t su uint32_t per gestire i delta negativi in 1-2 byte
static inline uint32_t zigzag_encode32(int32_t n) {
    return (uint32_t)((n << 1) ^ (n >> 31));
}

static inline int32_t zigzag_decode32(uint32_t n) {
    return (int32_t)((n >> 1) ^ (-(int32_t)(n & 1)));
}

// 2. Scrive un uint32_t in formato LEB128 (1..5 byte)
static inline size_t varint_encode32(uint8_t *out, uint32_t val) {
    size_t bytes = 0;
    while (val >= 0x80) {
        out[bytes++] = (uint8_t)((val & 0x7F) | 0x80);
        val >>= 7;
    }
    out[bytes++] = (uint8_t)(val & 0x7F);
    return bytes; // Ritorna i byte scritti
}

// 3. Legge un uint32_t da formato LEB128
static inline size_t varint_decode32(const uint8_t *in, uint32_t *val) {
    uint32_t result = 0;
    int shift = 0;
    size_t bytes = 0;
    uint8_t byte;
    do {
        byte = in[bytes++];
        result |= (uint32_t)(byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);
    *val = result;
    return bytes; // Ritorna i byte letti
}

typedef struct {
    uint8_t *buf;
    size_t max_size;
    size_t current_offset;
    uint32_t prev_value;
    uint32_t record_count;
} ptgz_delta_writer_t;

// Calcola quanti byte occuperà il prossimo valore SENZA scriverlo
static inline size_t ptgz_predict_record_size(ptgz_delta_writer_t *w, uint32_t value) {
    int32_t delta = (int32_t)value - (int32_t)w->prev_value;
    uint32_t zz = zigzag_encode32(delta);
    
    // Conteggio rapido dei byte occupati dal varint
    if (zz < (1U << 7))  return 1;
    if (zz < (1U << 14)) return 2;
    if (zz < (1U << 21)) return 3;
    if (zz < (1U << 28)) return 4;
    return 5;
}

// Inserisce il record nel buffer se c'è spazio sufficiente
int ptgz_push_record(ptgz_delta_writer_t *w, uint32_t value) {
    size_t needed_bytes = ptgz_predict_record_size(w, value);
    
    // Verifico che il record entri nel limite (es. 65504 byte per l'header PTGZ)
    if (w->current_offset + needed_bytes > w->max_size) {
        return 0; // Buffer pieno, sfora il blocco
    }

    int32_t delta = (int32_t)value - (int32_t)w->prev_value;
    uint32_t zz = zigzag_encode32(delta);
    
    // Scrittura effettiva
    w->current_offset += varint_encode32(w->buf + w->current_offset, zz);
    w->prev_value = value;
    w->record_count++;
    
    return 1; // Record aggiunto con successo
}

void ptgz_read_records(const uint8_t *buf, size_t buf_len, uint32_t *out_records, size_t count) {
    size_t offset = 0;
    uint32_t last_val = 0;

    for (size_t i = 0; i < count && offset < buf_len; i++) {
        uint32_t zz;
        offset += varint_decode32(buf + offset, &zz);
        
        int32_t delta = zigzag_decode32(zz);
        uint32_t current_val = last_val + delta;
        
        out_records[i] = current_val;
        last_val = current_val;
    }
}
