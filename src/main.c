#include "lab.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef TEST
#define main main_exclude
#endif

static void usage(void)
{
    puts("Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n"
         "                  [-c corrupt] [-d dup] [-p port] <relay> <file>\n"
         "       myapp recv -s <session> [-p port] <relay> <file>\n"
         "\n"
         "  -s <session>     session name shared by the sender and the receiver\n"
         "  -w <window>      Go-Back-N window size in packets, 1 to 64 (default: 8)\n"
         "  -T <timeout-ms>  retransmission timeout in milliseconds (default: 250)\n"
         "  -l <loss>        probability the relay drops a packet (default: 0)\n"
         "  -c <corrupt>     probability the relay flips a bit (default: 0)\n"
         "  -d <dup>         probability the relay duplicates a packet (default: 0)\n"
         "  -p <port>        relay port (default: 4250)\n"
         "  <relay>          host name or address of the relay\n"
         "  <file>           file to send, or file to write what is received");
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static int resolve_relay(const char *host, const char *port, struct sockaddr_storage *address,
                         socklen_t *address_length)
{
    struct addrinfo hints;
    struct addrinfo *result;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &result) != 0) return -1;
    memcpy(address, result->ai_addr, result->ai_addrlen);
    *address_length = (socklen_t)result->ai_addrlen;
    freeaddrinfo(result);
    return 0;
}

static int valid_session(const char *session)
{
    size_t i;
    for (i = 0; session[i] != '\0'; i++) {
        if (!((session[i] >= 'a' && session[i] <= 'z') ||
              (session[i] >= '0' && session[i] <= '9') || session[i] == '-')) return 0;
    }
    return i >= 1U && i <= 32U;
}

static int hello(int socket_fd, const struct sockaddr *relay, socklen_t relay_length,
                 const char *message)
{
    char reply[256];
    ssize_t received;
    struct pollfd descriptor = {socket_fd, POLLIN, 0};
    int attempt;
    for (attempt = 0; attempt < 5; attempt++) {
        if (sendto(socket_fd, message, strlen(message), 0, relay, relay_length) < 0) return -1;
        if (poll(&descriptor, 1, 1000) <= 0) continue;
        received = recvfrom(socket_fd, reply, sizeof reply - 1U, 0, NULL, NULL);
        if (received < 0) return -1;
        reply[received] = '\0';
        if (strncmp(reply, "OK", 2) == 0) return 0;
        fprintf(stderr, "%s\n", reply);
        return -2;
    }
    fprintf(stderr, "registration timed out\n");
    return -1;
}

static int send_packet(int fd, const struct sockaddr *relay, socklen_t relay_length,
                       const Packet *packet)
{
    uint8_t wire[PACKET_MAX_SIZE];
    size_t length;
    if (packet_encode(packet, wire, sizeof wire, &length) != 0) return -1;
    {
        ssize_t sent = sendto(fd, wire, length, 0, relay, relay_length);
        if (sent < 0 || (size_t)sent != length) {
            perror("sendto");
            return -1;
        }
    }
    return 0;
}

static int receive_packet(int fd, Packet *packet)
{
    uint8_t wire[PACKET_MAX_SIZE];
    ssize_t length = recvfrom(fd, wire, sizeof wire, 0, NULL, NULL);
    if (length < 0) return -1;
    return packet_decode(wire, (size_t)length, packet);
}

static int send_action(int fd, const struct sockaddr *relay, socklen_t relay_length,
                       const SenderAction *action)
{
    size_t i;
    for (i = 0; i < action->packet_count; i++) {
        if (send_packet(fd, relay, relay_length, &action->packets[i]) != 0) return -1;
    }
    return 0;
}

static int run_receiver(int fd, const struct sockaddr *relay, socklen_t relay_length,
                        const char *output_path)
{
    FILE *output = fopen(output_path, "wb");
    ReceiverState state;
    uint64_t last = monotonic_ms();
    if (output == NULL) return 2;
    receiver_init(&state);
    while (monotonic_ms() - last < 30000U) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        Packet packet;
        int ready = poll(&descriptor, 1, 1000);
        if (ready <= 0) continue;
        if (receive_packet(fd, &packet) != 0) continue;
        last = monotonic_ms();
        {
            ReceiverEvent event = receiver_packet(&state, &packet);
            if (!event.accepted) continue;
            if (event.deliver && fwrite(event.payload, 1, event.payload_length, output) != event.payload_length) {
                fclose(output);
                return 2;
            }
            if (send_packet(fd, relay, relay_length, &event.ack) != 0) {
                fclose(output);
                return 2;
            }
            if (event.finished) {
                uint64_t linger_end = monotonic_ms() + 2000U;
                (void)fclose(output);
                while (monotonic_ms() < linger_end) {
                    struct pollfd linger = {fd, POLLIN, 0};
                    if (poll(&linger, 1, 100) > 0 && receive_packet(fd, &packet) == 0) {
                        ReceiverEvent repeated = receiver_packet(&state, &packet);
                        if (repeated.accepted &&
                            send_packet(fd, relay, relay_length, &repeated.ack) != 0) {
                            return 2;
                        }
                    }
                }
                return 0;
            }
        }
    }
    (void)fclose(output);
    return 2;
}

static int run_sender(int fd, const struct sockaddr *relay, socklen_t relay_length,
                      const char *input_path, uint32_t window, uint64_t timeout_ms)
{
    FILE *input = fopen(input_path, "rb");
    long file_size;
    uint8_t *data;
    SenderState state;
    if (input == NULL || fseek(input, 0, SEEK_END) != 0) {
        fprintf(stderr, "cannot open input file %s\n", input_path);
        if (input != NULL) fclose(input);
        return 2;
    }
    file_size = ftell(input);
    if (file_size < 0 || fseek(input, 0, SEEK_SET) != 0) { fclose(input); return 2; }
    data = malloc((size_t)file_size == 0U ? 1U : (size_t)file_size);
    if (data == NULL || ((size_t)file_size != 0U && fread(data, 1, (size_t)file_size, input) != (size_t)file_size)) {
        free(data); fclose(input); return 2;
    }
    fclose(input);
    if (sender_init(&state, data, (size_t)file_size, window, monotonic_ms(), timeout_ms) != 0) {
        free(data);
        return 2;
    }
    free(data);
    if (!sender_ready(&state)) return 2;
    while (!sender_done(&state)) {
        uint64_t now = monotonic_ms();
        Packet packet;
        SenderAction action = sender_send_ready(&state, now, timeout_ms);
        if (send_action(fd, relay, relay_length, &action) != 0) {
            sender_destroy(&state);
            return 2;
        }
        {
            int wait_ms = action.timer_running
                ? (action.deadline_ms > now ? (int)(action.deadline_ms - now) : 0)
                : 1000;
            struct pollfd descriptor = {fd, POLLIN, 0};
            int ready = poll(&descriptor, 1, wait_ms);
            if (ready > 0) {
                if (receive_packet(fd, &packet) == 0 && packet.type == 1U &&
                    packet.length == 0U) {
                    action = sender_acknowledged(&state, packet.seq, monotonic_ms(),
                                                 timeout_ms);
                    if (action.failed) {
                        sender_destroy(&state);
                        return 2;
                    }
                }
            } else if (ready < 0 && errno != EINTR) {
                sender_destroy(&state);
                return 2;
            }
            if (action.timer_running && monotonic_ms() >= action.deadline_ms) {
                action = sender_timer_expired(&state, monotonic_ms(), timeout_ms);
                if (action.failed) {
                    fprintf(stderr, "giving up after 10 timeouts\n");
                    sender_destroy(&state);
                    return 2;
                }
                if (send_action(fd, relay, relay_length, &action) != 0) {
                    sender_destroy(&state);
                    return 2;
                }
            }
        }
    }
    sender_destroy(&state);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode;
    const char *session = NULL;
    const char *relay_host;
    const char *file_path;
    char port_text[16];
    int port = 4250;
    int window = 8;
    int timeout = 250;
    double loss = 0.0;
    double corrupt = 0.0;
    double dup = 0.0;
    int option;
    int fd;
    struct sockaddr_storage relay;
    socklen_t relay_length;
    if (argc == 1) { usage(); return 0; }
    if (argc < 2 || (strcmp(argv[1], "send") != 0 && strcmp(argv[1], "recv") != 0)) { usage(); return 1; }
    mode = argv[1];
    optind = 2;
    while ((option = getopt(argc, argv, "s:w:T:l:c:d:p:")) != -1) {
        char *end;
        switch (option) {
        case 's': session = optarg; break;
        case 'w': window = (int)strtol(optarg, &end, 10); if (*end != '\0') return 1; break;
        case 'T': timeout = (int)strtol(optarg, &end, 10); if (*end != '\0') return 1; break;
        case 'l': loss = strtod(optarg, &end); if (*end != '\0') return 1; break;
        case 'c': corrupt = strtod(optarg, &end); if (*end != '\0') return 1; break;
        case 'd': dup = strtod(optarg, &end); if (*end != '\0') return 1; break;
        case 'p': port = (int)strtol(optarg, &end, 10); if (*end != '\0') return 1; break;
        default: usage(); return 1;
        }
    }
    if (session == NULL || !valid_session(session) || optind + 2 != argc ||
        port < 1 || port > 65535 || timeout <= 0 || window < 1 || window > 64 ||
        loss < 0.0 || loss > 0.5 || corrupt < 0.0 || corrupt > 0.5 || dup < 0.0 || dup > 0.5) return 1;
    relay_host = argv[optind];
    file_path = argv[optind + 1];
    (void)snprintf(port_text, sizeof port_text, "%d", port);
    if (resolve_relay(relay_host, port_text, &relay, &relay_length) != 0) return 2;
    fd = socket(relay.ss_family, SOCK_DGRAM, 0);
    if (fd < 0) return 2;
    {
        char message[256];
        int hello_result;
        if (strcmp(mode, "recv") == 0) {
            (void)snprintf(message, sizeof message, "HELLO %s recv", session);
        } else {
            (void)snprintf(message, sizeof message, "HELLO %s send %.17g %.17g %.17g",
                           session, loss, corrupt, dup);
        }
        hello_result = hello(fd, (const struct sockaddr *)&relay, relay_length, message);
        if (hello_result != 0) { close(fd); return 2; }
    }
    {
        int result = strcmp(mode, "recv") == 0
            ? run_receiver(fd, (const struct sockaddr *)&relay, relay_length, file_path)
            : run_sender(fd, (const struct sockaddr *)&relay, relay_length, file_path,
                         (uint32_t)window, (uint64_t)timeout);
        close(fd);
        return result;
    }
}
