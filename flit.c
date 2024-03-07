/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f) // Set upper 3 bits of char to 0. Sameas CTRL key

/*** data ***/

struct editorConfig {
    int screenrows;
    int screencols;

    struct termios old_termios;
};

struct editorConfig E;

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
char editorReadKey() {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) fail("read");
    }

    return c;
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

void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        abAppend(ab, "~", 1);

        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[2J", 4); // Escape character + clear screen command
    abAppend(&ab, "\x1b[H", 3); // Position the cursor : (3 bytes)

    editorDrawRows(&ab);

    abAppend(&ab, "\x1b[H", 3);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);

    //TODO: HIDE THE CURSOR WHEN REPAINTING
}

/*** input ***/

/// @brief Awaits keypress, then handles it.
void editorHandleKeyPress() {
    char c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDERR_FILENO, "\x1b[H", 3);

            exit(0);
            break;
    }
}

/*** init ***/

void initEditor() {
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) fail("getWindowSize");
}

int main() {
    enableRawMode();
    initEditor();

    while(1) {
        editorRefreshScreen();
        editorHandleKeyPress();
    }

    return 0;
}