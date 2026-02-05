#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define XSH_SOCKET_MAX 108
#define XSH_DEFAULT_SOCKET_FMT "/tmp/xspeedhack_%d.sock"

#if defined(__APPLE__)
#define XSH_GETTIMEOFDAY_VOID_TZ 1
#elif defined(__GLIBC__) && defined(__GLIBC_MINOR__)
/* Newer glibc headers may declare gettimeofday(..., void*), older ones use struct timezone*. */
#if (__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34)
#define XSH_GETTIMEOFDAY_VOID_TZ 1
#else
#define XSH_GETTIMEOFDAY_VOID_TZ 0
#endif
#else
#define XSH_GETTIMEOFDAY_VOID_TZ 0
#endif

#if XSH_GETTIMEOFDAY_VOID_TZ
typedef void xsh_timezone_arg_t;
#else
typedef struct timezone xsh_timezone_arg_t;
#endif

typedef int (*clock_gettime_fn)(clockid_t, struct timespec *);
typedef int (*clock_nanosleep_fn)(clockid_t, int, const struct timespec *, struct timespec *);
typedef int (*nanosleep_fn)(const struct timespec *, struct timespec *);
typedef int (*gettimeofday_fn)(struct timeval *, xsh_timezone_arg_t *);
typedef time_t (*time_fn)(time_t *);
typedef unsigned int (*sleep_fn)(unsigned int);
typedef int (*usleep_fn)(useconds_t);

static clock_gettime_fn real_clock_gettime = NULL;
static clock_nanosleep_fn real_clock_nanosleep = NULL;
static nanosleep_fn real_nanosleep = NULL;
static gettimeofday_fn real_gettimeofday = NULL;
static time_fn real_time = NULL;
static sleep_fn real_sleep = NULL;
static usleep_fn real_usleep = NULL;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static double g_speed = 1.0;
static int64_t g_real_base_mono_ns = 0;
static int64_t g_scaled_base_mono_ns = 0;
static int64_t g_real_base_rt_ns = 0;
static int64_t g_scaled_base_rt_ns = 0;
static int g_scale_realtime = 0;
static char g_socket_path[XSH_SOCKET_MAX] = {0};

static int64_t timespec_to_ns(const struct timespec *ts) {
    return (int64_t)ts->tv_sec * 1000000000LL + (int64_t)ts->tv_nsec;
}

static struct timespec ns_to_timespec(int64_t ns) {
    struct timespec ts;
    if (ns < 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        return ts;
    }
    ts.tv_sec = (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    return ts;
}

static int is_monotonic_clock(clockid_t clk_id) {
    if (clk_id == CLOCK_MONOTONIC) {
        return 1;
    }
#ifdef CLOCK_MONOTONIC_RAW
    if (clk_id == CLOCK_MONOTONIC_RAW) {
        return 1;
    }
#endif
#ifdef CLOCK_MONOTONIC_COARSE
    if (clk_id == CLOCK_MONOTONIC_COARSE) {
        return 1;
    }
#endif
#ifdef CLOCK_BOOTTIME
    if (clk_id == CLOCK_BOOTTIME) {
        return 1;
    }
#endif
    return 0;
}

static int is_realtime_clock(clockid_t clk_id) {
    if (clk_id == CLOCK_REALTIME) {
        return 1;
    }
#ifdef CLOCK_REALTIME_COARSE
    if (clk_id == CLOCK_REALTIME_COARSE) {
        return 1;
    }
#endif
    return 0;
}

static int64_t real_monotonic_ns(void) {
    struct timespec ts;
    if (real_clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return timespec_to_ns(&ts);
}

static int64_t real_realtime_ns(void) {
    struct timespec ts;
    if (real_clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return timespec_to_ns(&ts);
}

static double get_speed(void) {
    double val;
    pthread_mutex_lock(&g_lock);
    val = g_speed;
    pthread_mutex_unlock(&g_lock);
    return val;
}

static void update_speed(double new_speed) {
    int64_t now_real_mono = real_monotonic_ns();
    int64_t now_real_rt = real_realtime_ns();

    pthread_mutex_lock(&g_lock);
    int64_t now_scaled_mono = g_scaled_base_mono_ns +
        (int64_t)((double)(now_real_mono - g_real_base_mono_ns) * g_speed);
    g_real_base_mono_ns = now_real_mono;
    g_scaled_base_mono_ns = now_scaled_mono;

    if (g_scale_realtime) {
        int64_t now_scaled_rt = g_scaled_base_rt_ns +
            (int64_t)((double)(now_real_rt - g_real_base_rt_ns) * g_speed);
        g_real_base_rt_ns = now_real_rt;
        g_scaled_base_rt_ns = now_scaled_rt;
    }

    g_speed = new_speed;
    pthread_mutex_unlock(&g_lock);
}

static int64_t scale_monotonic_ns(int64_t real_ns) {
    int64_t scaled;
    pthread_mutex_lock(&g_lock);
    scaled = g_scaled_base_mono_ns +
        (int64_t)((double)(real_ns - g_real_base_mono_ns) * g_speed);
    pthread_mutex_unlock(&g_lock);
    return scaled;
}

static int64_t scale_realtime_ns(int64_t real_ns) {
    int64_t scaled;
    pthread_mutex_lock(&g_lock);
    scaled = g_scaled_base_rt_ns +
        (int64_t)((double)(real_ns - g_real_base_rt_ns) * g_speed);
    pthread_mutex_unlock(&g_lock);
    return scaled;
}

static void compute_socket_path(void) {
    const char *env_path = getenv("XSH_SOCKET_PATH");
    if (env_path != NULL && env_path[0] != '\0') {
        if (strlen(env_path) >= sizeof(g_socket_path)) {
            fprintf(stderr, "[xspeedhack] XSH_SOCKET_PATH too long, falling back to default\n");
        } else {
            strncpy(g_socket_path, env_path, sizeof(g_socket_path) - 1);
            return;
        }
    }

    snprintf(g_socket_path, sizeof(g_socket_path), XSH_DEFAULT_SOCKET_FMT, getpid());
}

static void *socket_thread_main(void *arg) {
    (void)arg;

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[xspeedhack] socket");
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_socket_path, sizeof(addr.sun_path) - 1);

    unlink(g_socket_path);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("[xspeedhack] bind");
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 1) != 0) {
        perror("[xspeedhack] listen");
        close(server_fd);
        return NULL;
    }

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("[xspeedhack] accept");
            break;
        }

        unsigned char buf[4];
        size_t received = 0;
        while (1) {
            ssize_t n = read(client_fd, buf + received, 4 - received);
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            received += (size_t)n;
            if (received == 4) {
                float speed_val = 0.0f;
                memcpy(&speed_val, buf, sizeof(float));
                if (speed_val >= 0.0f) {
                    update_speed((double)speed_val);
                }
                received = 0;
            }
        }

        close(client_fd);
    }

    close(server_fd);
    return NULL;
}

static void init_real_symbols(void) {
    real_clock_gettime = (clock_gettime_fn)dlsym(RTLD_NEXT, "clock_gettime");
    real_clock_nanosleep = (clock_nanosleep_fn)dlsym(RTLD_NEXT, "clock_nanosleep");
    real_nanosleep = (nanosleep_fn)dlsym(RTLD_NEXT, "nanosleep");
    real_gettimeofday = (gettimeofday_fn)dlsym(RTLD_NEXT, "gettimeofday");
    real_time = (time_fn)dlsym(RTLD_NEXT, "time");
    real_sleep = (sleep_fn)dlsym(RTLD_NEXT, "sleep");
    real_usleep = (usleep_fn)dlsym(RTLD_NEXT, "usleep");

    const char *scale_rt = getenv("XSH_SCALE_REALTIME");
    if (scale_rt && strcmp(scale_rt, "1") == 0) {
        g_scale_realtime = 1;
    }

    g_real_base_mono_ns = real_monotonic_ns();
    g_scaled_base_mono_ns = g_real_base_mono_ns;
    g_real_base_rt_ns = real_realtime_ns();
    g_scaled_base_rt_ns = g_real_base_rt_ns;

    compute_socket_path();

    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, socket_thread_main, NULL) == 0) {
        pthread_detach(thread_id);
    } else {
        fprintf(stderr, "[xspeedhack] failed to start socket thread\n");
    }
}

static void ensure_init(void) {
    pthread_once(&g_init_once, init_real_symbols);
}

__attribute__((constructor))
static void xspeedhack_constructor(void) {
    ensure_init();
}

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    ensure_init();
    if (!real_clock_gettime) {
        return -1;
    }

    int rc = real_clock_gettime(clk_id, tp);
    if (rc != 0 || tp == NULL) {
        return rc;
    }

    if (is_monotonic_clock(clk_id)) {
        int64_t real_ns = timespec_to_ns(tp);
        int64_t scaled_ns = scale_monotonic_ns(real_ns);
        *tp = ns_to_timespec(scaled_ns);
        return rc;
    }

    if (g_scale_realtime && is_realtime_clock(clk_id)) {
        int64_t real_ns = timespec_to_ns(tp);
        int64_t scaled_ns = scale_realtime_ns(real_ns);
        *tp = ns_to_timespec(scaled_ns);
    }

    return rc;
}

int gettimeofday(struct timeval *tv, xsh_timezone_arg_t *tz) {
    ensure_init();
    if (!real_gettimeofday) {
        return -1;
    }
    int rc = real_gettimeofday(tv, tz);
    if (rc != 0 || tv == NULL) {
        return rc;
    }

    if (g_scale_realtime) {
        int64_t real_ns = (int64_t)tv->tv_sec * 1000000000LL + (int64_t)tv->tv_usec * 1000LL;
        int64_t scaled_ns = scale_realtime_ns(real_ns);
        tv->tv_sec = (time_t)(scaled_ns / 1000000000LL);
        tv->tv_usec = (suseconds_t)((scaled_ns % 1000000000LL) / 1000LL);
    }

    return rc;
}

time_t time(time_t *tloc) {
    ensure_init();
    if (!real_time) {
        return (time_t)-1;
    }

    if (!g_scale_realtime) {
        return real_time(tloc);
    }

    struct timespec ts;
    if (real_clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return real_time(tloc);
    }

    int64_t scaled_ns = scale_realtime_ns(timespec_to_ns(&ts));
    time_t scaled = (time_t)(scaled_ns / 1000000000LL);
    if (tloc) {
        *tloc = scaled;
    }
    return scaled;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    ensure_init();
    if (!real_nanosleep) {
        return -1;
    }
    if (req == NULL) {
        return real_nanosleep(req, rem);
    }

    double speed = get_speed();
    if (speed <= 0.0) {
        return real_nanosleep(req, rem);
    }

    int64_t req_ns = timespec_to_ns(req);
    int64_t adj_ns = (int64_t)((double)req_ns / speed);
    struct timespec adj = ns_to_timespec(adj_ns);
    struct timespec rem_real;
    int rc = real_nanosleep(&adj, rem ? &rem_real : NULL);
    if (rc != 0 && rem != NULL) {
        int64_t rem_scaled_ns = (int64_t)((double)timespec_to_ns(&rem_real) * speed);
        *rem = ns_to_timespec(rem_scaled_ns);
    }
    return rc;
}

int clock_nanosleep(clockid_t clk_id, int flags, const struct timespec *req, struct timespec *rem) {
    ensure_init();
    if (!real_clock_nanosleep) {
        errno = ENOSYS;
        return -1;
    }

    if (req == NULL || (flags & TIMER_ABSTIME)) {
        return real_clock_nanosleep(clk_id, flags, req, rem);
    }

    double speed = get_speed();
    if (speed <= 0.0) {
        return real_clock_nanosleep(clk_id, flags, req, rem);
    }

    int64_t req_ns = timespec_to_ns(req);
    int64_t adj_ns = (int64_t)((double)req_ns / speed);
    struct timespec adj = ns_to_timespec(adj_ns);
    struct timespec rem_real;
    int rc = real_clock_nanosleep(clk_id, flags, &adj, rem ? &rem_real : NULL);
    if (rc != 0 && rem != NULL) {
        int64_t rem_scaled_ns = (int64_t)((double)timespec_to_ns(&rem_real) * speed);
        *rem = ns_to_timespec(rem_scaled_ns);
    }
    return rc;
}

int usleep(useconds_t usec) {
    struct timespec req;
    req.tv_sec = (time_t)(usec / 1000000U);
    req.tv_nsec = (long)((usec % 1000000U) * 1000U);
    return nanosleep(&req, NULL);
}

unsigned int sleep(unsigned int seconds) {
    struct timespec req;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = 0;
    struct timespec rem;
    int rc = nanosleep(&req, &rem);
    if (rc == 0) {
        return 0;
    }
    return (unsigned int)rem.tv_sec;
}
