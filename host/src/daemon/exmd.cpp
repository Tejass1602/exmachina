#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstdarg>

#include "../shared/exm_packet.h"
#include "daemon/hmac_sha256.h"
#include "daemon/sandbox.h"

/* ─── Configuration ─── */
struct ExmdConfig {
    const char* serial_port = "/dev/ttyACM0";
    int baud_rate = 921600;
    const char* llm_url = "http://127.0.0.1:8080/completion";
    bool mock_serial = false;
    const char* hmac_key = nullptr;
    size_t hmac_key_len = 0;
};

/* ─── Global State ─── */
static ExmdConfig g_config;
static std::atomic<bool> g_running{true};
static int g_serial_fd = -1;
static int g_epoll_fd = -1;
static int g_timer_fd = -1;
static uint32_t g_last_valid_seq = 0;
static uint8_t g_hmac_key[32];
static bool g_hmac_key_set = false;

/* ─── Signal Handler ─── */
static void signal_handler(int sig) {
    g_running = false;
}

/* ─── Serial Port ─── */
static int open_serial(const char* port, int baud) {
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1;
    }
    
    cfmakeraw(&tty);
    cfsetispeed(&tty, baud);
    cfsetospeed(&tty, baud);
    
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag |= CREAD | CLOCAL;
    
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return -1;
    }
    
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void close_serial(int fd) {
    if (fd >= 0) close(fd);
}

static ssize_t read_exact(int fd, void* buf, size_t len) {
    uint8_t* p = (uint8_t*)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n <= 0) return n;
        total += n;
    }
    return total;
}

static ssize_t write_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n <= 0) return n;
        total += n;
    }
    return total;
}

/* ─── Packet I/O ─── */
static bool send_packet(int fd, uint8_t type, uint8_t flags, const void* payload, size_t payload_len) {
    if (payload_len > 512) return false;
    
    ExmHeader hdr;
    hdr.magic = 0x4558;
    hdr.type = type;
    hdr.flags = flags;
    hdr.payload_len = (uint16_t)payload_len;
    hdr.crc16 = 0;
    
    uint8_t crc_buf[6 + 512];
    std::memcpy(crc_buf, &hdr, 6);
    std::memcpy(crc_buf + 6, payload, payload_len);
    hdr.crc16 = exm_crc16(crc_buf, 6 + payload_len);
    
    uint8_t packet[8 + 512];
    std::memcpy(packet, &hdr, 8);
    std::memcpy(packet + 8, payload, payload_len);
    
    return write_all(fd, packet, 8 + payload_len) == (ssize_t)(8 + payload_len);
}

static bool recv_packet(int fd, ExmHeader* hdr, uint8_t* payload, size_t max_payload) {
    ssize_t n = read_exact(fd, hdr, sizeof(ExmHeader));
    if (n != sizeof(ExmHeader)) return false;
    
    if (hdr->magic != 0x4558) return false;
    if (hdr->payload_len > max_payload) return false;
    if (hdr->payload_len > 512) return false;
    
    n = read_exact(fd, payload, hdr->payload_len);
    if (n != (ssize_t)hdr->payload_len) return false;
    
    // Verify CRC
    uint8_t crc_buf[6 + 512];
    std::memcpy(crc_buf, hdr, 6);
    std::memcpy(crc_buf + 6, payload, hdr->payload_len);
    uint16_t crc = exm_crc16(crc_buf, 6 + hdr->payload_len);
    if (crc != hdr->crc16) return false;
    
    return true;
}

/* ─── HMAC Verification ─── */
static bool verify_probe_intent(const ExmProbeIntent* intent) {
    if (!g_hmac_key_set) return false;
    
    // Check sequence number
    if (intent->seq_num <= g_last_valid_seq) {
        return false;
    }
    
    // Verify HMAC over first 20 bytes
    uint8_t expected_hmac[32];
    exm_probe_intent_compute_hmac(g_hmac_key, 32, (const uint8_t*)intent, expected_hmac);
    
    if (exm_ct_memcmp(intent->hmac_sha256, expected_hmac, 32) != 0) {
        return false;
    }
    
    g_last_valid_seq = intent->seq_num;
    return true;
}

/* ─── Mock Serial for Testing ─── */
static bool run_mock_tests() {
    printf("Running mock serial tests...\n");
    
    // Test 1: Valid signed PKT_PROBE_INTENT accepted
    {
        ExmProbeIntent intent = {};
        intent.intent_id = INTENT_SCAN_BUS;
        intent.confidence = 200;
        intent.seq_num = 1;
        intent.rail_mv = 3300;
        intent.ripple_mv = 50;
        intent.i2c_count = 0;
        intent.who_am_i = 0;
        
        // Compute HMAC
        exm_probe_intent_compute_hmac(g_hmac_key, 32, (const uint8_t*)&intent, intent.hmac_sha256);
        
        if (!verify_probe_intent(&intent)) {
            printf("  FAIL: Valid intent rejected\n");
            return false;
        }
        printf("  PASS: Valid signed PKT_PROBE_INTENT accepted\n");
    }
    
    // Test 2: Tampered HMAC rejected
    {
        ExmProbeIntent intent = {};
        intent.intent_id = INTENT_CHECK_RAIL;
        intent.confidence = 150;
        intent.seq_num = 2;
        intent.rail_mv = 5000;
        intent.ripple_mv = 100;
        intent.i2c_count = 0;
        intent.who_am_i = 0;
        
        // Compute valid HMAC then tamper
        exm_probe_intent_compute_hmac(g_hmac_key, 32, (const uint8_t*)&intent, intent.hmac_sha256);
        intent.hmac_sha256[0] ^= 0xFF;  // Tamper
        
        if (verify_probe_intent(&intent)) {
            printf("  FAIL: Tampered HMAC accepted\n");
            return false;
        }
        printf("  PASS: Tampered HMAC rejected\n");
    }
    
    // Test 3: Replayed seq_num rejected
    {
        ExmProbeIntent intent = {};
        intent.intent_id = INTENT_BUILD_FIX;
        intent.confidence = 180;
        intent.seq_num = 1;  // Replay seq=1 from test 1
        intent.rail_mv = 3300;
        intent.ripple_mv = 30;
        intent.i2c_count = 0;
        intent.who_am_i = 0;
        
        exm_probe_intent_compute_hmac(g_hmac_key, 32, (const uint8_t*)&intent, intent.hmac_sha256);
        
        if (verify_probe_intent(&intent)) {
            printf("  FAIL: Replayed seq_num accepted\n");
            return false;
        }
        printf("  PASS: Replayed seq_num rejected\n");
    }
    
    // Test 4: Sandbox compile gate passes
    {
        // Create a temporary test file in current directory
        const char* test_file = "exm_sandbox_test.c";
        FILE* fp = fopen(test_file, "w");
        if (fp) {
            fprintf(fp, "#include <stdio.h>\nint main() {\n    return 0;\n}\n");
            fclose(fp);
        }
        
        const char* valid_diff = 
            "--- a/exm_sandbox_test.c\n"
            "+++ b/exm_sandbox_test.c\n"
            "@@ -1,4 +1,5 @@\n"
            " #include <stdio.h>\n"
            " int main() {\n"
            "+    printf(\"Hello\\n\");\n"
            "     return 0;\n"
            " }\n";
        
        SandboxResult result = sandbox_apply_and_compile(valid_diff, strlen(valid_diff));
        if (!result.success) {
            printf("  FAIL: Valid diff compilation failed: %s\n", result.error);
            unlink(test_file);
            return false;
        }
        printf("  PASS: Sandbox compile gate passes\n");
        unlink(test_file);
    }
    
    printf("All mock tests PASSED\n");
    return true;
}

/* ─── LLM Client ─── */
static bool send_to_llm(const char* prompt, std::string& response) {
    // Parse URL
    std::string url = g_config.llm_url;
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string path = "/completion";
    
    // Simple HTTP POST
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }
    
    // JSON request
    std::string json = "{\"prompt\":\"" + std::string(prompt) + "\",\"n_predict\":512,\"temperature\":0.1,\"grammar\":\"unified_diff\"}";
    char request[4096];
    snprintf(request, sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path.c_str(), host.c_str(), port, json.size(), json.c_str());
    
    send(sock, request, strlen(request), 0);
    
    // Read response (simplified)
    char buffer[8192];
    ssize_t n = recv(sock, buffer, sizeof(buffer) - 1, 0);
    close(sock);
    
    if (n <= 0) return false;
    buffer[n] = '\0';
    
    // Extract JSON body (skip headers)
    char* body = strstr(buffer, "\r\n\r\n");
    if (body) body += 4;
    else body = buffer;
    
    response = body;
    return true;
}

/* ─── Main Loop ─── */
static void run_daemon() {
    printf("ExMachina Host Daemon (exmd) starting...\n");
    printf("  Serial: %s @ %d baud\n", g_config.serial_port, g_config.baud_rate);
    printf("  LLM: %s\n", g_config.llm_url);
    
    // Open serial
    if (g_config.mock_serial) {
        printf("  Mode: Mock serial (running tests)\n");
        if (!run_mock_tests()) {
            printf("Mock tests FAILED\n");
            exit(1);
        }
        printf("Mock tests PASSED\n");
        return;
    }
    
    g_serial_fd = open_serial(g_config.serial_port, g_config.baud_rate);
    if (g_serial_fd < 0) {
        perror("Failed to open serial port");
        exit(1);
    }
    printf("  Serial opened: fd=%d\n", g_serial_fd);
    
    // Epoll setup
    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0) {
        perror("epoll_create1");
        exit(1);
    }
    
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_serial_fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_serial_fd, &ev) < 0) {
        perror("epoll_ctl serial");
        exit(1);
    }
    
    // Timer for heartbeat (500ms)
    g_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (g_timer_fd < 0) {
        perror("timerfd_create");
        exit(1);
    }
    
    struct itimerspec its = {};
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 500 * 1000 * 1000;  // 500ms
    its.it_value = its.it_interval;
    timerfd_settime(g_timer_fd, 0, &its, nullptr);
    
    ev.events = EPOLLIN;
    ev.data.fd = g_timer_fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_timer_fd, &ev) < 0) {
        perror("epoll_ctl timer");
        exit(1);
    }
    
    printf("Daemon running (Ctrl+C to stop)...\n");
    
    uint8_t rx_payload[512];
    struct epoll_event events[8];
    
    while (g_running) {
        int n = epoll_wait(g_epoll_fd, events, 8, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            
            if (fd == g_timer_fd) {
                // Send heartbeat
                uint64_t expirations;
                read(g_timer_fd, &expirations, sizeof(expirations));
                
                ExmHeartbeat hb;
                hb.uptime_ms = (uint32_t)(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                hb.status_flags = 0x01;
                
                send_packet(g_serial_fd, PKT_HEARTBEAT, 0, &hb, sizeof(hb));
            }
            else if (fd == g_serial_fd) {
                ExmHeader hdr;
                if (recv_packet(g_serial_fd, &hdr, rx_payload, sizeof(rx_payload))) {
                    if (hdr.type == PKT_PROBE_INTENT && hdr.payload_len == sizeof(ExmProbeIntent)) {
                        ExmProbeIntent* intent = (ExmProbeIntent*)rx_payload;
                        
                        printf("Received PROBE_INTENT: seq=%u, intent=%u, conf=%u\n",
                               intent->seq_num, intent->intent_id, intent->confidence);
                        
                        if (verify_probe_intent(intent)) {
                            printf("  HMAC verified, seq=%u\n", intent->seq_num);
                            
                            // Build prompt for LLM with telemetry + RAG context
                            // For now, just echo back
                            std::string prompt = "Probe intent received: seq=" + std::to_string(intent->seq_num) + 
                                               ", intent=" + std::to_string(intent->intent_id) +
                                               ", rail_mv=" + std::to_string(intent->rail_mv) +
                                               ", ripple_mv=" + std::to_string(intent->ripple_mv);
                            
                            // TODO: Fuse with RAG context from M3+M4
                            
                            // Send to LLM
                            std::string response;
                            if (send_to_llm(prompt.c_str(), response)) {
                                printf("  LLM response received\n");
                                // TODO: Parse response, apply via sandbox if unified diff
                            }
                        } else {
                            printf("  HMAC verification FAILED\n");
                        }
                    }
                    else if (hdr.type == PKT_HEARTBEAT) {
                        // Probe heartbeat
                    }
                    else if (hdr.type == PKT_AUDIO_FRAME) {
                        // Audio frame from probe (KWS)
                    }
                }
            }
        }
    }
    
    printf("Shutting down...\n");
}

int main(int argc, char** argv) {
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_config.serial_port = argv[++i];
        }
        else if (strcmp(argv[i], "--llm-url") == 0 && i + 1 < argc) {
            g_config.llm_url = argv[++i];
        }
        else if (strcmp(argv[i], "--mock-serial") == 0) {
            g_config.mock_serial = true;
        }
        else if (strcmp(argv[i], "--hmac-key") == 0 && i + 1 < argc) {
            const char* key = argv[++i];
            size_t len = strlen(key);
            if (len > 32) len = 32;
            std::memcpy(g_hmac_key, key, len);
            std::memset(g_hmac_key + len, 0, 32 - len);
            g_hmac_key_set = true;
        }
    }
    
    // Load HMAC key from env if not provided
    if (!g_hmac_key_set) {
        const char* env_key = getenv("EXM_DEV_HMAC_KEY");
        if (env_key) {
            size_t len = strlen(env_key);
            if (len > 32) len = 32;
            std::memcpy(g_hmac_key, env_key, len);
            std::memset(g_hmac_key + len, 0, 32 - len);
            g_hmac_key_set = true;
        } else {
            // Default test key
            std::memset(g_hmac_key, 0x42, 32);
            g_hmac_key_set = true;
        }
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    run_daemon();
    
    if (g_serial_fd >= 0) close_serial(g_serial_fd);
    if (g_epoll_fd >= 0) close(g_epoll_fd);
    if (g_timer_fd >= 0) close(g_timer_fd);
    
    return 0;
}