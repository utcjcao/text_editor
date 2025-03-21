#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// define commands for ctrl+

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

// give it a large int value out of range for a char so it doesn't conflict
// with anything. also we only define the first value, but the rest of the values
// are defined incrementally.
enum editorKey {
    BACKSPACE = 127,
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

// edit row: stores a line of text
// also store rsize and render for tabs, b/c we
// want to control how tabs are rendered
// rsize and render store size and string for translated string
typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

// rx stores the current x for a render string(b/c a tab should be treated as one character)
struct editorConfig {
    int cx, cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};

struct editorConfig E;

// prototype functions
//  we use these b/c we want to call functions before they are defined
//  (since c assumes it runs in one path from top to bottom)

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

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

// converts Cx to Rx, based on # of tabs, characters in a row
int editorRowCxtoRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            // basically takes the remainder of all characters divided by tab space character length,
            // finds the next available tab space location
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

// update function to translate certain characters
// eg tabs to multiple spaces
void editorUpdateRow(erow *row) {
    int tabs = 0;

    // count # of tabs
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            // if i get a tab, fill in 8 spaces
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

// updates each row one by one, reallocs for each new row
void editorInsertRow(int at, char *s, size_t len) {
    // validate at
    if (at < 0 || at > E.numrows) {
        return;
    }
    // reallocate mem
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    // move all the rows up by one
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) {
        return;
    }
    editorFreeRow(&E.row[at]);
    // after we deleted the row, we move all the other rows down one
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

// memmove is a like memcopy, but we allocated 2 more bytes, so we can
//  append the strings together (prev, middle, end)
void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) {
        at = row->size;
    }
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    // reallocate more memory
    row->chars = realloc(row->chars, row->size + len + 1);
    // memcpy the string ath the end of the list
    memcpy(&row->chars[row->size], s, len);
    // update size and append null term
    row->size += len;
    row->chars[row->size] = '\0';
    // update the row
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) {
        return;
    }
    // moves the characters from at+1 beyond to char[at] location, where we use row->size -at bytes to move

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

void editorInsertChar(int c) {
    // if we're at the end, we add a new row
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    // we insert the given character
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline() {
    if (E.cx == 0) {
        // if we have an empty row
        editorInsertRow(E.cy, "", 0);

    } else {
        // if we want to push a row down
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDeleteChar() {
    // if we're at the end, nothing to delete
    if (E.cy == E.numrows) {
        return;
    }
    if (E.cx == 0 && E.cy == 0) {
        return;
    }
    erow *row = &E.row[E.cy];
    // otherwise delete character if we have a nonempty row
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        // copy prev line to new line
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        // delete prev line
        editorDelRow(E.cy);
        E.cy--;
    }
}

// file input/output

char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int j = 0; j < E.numrows; j++) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        // copy the given row
        memcpy(p, E.row[j].chars, E.row[j].size);
        // move the string pointer to end of string
        p += E.row[j].size;
        // add newline
        *p = '\n';
        // go to next character
        p++;
        // repeat
    }
    return buf;
}

// you can open a file, and read it all in
void editorOpen(char *filename) {
    // save a copy of the filename so we can display it in status bar (or null if new file)
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        die("fopen");
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    // getline gets the current line from fp, sets linecap to total length
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            linelen--;
        }
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)");
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    // open the given file
    // RDWR is for read _ write
    // CREAT is for the file perms the new file should have (default 0644)
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        // truncates the length of preexisting file.
        // we do this b/c if we don't, we risk losing all of the data if the write
        // call fails, so ftruncate preserves some data
        if (ftruncate(fd, len) != -1) {
            // write in buffer
            if (write(fd, buf, len) == len) {
                // close the file reader
                close(fd);
                // free the buffer pointer
                free(buf);
                editorSetStatusMessage("%d bytes written to disk", len);
                E.dirty = 0;
                return;
            }
        }
        close(fd);
    } else {
        // always has to be done regardless of file checking error b/c its still in mem
        free(buf);
        editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
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

// we set our rowoff as the number of rows away from top of screen
// we set our coloff as the number of cols away from
// left edge of screen
void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxtoRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        // read in all the lines from E.rowoff ownwards
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (y == E.screenrows / 3 && E.numrows == 0) {
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
        } else {
            // calculate length based on total size minus how many offset of cols
            int len = E.row[filerow].rsize - E.coloff;
            // if col offset is greater than size, we set length of
            // characters to read to 0
            if (len < 0) {
                len = 0;
            }

            if (len > E.screencols) {
                len = E.screencols;
            }
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        // clears a row rather than entire screen when we refresh
        abAppend(ab, "\x1b[K", 3);
        // only add newlines when we aren't on last line
        abAppend(ab, "\r\n", 2);
    }
}

// first escape seq inverts colors, second one switches back to normal formatting
void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    // draw the filename in the status bar
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
    // draw the current line #
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
    // cut it short if it exceeds screen size
    if (len > E.screencols) {
        len = E.screencols;
    }
    abAppend(ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

// update the message if the update is less than 5 sec old
// (so its not too old)
void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) {
        msglen = E.screencols;
    }
    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen() {
    editorScroll();
    // make a new append buffer
    struct abuf ab = ABUF_INIT;

    // clear screen and reposition cursor to top left, hide cursor
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    // draw empty screen or file
    editorDrawRows(&ab);

    // draw status bar
    editorDrawStatusBar(&ab);

    // draw message bar
    editorDrawMessageBar(&ab);

    // move the cursor to where its stored in cy, cx
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    // reposition cursor back, unhide cursor
    abAppend(&ab, "\x1b[?25h", 6);

    // combine all the abAppend statements together
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// va commands help us set up a custom printf function
// takes in a variable # of inputs
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

// input section

// creates a buffer to write to, and awaits key presses to set name
char *editorPrompt(char *prompt) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) {
                buf[--buflen] = '\0';
            }
        }
        if (c == '\x1b') {
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

void editorMoveCursor(int key) {
    // check if we're on a valid line
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
    case ARROW_LEFT:
        if (E.cx != 0) {
            E.cx--;
        } else if (E.cy > 0) {
            // we can press <- at the beginning of a line
            // and go to front of prev line (as long as we)
            // arent on first line)
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        // check if we should be allowed to move cursor to the right
        if (row && E.cx < row->size) {
            E.cx++;
        } else if (row && E.cx == row->size) {
            // we can press -> at the end of a line
            // and go to front of next line
            // (as long as we aren't on last line)
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROW_UP:
        if (E.cy != 0) {
            E.cy--;
        }
        break;
    case ARROW_DOWN:
        if (E.cx < E.numrows) {
            E.cy++;
        }
        break;
    }

    // checks when we move from a different line that we
    // snap to correct value
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

// this funciton takes in the input and also checks for commands
void editorProcessKeypress() {
    static int quit_times = KILO_QUIT_TIMES;
    int c = editorReadKey();

    switch (c) {
    case '\r':
        editorInsertNewline();
        break;
    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0) {
            editorSetStatusMessage("WARNING! File has unsaved changes. Press Ctrl Q %d more times to quit.", quit_times);
            quit_times--;
            return;
        }
        // clears screen
        write(STDOUT_FILENO, "\x1b[2j", 4);
        // repositions cursor
        write(STDOUT_FILENO, "\x1b[H", 3);

        exit(0);
        break;
    case CTRL_KEY('s'):
        editorSave();
        break;

    case HOME_KEY:
        E.cx = 0;
        break;

    case END_KEY:
        // moves it to the end of line
        // we check if its the last line or not, b/c if we didn't,
        // it would automatically go to the next line(which does not exist)
        if (E.cy < E.numrows) {
            E.cx = E.row[E.cy].size;
        }
        break;
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        // this if loop exists b/c del deletes value to right not left
        if (c == DEL_KEY)
            editorMoveCursor(ARROW_RIGHT);
        editorDeleteChar();
        break;

    case PAGE_UP:
    case PAGE_DOWN: {
        if (c == PAGE_UP) {
            // change it to the current top (which is rowoff)
            E.cy = E.rowoff;
        } else if (c == PAGE_DOWN) {
            E.cy = E.rowoff + E.screenrows - 1;
            if (E.cy > E.numrows) {
                // change it to the largest possible bottom
                // (either rowoff + # of screen rows, or the actual bottom)
                E.cy = E.numrows;
            }
        }

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

    case CTRL_KEY('l'):
    case '\x1b':
        break;

    default:
        editorInsertChar(c);
        break;
    }
    quit_times = KILO_QUIT_TIMES;
}
// this is the init section

void initEditor() {
    // keep track of cursor position
    E.cx = 0;
    E.cy = 0;
    // keep track of terminal offset
    E.rowoff = 0;
    E.coloff = 0;
    // total rows
    E.numrows = 0;
    // current row buffer
    E.row = NULL;
    // flag to check if current differs from saved
    E.dirty = 0;
    // saves filename and status msg for status bar
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    // initiate the window size
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getWindowSize");
    }
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-Q to quit | Ctrl S to save");

    // read in the current input while its not q(quit).
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}