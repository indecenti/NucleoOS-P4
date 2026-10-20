// nv_termios.c — termios for the NucleoOS full-screen ports (see termios.h). WASI has none: the
// attributes a program sets are kept and read back, so curses' own save/restore logic works, but
// they change nothing. The Terminal itself is raw while the program shows the alternate screen
// (which curses does on start) and passes its output through untouched.
#include <errno.h>
#include <unistd.h>
#include "termios.h"

static struct termios s_tio;
static int s_init;

static int tty_fd(int fd) {
    if (fd < 0 || fd > 2 || !isatty(fd)) { errno = ENOTTY; return 0; }
    if (!s_init) {   // what a fresh Linux tty reports: cooked, echo, UTF-8, 38400 baud
        s_init = 1;
        s_tio.c_iflag = ICRNL | IXON | IUTF8;
        s_tio.c_oflag = OPOST | ONLCR;
        s_tio.c_cflag = CS8 | CREAD | B38400;
        s_tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
        s_tio.c_cc[VINTR] = 3;   s_tio.c_cc[VQUIT] = 28;  s_tio.c_cc[VERASE] = 127;
        s_tio.c_cc[VKILL] = 21;  s_tio.c_cc[VEOF] = 4;    s_tio.c_cc[VMIN] = 1;
        s_tio.c_cc[VSTART] = 17; s_tio.c_cc[VSTOP] = 19;  s_tio.c_cc[VSUSP] = 26;
        s_tio.c_cc[VREPRINT] = 18; s_tio.c_cc[VWERASE] = 23; s_tio.c_cc[VLNEXT] = 22;
        s_tio.__c_ispeed = s_tio.__c_ospeed = B38400;
    }
    return 1;
}

int tcgetattr(int fd, struct termios *t) {
    if (!tty_fd(fd)) return -1;
    *t = s_tio;
    return 0;
}

int tcsetattr(int fd, int act, const struct termios *t) {
    (void)act;
    if (!tty_fd(fd)) return -1;
    s_tio = *t;
    return 0;
}

int tcflush(int fd, int queue)       { (void)queue; return tty_fd(fd) ? 0 : -1; }
int tcdrain(int fd)                  { return tty_fd(fd) ? 0 : -1; }
int tcflow(int fd, int action)       { (void)action; return tty_fd(fd) ? 0 : -1; }
int tcsendbreak(int fd, int d)       { (void)d; return tty_fd(fd) ? 0 : -1; }

void cfmakeraw(struct termios *t) {
    t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

speed_t cfgetospeed(const struct termios *t)   { return t->__c_ospeed; }
speed_t cfgetispeed(const struct termios *t)   { return t->__c_ispeed; }
int cfsetospeed(struct termios *t, speed_t s)  { t->__c_ospeed = s; return 0; }
int cfsetispeed(struct termios *t, speed_t s)  { t->__c_ispeed = s; return 0; }
int cfsetspeed(struct termios *t, speed_t s)   { t->__c_ispeed = t->__c_ospeed = s; return 0; }
