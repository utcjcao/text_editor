#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// define commands for ctrl+

#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

// give it a large int value out of range for a char so it doesn't conflict
// with anything. also we only define the first value, but the rest of the values
// are defined incrementally.
enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

// data

struct editorConfig {
    int cx, cy;
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct editorConfig E;

// terminal functions

// write an exit function
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2j]", 4);
    write(STDOUT_FILENO, "\x1b[H]", 3);

    perror(s);
    exit(1);
}

void disableRawMode() {
    // turn canonical mode back on when i exit the program
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    // gets terminal attributes
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }
    // stdlib function that runs disable at exit
    atexit(disableRawMode);

    // we get the struct representing the attributes of the terminal
    struct termios raw = E.orig_termios;
    // AND it with the XOR of ECHO AND ICANON we want to turn off only the echo flag.
    // echo flag is represented by 0000000001000
    // ICANON controls whether you're reading it canonically or not.
    // by turning it off, it reads the file byte by byte rather than by line.
    // isig is to tell us to ignore quit commands like Ctrl Z
    // ixon is for input/output flow control
    // icrnl is for command shortcut commands
    // opost is to stop output processing features, so \n to \r\n
    // the other flags are traditional (ie basically doesn't apply to modern compilers, but we still use them)
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    // c_lflag controls local flag modes

    // the c_cc are control characters that control various terminal settings
    // VMIN is the minimum # of bytes to red before read returns. since we set it
    // to 0, it'll return as soon as there's any input
    raw.c_cc[VMIN] = 0;
    // VTIME is the max amount of time to wait before read returns.
    // units are 0.1 sec, so we set max to 0.1 sec
    raw.c_cc[VTIME] = 1;

    // we then set the file input (STDIN_FILENO) to change its mode at TCSAFLUSH
    // (apply changes after input output is empty), and use raw data.
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

// this function waits for one keypress and returns it
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }
    // if you  read an escape char, immediately read two more bytes and check
    // if they match a specific direction. if its not a character we recognize
    // or we don't recieve anything, we just return the escape character
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1':
                        return HOME_KEY;

                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    // x1b is a escape character, [6n is a device status report, and this asks for the
    // current cursor position
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    // read in the input of the command above into buffer.
    // the terminating char is R, so read until its R
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        i++;
    }
    buf[i] = '\0';

    // validate the output
    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }
    // validate that you get two digits back
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    // set our cols and rows to the TIOCWINSZ (terminal input output control window size)
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // if ioctl doesn't work, we move our cursor to the very bottom, read
        // this is an escape string that asks to move the cursor 999 to the bottom,
        // 999 bottom to right
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

// make an append buffer so you don't have multiple write commands
// (might cause flickers)

struct abuf {
    char *b;
    int len;
};

// represents an empty append buffer
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    // resizes previously allocated mem, but add more space for new characters
    char *new = realloc(ab->b, ab->len + len);
    // returns NULL if ab->b does not exist (basically malloc)
    if (new == NULL) {
        return;
    }
    // append the new string
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

// free the string memory
void abFree(struct abuf *ab) {
    free(ab->b);
}

// output section

void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        if (y == E.screenrows / 3) {
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
            if (welcomelen > E.screencols) {
                welcomelen = E.screencols;
            }
            int padding = (E.screencols - welcomelen) / 2;
            if (padding) {
                abAppend(ab, "~", 1);
                padding--;
            }
            while (padding--) {
                abAppend(ab, " ", 1);
            }
            abAppend(ab, welcome, welcomelen);
        } else {
            abAppend(ab, "~", 1);
        }
        // clears a row rather than entire screen when we refresh
        abAppend(ab, "\x1b[K", 3);
        // only add newlines when we aren't on last line
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    // make a new append buffer
    struct abuf ab = ABUF_INIT;

    // clear screen and reposition cursor to top left, hide cursor
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    // draw tildes
    editorDrawRows(&ab);

    // move the cursor to where its stored in cy, cx
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    // reposition cursor back, unhide cursor
    abAppend(&ab, "\x1b[?25h", 6);

    // combine all the abAppend statements together
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// input section

void editorMoveCursor(int key) {
    switch (key) {
    case ARROW_LEFT:
        if (E.cx != 0) {
            E.cx--;
        }
        break;
    case ARROW_RIGHT:
        if (E.cx != E.screencols - 1) {
            E.cx++;
        }
        break;
    case ARROW_UP:
        if (E.cy != 0) {
            E.cy--;
        }
        break;
    case ARROW_DOWN:
        if (E.cx != E.screenrows - 1) {
            E.cy++;
        }
        break;
    }
}

// this funciton takes in the input and also checks for commands
void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
    case CTRL_KEY('q'):
        // clears screen
        write(STDOUT_FILENO, "\x1b[2j", 4);
        // repositions cursor
        write(STDOUT_FILENO, "\x1b[H", 3);

        exit(0);
        break;

    case HOME_KEY:
        E.cx = 0;
        break;

    case END_KEY:
        E.cx = E.screencols - 1;
        break;

    case PAGE_UP:
    case PAGE_DOWN: {
        int times = E.screenrows;
        while (times--) {
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
    } break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;
    }
}
// this is the init section

void initEditor() {
    // keep track of cursor position
    E.cx = 0;
    E.cy = 0;
    // initiate the window size
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getWindowSize");
    }
}

int main() {
    enableRawMode();
    initEditor();
    // read in the current input while its not q(quit).
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}