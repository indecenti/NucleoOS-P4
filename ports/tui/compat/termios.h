// termios.h — the terminal interface for the NucleoOS full-screen (tty) ports.
//
// WASI has no termios: a WASI program cannot switch its terminal between cooked and raw mode.
// On NucleoOS it does not need to: the Terminal sends every key straight through (raw) while a
// program shows the alternate screen, which is what curses does on start (smcup). This header
// declares the standard interface with Linux's values so curses and the programs compile
// unchanged; nv_tty.c keeps the attributes a program sets and reads them back, and changes
// nothing else.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char cc_t;
typedef unsigned int  speed_t;
typedef unsigned int  tcflag_t;

#define NCCS 32
struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  __c_ispeed, __c_ospeed;
};

// c_cc indexes
#define VINTR     0
#define VQUIT     1
#define VERASE    2
#define VKILL     3
#define VEOF      4
#define VTIME     5
#define VMIN      6
#define VSWTC     7
#define VSTART    8
#define VSTOP     9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

// c_iflag
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IUCLC   0001000
#define IXON    0002000
#define IXANY   0004000
#define IXOFF   0010000
#define IMAXBEL 0020000
#define IUTF8   0040000

// c_oflag
#define OPOST  0000001
#define OLCUC  0000002
#define ONLCR  0000004
#define OCRNL  0000010
#define ONOCR  0000020
#define ONLRET 0000040
#define OFILL  0000100
#define OFDEL  0000200
#define NLDLY  0000400
#define NL0    0000000
#define NL1    0000400
#define CRDLY  0003000
#define CR0    0000000
#define TABDLY 0014000
#define TAB0   0000000
#define TAB3   0014000
#define XTABS  0014000
#define BSDLY  0020000
#define VTDLY  0040000
#define FFDLY  0100000

// c_cflag
#define CBAUD  0010017
#define CSIZE  0000060
#define CS5    0000000
#define CS6    0000020
#define CS7    0000040
#define CS8    0000060
#define CSTOPB 0000100
#define CREAD  0000200
#define PARENB 0000400
#define PARODD 0001000
#define HUPCL  0002000
#define CLOCAL 0004000

// c_lflag
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define ECHOCTL 0001000
#define ECHOPRT 0002000
#define ECHOKE  0004000
#define IEXTEN  0100000

// speeds
#define B0      0000000
#define B50     0000001
#define B75     0000002
#define B110    0000003
#define B134    0000004
#define B150    0000005
#define B200    0000006
#define B300    0000007
#define B600    0000010
#define B1200   0000011
#define B1800   0000012
#define B2400   0000013
#define B4800   0000014
#define B9600   0000015
#define B19200  0000016
#define B38400  0000017
#define B57600  0010001
#define B115200 0010002
#define B230400 0010003

// tcsetattr / tcflush / tcflow
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2
#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

// The window size (Linux's ioctl and layout). nv_tty.c answers TIOCGWINSZ on the terminal from
// the size the Terminal reported (LINES / COLUMNS), when linked with -Wl,--wrap=ioctl.
struct winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

int     tcgetattr(int fd, struct termios *t);
int     tcsetattr(int fd, int act, const struct termios *t);
int     tcflush(int fd, int queue);
int     tcdrain(int fd);
int     tcflow(int fd, int action);
int     tcsendbreak(int fd, int duration);
void    cfmakeraw(struct termios *t);
speed_t cfgetospeed(const struct termios *t);
speed_t cfgetispeed(const struct termios *t);
int     cfsetospeed(struct termios *t, speed_t s);
int     cfsetispeed(struct termios *t, speed_t s);
int     cfsetspeed(struct termios *t, speed_t s);

#ifdef __cplusplus
}
#endif
