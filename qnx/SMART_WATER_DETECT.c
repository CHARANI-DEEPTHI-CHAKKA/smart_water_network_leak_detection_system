#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/neutrino.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define UDP_PORT       5000
#define DASH_PORT      8080
#define ANALYSIS_SEC   5
#define NODE_TIMEOUT   10
#define LEAK_THRESHOLD 0.5f
#define GPIO_PATH      "/dev/gpio/17"
#define RELAY_ACTIVE_LOW 1
#define PULSE_CODE     (_PULSE_CODE_MINAVAIL + 1)
#define JSON_SIZE      1024

typedef struct {
    float n1, n2;
    int seen1, seen2;
    time_t t1, t2;
    pthread_mutex_t lock;
} WaterData;

static WaterData water = {0};
static volatile int running = 1;
static int gpio_fd = -1, channel_id, connection_id;
static timer_t timer_id;
static int last_condition = -1;

static int alive(time_t t)
{
    return t != 0 && (time(NULL) - t) <= NODE_TIMEOUT;
}

static void setValve(int on)
{
    char value;
    if (gpio_fd < 0) gpio_fd = open(GPIO_PATH, O_WRONLY);
    if (gpio_fd < 0) {
        perror("GPIO open");
        return;
    }
#if RELAY_ACTIVE_LOW
    value = on ? '0' : '1';
#else
    value = on ? '1' : '0';
#endif
    if (write(gpio_fd, &value, 1) != 1) perror("GPIO write");
}

/* Build only the fields required for the active condition.
   Virtual pressure is a demo model: P(bar) = 0.03 * flow(mL/s). */
static int evaluate(char *out, size_t cap, int *valve_command,
                    float n1, float n2, int r1, int r2)
{
    float diff = n1 - n2;
    int condition;

    *valve_command = 0; /* Fail-safe default: close valve. */

    if (!r1 && !r2) {
        condition = 1;
        snprintf(out, cap,
          "{\"condition\":1,\"node1Status\":\"DISCONNECTED\","
          "\"node2Status\":\"DISCONNECTED\","
          "\"recommendation\":\"Please connect Node 1 and Node 2 to the prototype\"}");
    } else if (r1 && !r2) {
        condition = 6;
        snprintf(out, cap,
          "{\"condition\":6,\"node1Status\":\"CONNECTED\","
          "\"node2Status\":\"DISCONNECTED\","
          "\"recommendation\":\"Please connect ESP32 Node 2 to the prototype properly\"}");
    } else if (!r1 && r2) {
        condition = 7;
        snprintf(out, cap,
          "{\"condition\":7,\"node1Status\":\"DISCONNECTED\","
          "\"node2Status\":\"CONNECTED\","
          "\"recommendation\":\"Please connect ESP32 Node 1 to the prototype properly\"}");
    } else if (n1 == 0.0f && n2 == 0.0f) {
        condition = 2;
        snprintf(out, cap,
          "{\"condition\":2,\"node1Flow\":0.0,\"node2Flow\":0.0,"
          "\"recommendation\":\"Please turn on your prototype\"}");
    } else if (n1 == 0.0f && n2 > 0.0f) {
        condition = 3;
        snprintf(out, cap,
          "{\"condition\":3,\"node1Status\":\"CONNECTED\",\"node1Flow\":%.2f,"
          "\"node2Status\":\"CONNECTED\",\"node2Flow\":%.2f,"
          "\"recommendation\":\"Please connect ESP32 Node 1 to the prototype properly\"}",
          n1, n2);
    } else if (n1 > 0.0f && n2 == 0.0f) {
        condition = 4;
        snprintf(out, cap,
          "{\"condition\":4,\"node1Flow\":%.2f,\"node1Pressure\":%.2f,"
          "\"node2Flow\":0.0,\"node2Pressure\":0.0,"
          "\"leakStatus\":\"LEAK DETECTED\","
          "\"valveControl\":\"OFF - CLOSE VALVE\","
          "\"recommendation\":\"There may be a break in the pipe between Node 1 and Node 2.\"}",
          n1, 0.03f * n1);
    } else if (n1 > 0.0f && n2 > 0.0f) {
        condition = 5;
        diff = n1 - n2;
        if (diff > LEAK_THRESHOLD) {
            snprintf(out, cap,
              "{\"condition\":5,\"node1Flow\":%.2f,\"node1Pressure\":%.2f,"
              "\"node2Flow\":%.2f,\"node2Pressure\":%.2f,"
              "\"leakStatus\":\"LEAK DETECTED\","
              "\"valveControl\":\"OFF - CLOSE VALVE\","
              "\"recommendation\":\"There is a leak between Node 1 and Node 2.\"}",
              n1, 0.03f * n1, n2, 0.03f * n2);
        } else {
            condition = 50;
            *valve_command = 1;
            snprintf(out, cap,
              "{\"condition\":5,\"node1Flow\":%.2f,\"node1Pressure\":%.2f,"
              "\"node2Flow\":%.2f,\"node2Pressure\":%.2f,"
              "\"leakStatus\":\"NORMAL\","
              "\"valveControl\":\"ON - OPEN VALVE\","
              "\"recommendation\":\"No leak detected between Node 1 and Node 2.\"}",
              n1, 0.03f * n1, n2, 0.03f * n2);
        }
    } else {
        condition = 8;
        snprintf(out, cap,
          "{\"condition\":8,\"node1Status\":\"CONNECTED\",\"node2Status\":\"CONNECTED\","
          "\"recommendation\":\"Invalid flow readings. Check both sensors.\"}");
    }

    return condition;
}

static void *udpReceiver(void *arg)
{
    int sock, node;
    float flow;
    char buf[128];
    struct sockaddr_in addr;
    (void)arg;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("UDP socket"); return NULL; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(UDP_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("UDP bind"); close(sock); return NULL;
    }

    printf("UDP receiver listening on port %d\n", UDP_PORT);
    while (running) {
        int n = (int)recvfrom(sock, buf, sizeof(buf)-1, 0, NULL, NULL);
        if (n <= 0) continue;
        buf[n] = '\0';

        /* Expected packet format: NODE1,12.34 or NODE2,10.50 */
        if (sscanf(buf, "NODE%d,%f", &node, &flow) == 2 && flow >= 0.0f) {
            pthread_mutex_lock(&water.lock);
            if (node == 1) {
                water.n1 = flow; water.seen1 = 1; water.t1 = time(NULL);
            } else if (node == 2) {
                water.n2 = flow; water.seen2 = 1; water.t2 = time(NULL);
            }
            pthread_mutex_unlock(&water.lock);
        }
    }
    close(sock);
    return NULL;
}

static void *analysisThread(void *arg)
{
    struct _pulse pulse;
    (void)arg;
    while (running) {
        int rc = MsgReceive(channel_id, &pulse, sizeof(pulse), NULL);
        if (rc == 0 && pulse.code == PULSE_CODE) {
            char json[JSON_SIZE];
            float n1, n2;
            int r1, r2, valve_on, condition;
            time_t t1, t2;

            pthread_mutex_lock(&water.lock);
            n1 = water.n1; n2 = water.n2;
            r1 = water.seen1; r2 = water.seen2;
            t1 = water.t1; t2 = water.t2;
            pthread_mutex_unlock(&water.lock);

            r1 = r1 && alive(t1);
            r2 = r2 && alive(t2);
            condition = evaluate(json, sizeof(json), &valve_on, n1, n2, r1, r2);
            setValve(valve_on);

            if (condition != last_condition) {
                printf("Condition %d | %s\n", condition,
                       condition == 1 ? "both nodes disconnected" :
                       condition == 2 ? "both flows zero" :
                       condition == 3 ? "Node 1 flow zero" :
                       condition == 4 ? "possible pipe break" :
                       condition == 5 ? "flow comparison" :
                       condition == 6 ? "Node 2 disconnected" :
                       condition == 7 ? "Node 1 disconnected" : "invalid data");
                printf("Dashboard JSON: %s\n", json);
                last_condition = condition;
            }
        }
    }
    return NULL;
}

static int sendAll(int fd, const char *p, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void *dashboardServer(void *arg)
{
    int server, client, opt = 1;
    struct sockaddr_in addr;
    (void)arg;

    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("HTTP socket"); return NULL; }
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DASH_PORT);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("HTTP bind"); close(server); return NULL;
    }
    if (listen(server, 8) < 0) {
        perror("HTTP listen"); close(server); return NULL;
    }

    printf("Dashboard API: http://<QNX-IP>:%d/data\n", DASH_PORT);
    while (running) {
        char req[1024], json[JSON_SIZE], header[256];
        float n1, n2;
        int r1, r2, valve_on, condition;
        time_t t1, t2;
        size_t body_len;
        client = accept(server, NULL, NULL);
        if (client < 0) continue;
        (void)recv(client, req, sizeof(req)-1, 0);

        pthread_mutex_lock(&water.lock);
        n1 = water.n1; n2 = water.n2;
        r1 = water.seen1; r2 = water.seen2;
        t1 = water.t1; t2 = water.t2;
        pthread_mutex_unlock(&water.lock);
        r1 = r1 && alive(t1); r2 = r2 && alive(t2);
        condition = evaluate(json, sizeof(json), &valve_on, n1, n2, r1, r2);
        (void)condition; /* HTTP requests report state; timer thread controls GPIO. */

        body_len = strlen(json);
        snprintf(header, sizeof(header),
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
          "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
          "Content-Length: %lu\r\nConnection: close\r\n\r\n",
          (unsigned long)body_len);
        sendAll(client, header, strlen(header));
        sendAll(client, json, body_len);
        close(client);
    }
    close(server);
    return NULL;
}

int main(void)
{
    pthread_t rx_thread, analysis_thread, http_thread;
    struct sigevent event;
    struct itimerspec timer_spec;

    pthread_mutex_init(&water.lock, NULL);

    channel_id = ChannelCreate(0);
    if (channel_id < 0) { perror("ChannelCreate"); return EXIT_FAILURE; }

    connection_id = ConnectAttach(0, 0, channel_id, _NTO_SIDE_CHANNEL, 0);
    if (connection_id < 0) {
        perror("ConnectAttach"); ChannelDestroy(channel_id); return EXIT_FAILURE;
    }

    SIGEV_PULSE_INIT(&event, connection_id, 10, PULSE_CODE, 0);
    if (timer_create(CLOCK_MONOTONIC, &event, &timer_id) < 0) {
        perror("timer_create"); return EXIT_FAILURE;
    }
    memset(&timer_spec, 0, sizeof(timer_spec));
    timer_spec.it_value.tv_sec = ANALYSIS_SEC;
    timer_spec.it_interval.tv_sec = ANALYSIS_SEC;
    if (timer_settime(timer_id, 0, &timer_spec, NULL) < 0) {
        perror("timer_settime"); return EXIT_FAILURE;
    }

    setValve(0);
    pthread_create(&rx_thread, NULL, udpReceiver, NULL);
    pthread_create(&analysis_thread, NULL, analysisThread, NULL);
    pthread_create(&http_thread, NULL, dashboardServer, NULL);

    printf("Smart Water Network Detection started\n");
    printf("UDP=%d | HTTP=%d | interval=%ds | timeout=%ds | threshold=%.2f mL/s\n",
           UDP_PORT, DASH_PORT, ANALYSIS_SEC, NODE_TIMEOUT, LEAK_THRESHOLD);

    while (running) sleep(1);

    pthread_cancel(rx_thread); pthread_cancel(analysis_thread); pthread_cancel(http_thread);
    pthread_join(rx_thread, NULL); pthread_join(analysis_thread, NULL); pthread_join(http_thread, NULL);
    timer_delete(timer_id);
    ConnectDetach(connection_id);
    ChannelDestroy(channel_id);
    pthread_mutex_destroy(&water.lock);
    if (gpio_fd >= 0) close(gpio_fd);
    return EXIT_SUCCESS;
}
