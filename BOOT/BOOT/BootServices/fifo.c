// fifo.c
#include "fifo.h"

void fifo_init(fifo_t *f) {
    f->head = f->tail = 0;
}

bool fifo_put(fifo_t *f, uint8_t byte) {
    uint16_t next = (f->head + 1) % FIFO_SIZE;
    if (next == f->tail) {
        return false;  // 满
    }
    f->buffer[f->head] = byte;
    f->head = next;
    return true;
}

bool fifo_get(fifo_t *f, uint8_t *byte) {
    if (f->head == f->tail) {
        return false;  // 空
    }
    *byte = f->buffer[f->tail];
    f->tail = (f->tail + 1) % FIFO_SIZE;
    return true;
}

uint16_t fifo_available(fifo_t *f) {
    return (f->head - f->tail + FIFO_SIZE) % FIFO_SIZE;
}

void fifo_flush(fifo_t *f) {
    f->tail = f->head;
}

