#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "harness/unity.h"
#include "../src/lab.h"


void setUp(void) {
  printf("Setting up tests...\n");
}

void tearDown(void) {
  printf("Tearing down tests...\n");
}

void test_get_greeting(void) {
  char *greeting = get_greeting("Alice");
  TEST_ASSERT_NOT_NULL(greeting);
  TEST_ASSERT_EQUAL_STRING("Hello, Alice!", greeting);
  free(greeting); // Free the allocated memory for the greeting

  greeting = get_greeting(NULL);
  TEST_ASSERT_NULL(greeting);

  greeting = get_greeting("");
  TEST_ASSERT_NOT_NULL(greeting);
  TEST_ASSERT_EQUAL_STRING("Hello, !", greeting);
  free(greeting);
}

void test_checksum_and_packet_validation(void) {
  uint8_t wire[PACKET_MAX_SIZE];
  size_t wire_length;
  Packet packet = packet_make_data(2, (const uint8_t *)"Hi!", 3);
  Packet decoded;
  TEST_ASSERT_EQUAL_INT(0, packet_encode(&packet, wire, sizeof wire, &wire_length));
  wire[2] = 0;
  wire[3] = 0;
  TEST_ASSERT_EQUAL_HEX16(0x9691, packet_checksum(wire, wire_length));
  TEST_ASSERT_EQUAL_INT(0, packet_encode(&packet, wire, sizeof wire, &wire_length));
  TEST_ASSERT_EQUAL_INT(0, packet_decode(wire, wire_length, &decoded));
  TEST_ASSERT_EQUAL_UINT32(2, decoded.seq);
  TEST_ASSERT_EQUAL_UINT16(3, decoded.length);
  wire[wire_length - 1U] ^= 1U;
  TEST_ASSERT_EQUAL_INT(-1, packet_decode(wire, wire_length, &decoded));
  TEST_ASSERT_EQUAL_INT(-1, packet_decode(wire, 9, &decoded));

  TEST_ASSERT_EQUAL_HEX16(0x220d, packet_checksum(
      (const uint8_t *)"\x00\x01\xf2\x03\xf4\xf5\xf6\xf7", 8));
  TEST_ASSERT_EQUAL_INT(-1, packet_decode((const uint8_t *)"\0\0\0\0\0\0\0\0\0", 9, &decoded));
  TEST_ASSERT_EQUAL_INT(-1, packet_decode(wire, PACKET_HEADER_SIZE, &decoded));
  wire[0] = 3;
  TEST_ASSERT_EQUAL_INT(-1, packet_decode(wire, wire_length, &decoded));
}

void test_packet_argument_and_format_errors(void) {
  uint8_t wire[PACKET_MAX_SIZE];
  size_t wire_length = 0;
  Packet packet = packet_make_data(1, (const uint8_t *)"x", 1);
  Packet invalid = packet_make_ack(1);

  TEST_ASSERT_EQUAL_INT(-1, packet_encode(NULL, wire, sizeof wire, &wire_length));
  TEST_ASSERT_EQUAL_INT(-1, packet_encode(&packet, NULL, sizeof wire, &wire_length));
  TEST_ASSERT_EQUAL_INT(-1, packet_encode(&packet, wire, sizeof wire, NULL));
  TEST_ASSERT_EQUAL_INT(-1, packet_encode(&packet, wire, PACKET_HEADER_SIZE, &wire_length));
  invalid.length = 1;
  TEST_ASSERT_EQUAL_INT(-1, packet_encode(&invalid, wire, sizeof wire, &wire_length));
  invalid = packet_make_data(1, NULL, 0);
  TEST_ASSERT_EQUAL_UINT16(0, invalid.length);
  invalid = packet_make_data(1, (const uint8_t *)"long", PACKET_PAYLOAD_MAX + 1U);
  TEST_ASSERT_EQUAL_UINT16(PACKET_PAYLOAD_MAX, invalid.length);
  TEST_ASSERT_EQUAL_INT(-1, packet_decode(NULL, PACKET_HEADER_SIZE, &invalid));
  TEST_ASSERT_EQUAL_INT(-1, packet_decode(wire, PACKET_HEADER_SIZE, NULL));
}

void test_receiver_orders_and_lingers_fin(void) {
  ReceiverState state;
  ReceiverEvent event;
  Packet first = packet_make_data(0, (const uint8_t *)"a", 1);
  Packet future = packet_make_data(2, (const uint8_t *)"c", 1);
  Packet fin = packet_make_fin(1);
  Packet invalid = packet_make_ack(0);
  receiver_init(&state);
  event = receiver_packet(&state, &first);
  TEST_ASSERT_TRUE(event.deliver);
  TEST_ASSERT_EQUAL_UINT32(1, event.ack.seq);
  event = receiver_packet(&state, &future);
  TEST_ASSERT_FALSE(event.deliver);
  TEST_ASSERT_EQUAL_UINT32(1, event.ack.seq);
  event = receiver_packet(&state, &fin);
  TEST_ASSERT_TRUE(event.finished);
  TEST_ASSERT_EQUAL_UINT32(2, event.ack.seq);
  event = receiver_packet(&state, &fin);
  TEST_ASSERT_FALSE(event.deliver);
  TEST_ASSERT_EQUAL_UINT32(2, event.ack.seq);
  event = receiver_packet(&state, &future);
  TEST_ASSERT_TRUE(event.accepted);
  event = receiver_packet(&state, &invalid);
  TEST_ASSERT_FALSE(event.accepted);
  receiver_init(NULL);
  event = receiver_packet(NULL, &fin);
  TEST_ASSERT_FALSE(event.accepted);
  event = receiver_packet(&state, NULL);
  TEST_ASSERT_FALSE(event.accepted);
}

void test_sender_window_ack_and_timeout(void) {
  SenderState state;
  Packet packet;
  uint8_t data[2048] = {0};
  sender_init(&state, data, sizeof data, 2, 0, 100);
  TEST_ASSERT_TRUE(sender_next_packet(&state, &packet, 0, 100));
  TEST_ASSERT_TRUE(sender_next_packet(&state, &packet, 0, 100));
  TEST_ASSERT_FALSE(sender_next_packet(&state, &packet, 0, 100));
  TEST_ASSERT_EQUAL_INT(1, sender_ack(&state, 2, 10, 100));
  TEST_ASSERT_TRUE(sender_next_packet(&state, &packet, 10, 100));
  TEST_ASSERT_EQUAL_INT(1, sender_timeout(&state, 111, 100));
  TEST_ASSERT_EQUAL_INT(0, sender_timeout(&state, 112, 100));
  TEST_ASSERT_EQUAL_INT(0, sender_ack(&state, 2, 120, 100));
  sender_destroy(&state);
}

void test_sender_stops_at_fin_and_gives_up(void) {
  SenderState state;
  Packet packet;
  sender_init(&state, NULL, 0, 64, 0, 10);
  TEST_ASSERT_TRUE(sender_next_packet(&state, &packet, 0, 10));
  TEST_ASSERT_EQUAL_UINT8(2, packet.type);
  TEST_ASSERT_FALSE(sender_next_packet(&state, &packet, 0, 10));
  TEST_ASSERT_EQUAL_INT(1, sender_timeout(&state, 10, 10));
  TEST_ASSERT_EQUAL_INT(1, sender_timeout(&state, 20, 10));
  TEST_ASSERT_EQUAL_INT(1, sender_timeout(&state, 30, 10));
  TEST_ASSERT_EQUAL_INT(1, sender_timeout(&state, 40, 10));
  TEST_ASSERT_EQUAL_INT(1, sender_timeout(&state, 50, 10));
  TEST_ASSERT_EQUAL_INT(1, sender_timeout(&state, 60, 10));
  TEST_ASSERT_EQUAL_INT(1, sender_timeout(&state, 70, 10));
  TEST_ASSERT_EQUAL_INT(1, sender_timeout(&state, 80, 10));
  TEST_ASSERT_EQUAL_INT(1, sender_timeout(&state, 90, 10));
  TEST_ASSERT_EQUAL_INT(-1, sender_timeout(&state, 100, 10));
  sender_destroy(&state);
}

void test_sender_actions_hide_retransmission_state(void) {
  SenderState state;
  SenderAction action;
  uint8_t data[2048] = {0};

  TEST_ASSERT_EQUAL_INT(0, sender_init(&state, data, sizeof data, 2, 0, 100));
  TEST_ASSERT_TRUE(sender_ready(&state));
  action = sender_send_ready(&state, 0, 100);
  TEST_ASSERT_EQUAL_UINT32(2, action.packet_count);
  TEST_ASSERT_TRUE(action.timer_running);
  TEST_ASSERT_EQUAL_UINT64(100, action.deadline_ms);
  action = sender_timer_expired(&state, 100, 100);
  TEST_ASSERT_EQUAL_UINT32(2, action.packet_count);
  TEST_ASSERT_EQUAL_UINT32(0, action.packets[0].seq);
  TEST_ASSERT_EQUAL_UINT32(1, action.packets[1].seq);
  action = sender_acknowledged(&state, 2, 110, 100);
  TEST_ASSERT_EQUAL_UINT32(1, action.packet_count);
  TEST_ASSERT_EQUAL_UINT32(2, action.packets[0].seq);
  sender_destroy(&state);
}

void test_sender_edge_cases_and_empty_file(void) {
  SenderState state;
  SenderState uninitialized;
  SenderAction action;
  Packet packet;
  uint8_t exact[PACKET_PAYLOAD_MAX] = {0};

  TEST_ASSERT_EQUAL_INT(-1, sender_init(NULL, NULL, 0, 1, 0, 10));
  TEST_ASSERT_FALSE(sender_ready(NULL));
  TEST_ASSERT_FALSE(sender_done(NULL));
  sender_destroy(NULL);
  memset(&uninitialized, 0, sizeof uninitialized);
  TEST_ASSERT_FALSE(sender_next_packet(&uninitialized, &packet, 0, 10));
  TEST_ASSERT_FALSE(sender_next_packet(NULL, &packet, 0, 10));
  TEST_ASSERT_FALSE(sender_next_packet(&uninitialized, NULL, 0, 10));
  TEST_ASSERT_FALSE(sender_ack(NULL, 1, 0, 10));
  TEST_ASSERT_FALSE(sender_ack(&uninitialized, 1, 0, 10));
  TEST_ASSERT_FALSE(sender_timeout(NULL, 0, 10));
  TEST_ASSERT_FALSE(sender_timeout(&uninitialized, 0, 10));
  action = sender_send_ready(NULL, 0, 10);
  TEST_ASSERT_TRUE(action.failed);
  action = sender_acknowledged(NULL, 1, 0, 10);
  TEST_ASSERT_FALSE(action.failed);
  action = sender_timer_expired(NULL, 0, 10);
  TEST_ASSERT_TRUE(action.failed);

  TEST_ASSERT_EQUAL_INT(0, sender_init(&state, NULL, 0, 0, 50, 10));
  TEST_ASSERT_TRUE(sender_ready(&state));
  TEST_ASSERT_TRUE(sender_next_packet(&state, &packet, 50, 10));
  TEST_ASSERT_EQUAL_UINT8(2, packet.type);
  TEST_ASSERT_EQUAL_INT(1, sender_ack(&state, 1, 55, 10));
  TEST_ASSERT_TRUE(sender_done(&state));
  TEST_ASSERT_FALSE(sender_timeout(&state, 100, 10));
  sender_destroy(&state);

  TEST_ASSERT_EQUAL_INT(0, sender_init(&state, exact, sizeof exact, 4, 0, 10));
  TEST_ASSERT_TRUE(sender_next_packet(&state, &packet, 0, 10));
  TEST_ASSERT_EQUAL_UINT8(0, packet.type);
  TEST_ASSERT_EQUAL_UINT16(PACKET_PAYLOAD_MAX, packet.length);
  TEST_ASSERT_TRUE(sender_ack(&state, 1, 1, 10));
  TEST_ASSERT_TRUE(sender_next_packet(&state, &packet, 1, 10));
  TEST_ASSERT_EQUAL_UINT8(2, packet.type);
  sender_destroy(&state);
}

void test_sender_ack_and_timer_edge_cases(void) {
  SenderState state;
  SenderAction action;
  uint8_t data[1] = {7};
  TEST_ASSERT_EQUAL_INT(0, sender_init(&state, data, sizeof data, 1, 0, 10));
  TEST_ASSERT_EQUAL_INT(0, sender_ack(&state, 0, 0, 10));
  TEST_ASSERT_EQUAL_INT(0, sender_ack(&state, 2, 0, 10));
  TEST_ASSERT_EQUAL_INT(0, sender_timeout(&state, 9, 10));
  sender_destroy(&state);

  TEST_ASSERT_EQUAL_INT(0, sender_init(&state, data, sizeof data, 2, 0, 10));
  TEST_ASSERT_TRUE(sender_next_packet(&state, &(Packet){0}, 0, 10));
  TEST_ASSERT_TRUE(sender_next_packet(&state, &(Packet){0}, 0, 10));
  TEST_ASSERT_EQUAL_INT(1, sender_ack(&state, 1, 2, 10));
  TEST_ASSERT_TRUE(state.timer_running);
  sender_destroy(&state);

  TEST_ASSERT_EQUAL_INT(0, sender_init(&state, NULL, 0, 1, 0, 10));
  action = sender_send_ready(&state, 0, 10);
  TEST_ASSERT_EQUAL_UINT32(1, action.packet_count);
  for (unsigned attempt = 1; attempt < 10U; attempt++) {
    action = sender_timer_expired(&state, (uint64_t)attempt * 10U, 10);
    TEST_ASSERT_FALSE(action.failed);
  }
  action = sender_timer_expired(&state, 100, 10);
  TEST_ASSERT_TRUE(action.failed);
  sender_destroy(&state);
}

static unsigned test_random(unsigned *seed) {
  *seed = *seed * 1664525U + 1013904223U;
  return *seed;
}

static void queue_packet(Packet *queue, size_t *count, const Packet *packet,
                         unsigned *seed) {
  uint8_t wire[PACKET_MAX_SIZE];
  size_t wire_length;
  Packet decoded;
  unsigned chance = test_random(seed) % 100U;
  if (chance < 20U) return;
  TEST_ASSERT_EQUAL_INT(0, packet_encode(packet, wire, sizeof wire, &wire_length));
  if (chance < 40U) {
    wire[4] ^= 1U;
  }
  if (packet_decode(wire, wire_length, &decoded) != 0) return;
  TEST_ASSERT_TRUE(*count < 4096U);
  queue[(*count)++] = decoded;
  if (test_random(seed) % 100U >= 80U) {
    TEST_ASSERT_TRUE(*count < 4096U);
    queue[(*count)++] = decoded;
  }
}

static void queue_action(Packet *queue, size_t *count, const SenderAction *action,
                         unsigned *seed) {
  size_t i;
  for (i = 0; i < action->packet_count; i++) {
    queue_packet(queue, count, &action->packets[i], seed);
  }
}

void test_in_memory_transfer_with_loss_corruption_and_duplicates(void) {
  uint8_t input[2500];
  uint8_t output[2500];
  static Packet data_queue[4096];
  static Packet ack_queue[4096];
  unsigned seeds[] = {7U, 19U, 41U};
  size_t seed_index;

  for (size_t i = 0; i < sizeof input; i++) input[i] = (uint8_t)(i * 37U);
  for (seed_index = 0; seed_index < sizeof seeds / sizeof seeds[0]; seed_index++) {
    SenderState sender;
    ReceiverState receiver;
    SenderAction action;
    unsigned seed = seeds[seed_index];
    size_t data_count = 0;
    size_t ack_count = 0;
    size_t delivered = 0;
    uint64_t now = 0;
    unsigned steps;

    TEST_ASSERT_EQUAL_INT(0, sender_init(&sender, input, sizeof input, 4, now, 5));
    receiver_init(&receiver);
    action = sender_send_ready(&sender, now, 5);
    queue_action(data_queue, &data_count, &action, &seed);

    for (steps = 0; steps < 10000U && !sender_done(&sender); steps++) {
      if (data_count != 0U) {
        Packet packet = data_queue[--data_count];
        ReceiverEvent event = receiver_packet(&receiver, &packet);
        if (event.deliver) {
          TEST_ASSERT_TRUE(delivered + event.payload_length <= sizeof output);
          memcpy(output + delivered, event.payload, event.payload_length);
          delivered += event.payload_length;
        }
        if (event.accepted) {
          queue_packet(ack_queue, &ack_count, &event.ack, &seed);
        }
      } else if (ack_count != 0U) {
        Packet ack = ack_queue[--ack_count];
        action = sender_acknowledged(&sender, ack.seq, now, 5);
        TEST_ASSERT_FALSE(action.failed);
        queue_action(data_queue, &data_count, &action, &seed);
      } else {
        now += 5;
        action = sender_timer_expired(&sender, now, 5);
        TEST_ASSERT_FALSE(action.failed);
        queue_action(data_queue, &data_count, &action, &seed);
      }
    }
    TEST_ASSERT_TRUE(sender_done(&sender));
    TEST_ASSERT_EQUAL_UINT32(sizeof input, delivered);
    TEST_ASSERT_EQUAL_MEMORY(input, output, sizeof input);
    sender_destroy(&sender);
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_get_greeting);
  RUN_TEST(test_checksum_and_packet_validation);
  RUN_TEST(test_packet_argument_and_format_errors);
  RUN_TEST(test_receiver_orders_and_lingers_fin);
  RUN_TEST(test_sender_window_ack_and_timeout);
  RUN_TEST(test_sender_stops_at_fin_and_gives_up);
  RUN_TEST(test_sender_actions_hide_retransmission_state);
  RUN_TEST(test_sender_edge_cases_and_empty_file);
  RUN_TEST(test_sender_ack_and_timer_edge_cases);
  RUN_TEST(test_in_memory_transfer_with_loss_corruption_and_duplicates);
  return UNITY_END();
}
