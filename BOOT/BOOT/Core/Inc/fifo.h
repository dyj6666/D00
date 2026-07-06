// fifo.h
#ifndef FIFO_H
#define FIFO_H

#include <stdint.h>
#include <stdbool.h>

#define FIFO_SIZE 2048

typedef struct {
    uint8_t  buffer[FIFO_SIZE];
    uint16_t head;
    uint16_t tail;
} fifo_t;

void     fifo_init(fifo_t *f);
bool     fifo_put(fifo_t *f, uint8_t byte);
bool     fifo_get(fifo_t *f, uint8_t *byte);
uint16_t fifo_available(fifo_t *f);
void     fifo_flush(fifo_t *f);

#endif

