#include "ash.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t got_winch;
static volatile sig_atomic_t got_stop;

static void winch_handler(int signo)
{
    (void)signo;
    got_winch = 1;
}

static void stop_handler(int signo)
{
    (void)signo;
    got_stop = signo;
}

static int write_all(int fd, const char *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

static void copy_winsize(int master)
{
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
        ioctl(master, TIOCSWINSZ, &ws);
}

static void make_raw(struct termios *saved)
{
    tcgetattr(STDIN_FILENO, saved);
    struct termios raw = *saved;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                     ICRNL | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void write_header(int logfd)
{
    char stamp[64];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm || strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm) == 0)
        strcpy(stamp, "unknown-time");
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "?");
    char header[PATH_MAX + 128];
    int n = snprintf(header, sizeof(header), "\n### ash transcript %s %s\n",
                     stamp, cwd);
    if (n > 0)
        write_all(logfd, header, (size_t)n);
}

static void supervise(int master, pid_t child, int logfd,
                      const struct termios *saved)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = winch_handler;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = stop_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int stdin_open = 1;
    for (;;) {
        if (got_stop) {
            kill(child, got_stop);
            got_stop = 0;
        }
        if (got_winch) {
            copy_winsize(master);
            got_winch = 0;
        }
        struct pollfd fds[2];
        fds[0].fd = stdin_open ? STDIN_FILENO : -1;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = master;
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (stdin_open && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[4096];
            ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r > 0) {
                if (write_all(master, buf, (size_t)r) < 0)
                    break;
            } else if (r == 0) {
                stdin_open = 0;
            } else if (errno != EINTR) {
                stdin_open = 0;
            }
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[4096];
            ssize_t r = read(master, buf, sizeof(buf));
            if (r > 0) {
                write_all(STDOUT_FILENO, buf, (size_t)r);
                write_all(logfd, buf, (size_t)r);
            } else if (r == 0) {
                break;
            } else if (errno != EINTR && errno != EAGAIN) {
                break;
            }
        }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, saved);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    close(logfd);
    _exit(WIFEXITED(status) ? WEXITSTATUS(status)
                            : 128 + WTERMSIG(status));
}

void transcript_spawn(void)
{
    const char *home = var_get("HOME");
    if (!home || !home[0])
        return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.ash_transcript", home);
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        return;
    if (grantpt(master) < 0 || unlockpt(master) < 0) {
        close(master);
        return;
    }
    char *slave_name = ptsname(master);
    if (!slave_name) {
        close(master);
        return;
    }
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);
        return;
    }
    int logfd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (logfd >= 0) {
        fchmod(logfd, 0600);
        write_header(logfd);
    }
    copy_winsize(master);
    struct termios saved;
    make_raw(&saved);
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        close(master);
        close(slave);
        if (logfd >= 0)
            close(logfd);
        return;
    }
    if (child == 0) {
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO)
            close(slave);
        close(master);
        if (logfd >= 0)
            close(logfd);
        shell.shell_pid = getpid();
        shell.terminal_fd = STDIN_FILENO;
        tcsetpgrp(STDIN_FILENO, getpgrp());
        return;
    }
    close(slave);
    if (logfd < 0) {
        logfd = open("/dev/null", O_WRONLY);
        if (logfd < 0)
            logfd = STDERR_FILENO;
    }
    supervise(master, child, logfd, &saved);
}
