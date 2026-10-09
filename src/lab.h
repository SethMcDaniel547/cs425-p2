#ifndef LAB_H
#define LAB_H

#include <stddef.h>
#include <stdint.h>

#define PACKET_PAYLOAD_MAX 1024U
#define PACKET_HEADER_SIZE 10U
#define PACKET_MAX_SIZE (PACKET_HEADER_SIZE + PACKET_PAYLOAD_MAX)
#define SENDER_MAX_WINDOW 64U

typedef struct {
    uint8_t type;
    uint8_t reserved;
    uint16_t checksum;
    uint32_t seq;
    uint16_t length;
    uint8_t payload[PACKET_PAYLOAD_MAX];
} Packet;

uint16_t packet_checksum(const uint8_t *data, size_t length);
int packet_encode(const Packet *packet, uint8_t *wire, size_t capacity,
                  size_t *wire_length);
int packet_decode(const uint8_t *wire, size_t wire_length, Packet *packet);
Packet packet_make_data(uint32_t seq, const uint8_t *data, size_t length);
Packet packet_make_ack(uint32_t seq);
Packet packet_make_fin(uint32_t seq);

typedef struct {
    uint32_t expected;
    int finished;
} ReceiverState;

typedef struct {
    int accepted;
    int deliver;
    int finished;
    Packet ack;
    const uint8_t *payload;
    size_t payload_length;
} ReceiverEvent;

void receiver_init(ReceiverState *state);
ReceiverEvent receiver_packet(ReceiverState *state, const Packet *packet);

typedef struct {
    Packet *packets;
    size_t packet_count;
    uint32_t base;
    uint32_t next;
    uint32_t window;
    uint32_t timeout_count;
    uint64_t deadline_ms;
    int timer_running;
    int fin_sent;
    int complete;
} SenderState;

typedef struct {
    Packet packets[SENDER_MAX_WINDOW];
    size_t packet_count;
    uint64_t deadline_ms;
    int timer_running;
    int complete;
    int failed;
} SenderAction;

int sender_init(SenderState *state, const uint8_t *data, size_t length,
                uint32_t window, uint64_t now_ms, uint64_t timeout_ms);
void sender_destroy(SenderState *state);
int sender_next_packet(SenderState *state, Packet *packet, uint64_t now_ms,
                       uint64_t timeout_ms);
int sender_ack(SenderState *state, uint32_t ack_seq, uint64_t now_ms,
               uint64_t timeout_ms);
int sender_timeout(SenderState *state, uint64_t now_ms, uint64_t timeout_ms);
int sender_done(const SenderState *state);
int sender_ready(const SenderState *state);
SenderAction sender_send_ready(SenderState *state, uint64_t now_ms,
                               uint64_t timeout_ms);
SenderAction sender_acknowledged(SenderState *state, uint32_t ack_seq,
                                 uint64_t now_ms, uint64_t timeout_ms);
SenderAction sender_timer_expired(SenderState *state, uint64_t now_ms,
                                  uint64_t timeout_ms);

char *get_greeting(const char *restrict name);

#endif
