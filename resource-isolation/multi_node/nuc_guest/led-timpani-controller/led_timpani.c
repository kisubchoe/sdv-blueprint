/**
 * LED TIMPANI Controller - Precision LED control based on TIMPANI signal
 *
 * Architecture:
 * - Main Thread: Waits for TIMPANI signal (SIGRTMIN+2) and toggles LED
 * - Worker Thread: Simulates CPU load (busy_work)
 * - Metrics Thread: Prometheus HTTP server (:9101)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sched.h>

/* Constant definitions */
#define SIGNO_TT            (__SIGRTMIN + 2)
#define SERIAL_PORT         "/dev/arduino_led_timpani"
#define BAUD_RATE           B115200
#define INTERVAL_MS         500.0
#define BUSY_ITERATIONS_DEFAULT 13000000
#define SYNC_FILE           "/tmp/timpani_start"
#define LED_STATE_FILE      "/tmp/timpani_led_state"
#define METRICS_PORT        9101

/* Global state */
static volatile sig_atomic_t g_running = 1;
static volatile bool g_sync_ready = false;
static int g_busy_iterations = BUSY_ITERATIONS_DEFAULT;

/* Prometheus metrics */
static struct {
    double delay_ms;
    double interval_ms;
    int led_state;
    unsigned long signal_count;
    pthread_mutex_t lock;
} g_metrics = { .delay_ms = 0, .interval_ms = 500.0, .led_state = 0, .signal_count = 0, .lock = PTHREAD_MUTEX_INITIALIZER };

/* Current time (ms) */
static inline double time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Shutdown signal handler */
static void handle_shutdown(int sig) {
    (void)sig;
    g_running = 0;
}

/* TIMPANI signal handler (empty handler - prevents default termination) */
static void handle_timpani(int sig) {
    (void)sig;
}

/* Serial port setup */
static int serial_open(const char *port) {
    int fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) return -1;

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, BAUD_RATE);
    cfsetispeed(&tty, BAUD_RATE);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8 | CLOCAL | CREAD;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Send LED state */
static inline void led_send(int fd, bool state) {
    if (fd >= 0) {
        char c = state ? '1' : '0';
        write(fd, &c, 1);
    }
}

/* Save LED state to file (for synchronization with normal controller) */
static inline void led_state_save(bool state) {
    FILE *f = fopen(LED_STATE_FILE, "w");
    if (f) {
        fputc(state ? '1' : '0', f);
        fclose(f);
    }
}

/* CPU load task */
static unsigned long long busy_work(int iterations) {
    volatile unsigned long long sum = 0;
    for (int i = 0; i < iterations; i++) {
        sum += (unsigned long long)i * i;
    }
    return sum;
}

/* Worker Thread: Continuous CPU load */
static void *worker_thread(void *arg) {
    (void)arg;

    /* Wait for sync file creation */
    while (g_running && !g_sync_ready) {
        usleep(10000);
    }

    /* Inherit CPU affinity from main thread */
    cpu_set_t cpuset;
    if (sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }

    printf("[Worker] Started busy_work loop\n");
    while (g_running) {
        busy_work(g_busy_iterations);
    }
    printf("[Worker] Stopped\n");
    return NULL;
}

/* Metrics Thread: Prometheus HTTP server */
static void *metrics_thread(void *arg) {
    (void)arg;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[Metrics] socket");
        return NULL;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(METRICS_PORT)
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[Metrics] bind");
        close(server_fd);
        return NULL;
    }

    listen(server_fd, 5);
    printf("[Metrics] Server listening on port %d\n", METRICS_PORT);

    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        if (select(server_fd + 1, &fds, NULL, NULL, &tv) <= 0) continue;

        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;

        /* Read request (simply consume) */
        char buf[512];
        read(client, buf, sizeof(buf));

        /* Generate metrics response */
        pthread_mutex_lock(&g_metrics.lock);
        char body[1024];
        int body_len = snprintf(body, sizeof(body),
            "# HELP led_delay_ms LED control delay in milliseconds\n"
            "# TYPE led_delay_ms gauge\n"
            "led_delay_ms{container=\"timpani\"} %.2f\n"
            "# HELP led_interval_ms LED toggle interval in milliseconds\n"
            "# TYPE led_interval_ms gauge\n"
            "led_interval_ms{container=\"timpani\"} %.2f\n"
            "# HELP led_state Current LED state (0=OFF, 1=ON)\n"
            "# TYPE led_state gauge\n"
            "led_state{container=\"timpani\"} %d\n"
            "# HELP led_signal_count Total LED toggle count\n"
            "# TYPE led_signal_count counter\n"
            "led_signal_count{container=\"timpani\"} %lu\n",
            g_metrics.delay_ms, g_metrics.interval_ms,
            g_metrics.led_state, g_metrics.signal_count);
        pthread_mutex_unlock(&g_metrics.lock);

        char resp[2048];
        int resp_len = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n%s",
            body_len, body);

        write(client, resp, resp_len);
        close(client);
    }

    close(server_fd);
    printf("[Metrics] Server stopped\n");
    return NULL;
}

int main(int argc, char *argv[]) {
    const char *serial_port = (argc > 1) ? argv[1] : SERIAL_PORT;

    /* Disable stdout buffering */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Read busy_iterations from environment variable */
    const char *env_iter = getenv("BUSY_ITERATIONS");
    if (env_iter) g_busy_iterations = atoi(env_iter);
    if (g_busy_iterations <= 0) g_busy_iterations = BUSY_ITERATIONS_DEFAULT;

    printf("=== LED TIMPANI Controller ===\n");
    printf("Serial: %s, Interval: %.0fms, Iterations: %d\n",
           serial_port, INTERVAL_MS, g_busy_iterations);

    /* Set process name (for TIMPANI-N identification) */
    prctl(PR_SET_NAME, "led_timpani", 0, 0, 0);

    /* Remove previous sync file */
    unlink(SYNC_FILE);

    /* Set shutdown signal handlers (SIGINT, SIGTERM) */
    struct sigaction sa;
    sa.sa_handler = handle_shutdown;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Register TIMPANI signal handler (prevents default termination) */
    struct sigaction sa_tt;
    sa_tt.sa_handler = handle_timpani;
    sigemptyset(&sa_tt.sa_mask);
    sa_tt.sa_flags = 0;
    sigaction(SIGNO_TT, &sa_tt, NULL);

    /* Block TIMPANI signal (for synchronous handling with sigtimedwait) */
    sigset_t sig_set;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGNO_TT);
    if (sigprocmask(SIG_BLOCK, &sig_set, NULL) == -1) {
        perror("sigprocmask failed");
        return EXIT_FAILURE;
    }

    /* Connect to serial port */
    int serial_fd = serial_open(serial_port);
    if (serial_fd < 0) {
        fprintf(stderr, "Warning: Serial port unavailable, dry-run mode\n");
    } else {
        printf("Serial connected: %s\n", serial_port);
    }

    /* Start threads */
    pthread_t worker_tid, metrics_tid;
    pthread_create(&metrics_tid, NULL, metrics_thread, NULL);
    pthread_create(&worker_tid, NULL, worker_thread, NULL);

    /* Main Loop: Wait for TIMPANI signal */
    struct timespec timeout = { .tv_sec = 1, .tv_nsec = 0 };
    double last_time = time_ms();
    bool led_state = false;
    unsigned long signal_count = 0;

    while (g_running) {
        int ret = sigtimedwait(&sig_set, NULL, &timeout);
        if (ret == -1) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;  /* Timeout or interrupt - normal */
            } else {
                perror("sigtimedwait failed");
                break;
            }
        }

        if (ret != SIGNO_TT) {
            printf("Unexpected signal: %d\n", ret);
            continue;
        }

        double recv_time = time_ms();

        /* On first signal: create sync file */
        if (!g_sync_ready) {
            FILE *f = fopen(SYNC_FILE, "w");
            if (f) fclose(f);
            g_sync_ready = true;
            last_time = recv_time;
            printf("Sync file created: %s\n", SYNC_FILE);
        }

        /* Toggle LED */
        signal_count++;
        led_state = !led_state;
        led_send(serial_fd, led_state);
        led_state_save(led_state);

        double done_time = time_ms();
        double computation_ms = done_time - recv_time;
        double interval_ms = recv_time - last_time;

        /* Calculate delay */
        double delay_ms;
        if (computation_ms < INTERVAL_MS) {
            delay_ms = (interval_ms > INTERVAL_MS) ? (interval_ms - INTERVAL_MS) : 0;
        } else {
            delay_ms = computation_ms;
        }

        /* Update metrics */
        pthread_mutex_lock(&g_metrics.lock);
        g_metrics.delay_ms = delay_ms;
        g_metrics.interval_ms = interval_ms;
        g_metrics.led_state = led_state ? 1 : 0;
        g_metrics.signal_count = signal_count;
        pthread_mutex_unlock(&g_metrics.lock);

        /* Print log */
        const char *warn = (computation_ms > 100) ? " - DELAY WARNING" : "";
        printf("[Timpani %lu] %sLED %s (interval: %.1fms, delay: %.1fms)%s\n",
               signal_count, (computation_ms > 100) ? "!! " : "",
               led_state ? "ON" : "OFF", interval_ms, delay_ms, warn);

        last_time = recv_time;
    }

    /* Cleanup */
    pthread_join(worker_tid, NULL);
    pthread_join(metrics_tid, NULL);
    unlink(SYNC_FILE);
    if (serial_fd >= 0) close(serial_fd);

    printf("Shutdown complete\n");
    return 0;
}
