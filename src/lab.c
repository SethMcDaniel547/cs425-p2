#include "lab.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t packet_checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    size_t i;
    for (i = 0; i + 1U < length; i += 2U) {
        sum += ((uint32_t)data[i] << 8U) | data[i + 1U];
        sum = (sum & 0xffffU) + (sum >> 16U);
    }
    if (i < length) {
        sum += (uint32_t)data[i] << 8U;
        sum = (sum & 0xffffU) + (sum >> 16U);
    }
    return (uint16_t)(~sum);
}

int packet_encode(const Packet *packet, uint8_t *wire, size_t capacity,
                  size_t *wire_length)
{
    size_t total;
    uint16_t checksum;
    if (packet == NULL || wire == NULL || wire_length == NULL ||
        packet->type > 2U || packet->reserved != 0U ||
        packet->length > PACKET_PAYLOAD_MAX ||
        ((packet->type == 1U || packet->type == 2U) && packet->length != 0U)) {
        return -1;
    }
    total = PACKET_HEADER_SIZE + packet->length;
    if (capacity < total) return -1;
    wire[0] = packet->type;
    wire[1] = 0;
    wire[2] = 0;
    wire[3] = 0;
    {
        uint32_t nseq = htonl(packet->seq);
        uint16_t nlen = htons(packet->length);
        memcpy(wire + 4, &nseq, sizeof nseq);
        memcpy(wire + 8, &nlen, sizeof nlen);
    }
    if (packet->length != 0U) memcpy(wire + PACKET_HEADER_SIZE, packet->payload, packet->length);
    checksum = packet_checksum(wire, total);
    {
        uint16_t nchecksum = htons(checksum);
        memcpy(wire + 2, &nchecksum, sizeof nchecksum);
    }
    *wire_length = total;
    return 0;
}

int packet_decode(const uint8_t *wire, size_t wire_length, Packet *packet)
{
    uint16_t received;
    uint16_t length;
    uint32_t seq;
    uint8_t copy[PACKET_MAX_SIZE];
    if (wire == NULL || packet == NULL || wire_length < PACKET_HEADER_SIZE ||
        wire_length > PACKET_MAX_SIZE) return -1;
    memcpy(&length, wire + 8, sizeof length);
    length = ntohs(length);
    if (length > PACKET_PAYLOAD_MAX || PACKET_HEADER_SIZE + length != wire_length ||
        wire[0] > 2U || wire[1] != 0U ||
        ((wire[0] == 1U || wire[0] == 2U) && length != 0U)) return -1;
    memcpy(copy, wire, wire_length);
    memcpy(&received, copy + 2, sizeof received);
    received = ntohs(received);
    copy[2] = 0;
    copy[3] = 0;
    if (packet_checksum(copy, wire_length) != received) return -1;
    packet->type = wire[0];
    packet->reserved = wire[1];
    packet->checksum = received;
    memcpy(&seq, wire + 4, sizeof seq);
    packet->seq = ntohl(seq);
    packet->length = length;
    if (length != 0U) memcpy(packet->payload, wire + PACKET_HEADER_SIZE, length);
    return 0;
}

static Packet packet_make(uint8_t type, uint32_t seq)
{
    Packet packet;
    memset(&packet, 0, sizeof packet);
    packet.type = type;
    packet.seq = seq;
    return packet;
}

Packet packet_make_data(uint32_t seq, const uint8_t *data, size_t length)
{
    Packet packet = packet_make(0, seq);
    if (length > PACKET_PAYLOAD_MAX) length = PACKET_PAYLOAD_MAX;
    packet.length = (uint16_t)length;
    if (length != 0U && data != NULL) memcpy(packet.payload, data, length);
    return packet;
}

Packet packet_make_ack(uint32_t seq) { return packet_make(1, seq); }
Packet packet_make_fin(uint32_t seq) { return packet_make(2, seq); }

void receiver_init(ReceiverState *state)
{
    if (state != NULL) {
        state->expected = 0;
        state->finished = 0;
    }
}

ReceiverEvent receiver_packet(ReceiverState *state, const Packet *packet)
{
    ReceiverEvent event;
    memset(&event, 0, sizeof event);
    if (state == NULL || packet == NULL) return event;
    event.accepted = 1;
    event.ack = packet_make_ack(state->expected);
    if (packet->type == 0U && !state->finished && packet->seq == state->expected) {
        state->expected++;
        event.deliver = 1;
        event.payload = packet->payload;
        event.payload_length = packet->length;
        event.ack = packet_make_ack(state->expected);
    } else if (packet->type == 2U && !state->finished && packet->seq == state->expected &&
               packet->length == 0U) {
        state->expected++;
        state->finished = 1;
        event.finished = 1;
        event.ack = packet_make_ack(state->expected);
    } else if (packet->type != 0U && packet->type != 2U) {
        event.accepted = 0;
    }
    return event;
}

int sender_init(SenderState *state, const uint8_t *data, size_t length,
                uint32_t window, uint64_t now_ms, uint64_t timeout_ms)
{
    size_t count = (length + PACKET_PAYLOAD_MAX - 1U) / PACKET_PAYLOAD_MAX;
    size_t i;
    if (state == NULL) return -1;
    memset(state, 0, sizeof *state);
    state->packet_count = count;
    state->window = window < 1U ? 1U : (window > SENDER_MAX_WINDOW ? SENDER_MAX_WINDOW : window);
    state->packets = calloc(count + 1U, sizeof *state->packets);
    if (state->packets == NULL) return -1;
    for (i = 0; i < count; i++) {
        size_t offset = i * PACKET_PAYLOAD_MAX;
        size_t remaining = length - offset;
        state->packets[i] = packet_make_data((uint32_t)i, data + offset,
                                              remaining > PACKET_PAYLOAD_MAX ? PACKET_PAYLOAD_MAX : remaining);
    }
    state->packets[count] = packet_make_fin((uint32_t)count);
    state->deadline_ms = now_ms + timeout_ms;
    return 0;
}

void sender_destroy(SenderState *state)
{
    if (state != NULL) {
        free(state->packets);
        state->packets = NULL;
    }
}

int sender_next_packet(SenderState *state, Packet *packet, uint64_t now_ms,
                       uint64_t timeout_ms)
{
    uint32_t total;
    if (state == NULL || packet == NULL || state->packets == NULL ||
        state->complete) return 0;
    total = (uint32_t)state->packet_count + 1U;
    if (state->next >= total || state->next >= state->base + state->window) return 0;
    *packet = state->packets[state->next++];
    if (!state->timer_running) {
        state->timer_running = 1;
        state->deadline_ms = now_ms + timeout_ms;
    }
    return 1;
}

int sender_ack(SenderState *state, uint32_t ack_seq, uint64_t now_ms,
               uint64_t timeout_ms)
{
    uint32_t total;
    if (state == NULL) return 0;
    total = (uint32_t)state->packet_count + 1U;
    if (state == NULL || ack_seq <= state->base || ack_seq > total || ack_seq > state->next) return 0;
    state->base = ack_seq;
    state->timeout_count = 0;
    if (state->base == total) {
        state->complete = 1;
        state->timer_running = 0;
    } else {
        state->timer_running = state->base < state->next;
        if (state->timer_running) state->deadline_ms = now_ms + timeout_ms;
    }
    return 1;
}

int sender_timeout(SenderState *state, uint64_t now_ms, uint64_t timeout_ms)
{
    if (state == NULL || !state->timer_running || now_ms < state->deadline_ms) return 0;
    state->timeout_count++;
    if (state->timeout_count >= 10U) return -1;
    state->deadline_ms = now_ms + timeout_ms;
    return 1;
}

int sender_done(const SenderState *state) { return state != NULL && state->complete; }
int sender_ready(const SenderState *state)
{
    return state != NULL && state->packets != NULL;
}

static SenderAction sender_action(const SenderState *state)
{
    SenderAction action;
    memset(&action, 0, sizeof action);
    if (state != NULL) {
        action.deadline_ms = state->deadline_ms;
        action.timer_running = state->timer_running;
        action.complete = state->complete;
    }
    return action;
}

SenderAction sender_send_ready(SenderState *state, uint64_t now_ms,
                               uint64_t timeout_ms)
{
    SenderAction action = sender_action(state);
    Packet packet;
    if (state == NULL) {
        action.failed = 1;
        return action;
    }
    while (action.packet_count < SENDER_MAX_WINDOW &&
           sender_next_packet(state, &packet, now_ms, timeout_ms) != 0) {
        action.packets[action.packet_count++] = packet;
    }
    action.deadline_ms = state->deadline_ms;
    action.timer_running = state->timer_running;
    action.complete = state->complete;
    return action;
}

SenderAction sender_acknowledged(SenderState *state, uint32_t ack_seq,
                                 uint64_t now_ms, uint64_t timeout_ms)
{
    SenderAction action;
    if (state == NULL || sender_ack(state, ack_seq, now_ms, timeout_ms) == 0) {
        action = sender_action(state);
        return action;
    }
    return sender_send_ready(state, now_ms, timeout_ms);
}

SenderAction sender_timer_expired(SenderState *state, uint64_t now_ms,
                                  uint64_t timeout_ms)
{
    SenderAction action = sender_action(state);
    uint32_t sequence;
    if (state == NULL) {
        action.failed = 1;
        return action;
    }
    if (sender_timeout(state, now_ms, timeout_ms) < 0) {
        action.failed = 1;
        return action;
    }
    if (state->timer_running) {
        for (sequence = state->base;
             sequence < state->next && action.packet_count < SENDER_MAX_WINDOW;
             sequence++) {
            action.packets[action.packet_count++] = state->packets[sequence];
        }
    }
    action.deadline_ms = state->deadline_ms;
    action.timer_running = state->timer_running;
    action.complete = state->complete;
    return action;
}

char *get_greeting(const char *restrict name)
{
    int length;
    char *greeting;
    if (name == NULL) return NULL;
    length = snprintf(NULL, 0, "Hello, %s!", name);
    if (length < 0) return NULL;
    greeting = malloc((size_t)length + 1U);
    if (greeting == NULL) return NULL;
    (void)snprintf(greeting, (size_t)length + 1U, "Hello, %s!", name);
    return greeting;
}
