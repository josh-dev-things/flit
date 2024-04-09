/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define VERSION "0.0.2"
#define TAB_STOP 8

#define CTRL_KEY(k) ((k) & 0x1f) // Set upper 3 bits of char to 0. Sameas CTRL key

enum editorKey {
    BACKSPACE = 127,
    LEFT = 1000,
    RIGHT,
    UP,
    DOWN,
    DEL,
    P_UP,
    P_DOWN
};

/*** data ***/

typedef struct erow {
    int size;
    int rsize;
    char* chars;
    char* render;
} erow;

struct editorConfig {
    int cx, cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow* row;
    int dirty;
    char* filename;
    char statusmsg[80];
    time_t statusmsg_time;

    struct termios old_termios;
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char* fmt, ...);

/*** terminal ***/

/// @brief Program exits with error. 
/// @param s error message
void fail(const char* s) {
    // Clear terminal
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDERR_FILENO, "\x1b[H", 3);

    perror(s); // Print error message
    exit(1);
}

/// @brief Disable Raw Terminal Mode
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.old_termios) == -1){
        fail("tcsetattr");
    }
}

/// @brief Enable Raw Terminal Mode
void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.old_termios) == -1) fail("tcgetattr");
    atexit(disableRawMode); // Restore the terminal attributes upon exit

    struct termios raw = E.old_termios;

    // NB the &= corresponds to AND then assign. Similar to *=
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // Disable input for control characters.
    raw.c_oflag &= ~(OPOST); // Disable output processing
    raw.c_cflag |= (CS8); // Character size: 8 bits per byte
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN); // Disabling echo, canon' input, signals, etc. NOT the bitflags
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) fail("tcsetattr"); // Apply change after pending output written to terminal
}

/// @brief Wait for one keypress & return it
/// @return Pressed key
int editorReadKey() {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) fail("read");
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '3': return DEL;
                        case '5': return P_UP;
                        case '6': return P_DOWN;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return UP;
                    case 'B': return DOWN;
                    case 'C': return RIGHT;
                    case 'D': return LEFT;
                }
            }
        }
    

    return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int* rows, int* cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

/// @brief Convert cx to rx
/// @param row row to convert
int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        rx++;
    }
    return rx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for(j = 0; j < row->size; j++) {
        if(row->chars[j] == '\t') tabs++;
    }
    
    free(row->render);
    row->render = malloc(row->size + tabs*(TAB_STOP-1) + 1);
    
    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) row -> render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorAppendRow(char* s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    
    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len+1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorRowInsertChar(erow* row, int at, int c) {
    if (at < 0 || at > row->size) {
        at = row->size; // Interesting wraparound
    }

    row->chars = realloc(row->chars, row->size+2);
    memmove(&row->chars[at+1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
}

void editorRowDeleteChar(erow* row, int at) {
    if (at < 0 || at > row->size) return;

    memmove(&row->chars[at], &row->chars[at+1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
    if(E.cy == E.numrows) {
        editorAppendRow("", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorDeleteChar() {
    if(E.cy == E.numrows) return;

    erow* row = &E.row[E.cy];
    if(E.cx > 0) {
        editorRowDeleteChar(row, E.cx-1);
        E.cx--;
    }
}

/*** file IO ***/

/// @brief calculate length of buffer & return buffer containing all rows
/// @param buflen pointer to store buffer length into
/// @return all rows in a single buffer
char* editorRowsToString(int* buflen) {
    int totlen = 0;
    int j;
    for(j = 0; j < E.numrows; j++) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    char* buf = malloc(totlen);
    char* p = buf;
    for(j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char* filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE* fp = fopen(filename, "r");
    if(!fp) fail("fopen");

    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    if(linelen != -1)
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave() {
    if (E.filename == NULL) return; // Prompt user for a filename?

    int len;
    char* buf = editorRowsToString(&len);
    
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if(fd != -1) {
        if(ftruncate(fd, len) != -1) {
            if(write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk.", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Write failed. IO error: %s", strerror(errno));
}

/*** append buffer ***/

struct abuf {
    char* b;
    int len
};

#define ABUF_INIT {NULL, 0} // Empty buffer

void abAppend(struct abuf* ab, const char* s, int len) {
    char* new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf* ab) {
    free(ab->b);
}

/*** output ***/

void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
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
    int y;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if(filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Flit editor -- version %s", VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;

                // Padding
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);

                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].chars[E.coloff], len);
        }

        abAppend(ab, "\x1b[K", 3); // Clearing screen by "Erasing in line"
        abAppend(ab, "\r\n", 2);
        
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? ("(modified)", E.dirty) : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d%d",
        E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
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

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\033[H\033[J", 6); // Clear the screen.
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3); // Position the cursor : (3 bytes)

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);

}

void editorSetStatusMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

void editorMoveCursor(int key) { 
    erow* row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case UP:
            if (E.cy != 0) E.cy--;
            break;
        case DOWN:
            if (E.cy < E.numrows) E.cy++;
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

/// @brief Awaits keypress, then handles it.
void editorHandleKeyPress() {
    int c = editorReadKey();

    switch (c) {
        case '\r':
            /* TODO */
            break;

        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDERR_FILENO, "\x1b[H", 3);

            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL:
            if(c == DEL) editorMoveCursor(RIGHT);
            editorDeleteChar();
            break;

        case P_UP:
        case P_DOWN:
            {
                if (c == P_UP) {
                    E.cy = E.rowoff;
                } else if (c == P_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }

                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(c == P_UP ? UP : DOWN);
                break;
            }

        case UP:
        case DOWN:
        case LEFT:
        case RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'): // We already refresh
        case '\x1b':        // Ignoring Escape Key
            break;

        default:
            editorInsertChar(c);
            break;
        
    }
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;

    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) fail("getWindowSize");
    E.screenrows-=2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();

    if(argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

    while(1) {
        editorRefreshScreen();
        editorHandleKeyPress();
    }

    return 0;
}
