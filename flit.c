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

#define VERSION "0.2.2"
#define TAB_STOP 8
#define MARGIN 6

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

enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** data ***/

struct editorSyntax {
    char* filetype;     // Name
    char** filematch;   // Pattern matches
    char** keywords;    // Language Keywords
    char* singleline_comment_start;
    char* multiline_comment_start;
    char* multiline_comment_end;
    int flags;          // What to highlight
};

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char* chars;
    char* render;
    unsigned char* hl;
    int hl_open_comment;
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

    int dropped_cursor_x, dropped_cursor_y; // Inneficiency I know
    int selection_start_x, selection_start_y; // Selection start
    int selection_end_x, selection_end_y;     // Selection end
    int selecting;
    char* copy_buffer;
    int copy_buffer_len;

    struct editorSyntax *syntax;
    struct termios old_termios;
};

struct editorConfig E;

/*** filetypes ***/

char* C_HL_extensions[] = {".c", ".h", ".cpp", NULL};
char* C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",

    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL
};

char* MD_HL_extensions[] = {".md", NULL};
char* MD_HL_keywords[] = {
    "#|", NULL
};

char* PY_HL_extensions[] = {".py", NULL};
char* PY_HL_keywords[] = {
    "if", "else", "elif", "for", "while", "break", "continue", "try", "except",
    "finally", "with", "as", "pass", "raise", "yield", "return", "TRUE", "FALSE",
    "None", "and", "or", "not", "in", "is", "lambda",

    "int|", "float|", "list|", "tuple|", "range|", "str|", "dict|", "set|", "bool|",
    "len|", "type|", "print|", "input|", "open|", "enumerate|", NULL
};

struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    }, {
        "md",
        MD_HL_extensions,
        MD_HL_keywords,
        NULL, "<!--", "-->",
        NULL
    }, {
        "py",
        PY_HL_extensions,
        PY_HL_keywords,
        "#", NULL, NULL,
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    }
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0])) // Length of HLDB array

/*** prototypes ***/

void editorSetStatusMessage(const char* fmt, ...);
void editorRefreshScreen();
char* editorPrompt(char* promt, void (*callback)(char*, int));

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

/** syntax highlighting ***/

int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow* row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if (E.syntax == NULL) return;

    char** keywords = E.syntax->keywords;

    char* scs = E.syntax->singleline_comment_start;
    char* mcs = E.syntax->multiline_comment_start;
    char* mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
      int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while(i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i-1] : HL_NORMAL;

        /* If single line comments */
        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->size - i);
                break;
            }
        }

        /* If multiline comments */
        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        /* If String syntax flag set, highlight strings & characters */
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;

                if (c == '\\' && i + 1 < row->size) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }

                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        /* If Number syntax flag set, highlight numbers */
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if(isdigit(c) && (prev_sep || prev_hl == HL_NUMBER || (c == '.' && prev_hl == HL_NUMBER))) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep;
                continue;
            }
        }

        if(prev_sep) {
            int j;
            for(j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen-1] == '|';
                if (kw2) klen--;

                if (!strncmp(&row->render[i], keywords[j], klen) && is_separator(row->render[i+klen])) {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }

            if(keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx + 1]); 
}

/* 31 = */
int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_MLCOMMENT:
        case HL_COMMENT: return 35;// MAGENTA
        case HL_KEYWORD1: return 31; // RED
        case HL_KEYWORD2: return 34; // BLUE
        case HL_NUMBER: return 36; // CYAN
        case HL_STRING: return 32; // GREEN
        case HL_MATCH: return 33;  // YELLOW
        default: return 37;        // WHITE
    }
}

void editorSelectSyntaxHighlight() {
    E.syntax = NULL;
    if(E.filename == NULL) return;

    char* ext = strrchr(E.filename, '.');

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax* s = &HLDB[j];
        unsigned int i = 0;
        while(s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            if((is_ext && ext && !strcmp(ext, s->filematch[i])) || (!is_ext && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;

                int filerow;
                for (filerow = 0; filerow < E.numrows; filerow++) {
                    editorUpdateSyntax(&E.row[filerow]);
                }

                return;
            }
            i++;
        }
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

int editorRowRxToCx(erow* row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP);
        cur_rx++;

        if (cur_rx > rx) return cx;
    }
    return cx;
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

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char* s, size_t len) {
    if(at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;
    
    E.row[at].idx = at;

    E.row[at].size = len;
    E.row[at].chars = malloc(len+1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow* row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
    E.numrows--;
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
    E.dirty++;
}

void editorRowInsertString(erow* row, int at, int len, char* cs) {
    if (at < 0 || at > row->size) {
        at = row->size;
    }

    row->chars = realloc(row->chars, row->size + len + 1);
    memmove(&row->chars[at+len], &row->chars[at], row->size - at + 1);
    memcpy(&row->chars[at], cs, len);
    row->size += len;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow* row, char* s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDeleteChar(erow* row, int at) {
    if (at < 0 || at >= row->size) return;

    memmove(&row->chars[at], &row->chars[at+1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
    if(E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline() {
    if(E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow* row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx); // Split the current row in 2. Divide @ cusor position.
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDeleteChar() {
    if(E.cy == E.numrows) return;
    if(E.cx == 0 && E.cy == 0) return;

    erow* row = &E.row[E.cy];
    if(E.cx > 0) {
        editorRowDeleteChar(row, E.cx-1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy-1].size;
        editorRowAppendString(&E.row[E.cy-1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

void editorStartSelecting() {
    editorSetStatusMessage("Selection: Use Arrows | Ctrl-E");
    E.selecting = 1;
    editorDropCursor();
}

void editorStopSelecting() {
    E.selecting = 0;
    E.selection_end_x = 0;
    E.selection_end_y = 0;
    E.selection_start_x = 0;
    E.selection_start_y = 0;
}

int editorCollectSelection() {
    if(E.selecting) {
        int sel_start_y = E.dropped_cursor_y, sel_end_y = E.cy;
        int sel_start_x = E.dropped_cursor_x, sel_end_x = E.cx;

        if(sel_start_y > sel_end_y || (sel_start_y == sel_end_y && sel_start_x > sel_end_x)) {
            sel_start_x = sel_end_x; sel_start_y = sel_end_y;
            sel_end_x = E.dropped_cursor_x; sel_end_y = E.dropped_cursor_y;
        }

        int count = 0;
        if (sel_end_y == sel_start_y) {
            // Same line
            count = sel_end_x - sel_start_x;
        } else {
            // First line
            count += E.row[sel_start_y].size - sel_start_x;

            // Complete lines in range
            for (int i = sel_start_y + 1; i < sel_end_y; i++) {
                count++; // Newline
                count += E.row[i].size;
            }

            // Last line
            count++; // Newline
            count += sel_end_x;
        }

        E.selection_start_x = sel_start_x;
        E.selection_start_y = sel_start_y;
        E.selection_end_y = sel_end_y;
        E.selection_end_x = sel_end_x;

        return count;
    } else {
        return 0;
    }
}

/// @brief copy the selection of characters to a buffer
void editorSelectionCopy() {
    if(E.selecting) {
        int buffer_len = editorCollectSelection();

        // Now iterate through all the characters to copy & append them to buffer
        char* buffer = malloc(buffer_len + 1); // Null terminated string
        int index = 0;

        if (E.selection_end_y == E.selection_start_y) {
            strncpy(buffer, &E.row[E.selection_end_y].chars[E.selection_start_x], buffer_len);
            buffer[buffer_len] = '\0';
        } else {
            strncpy(buffer, &E.row[E.selection_start_y].chars[E.selection_start_x], E.row[E.selection_start_y].size - E.selection_start_x);
            index += E.row[E.selection_start_y].size - E.selection_start_x; // No. Characters from first line

            for (int i = E.selection_start_y + 1; i < E.selection_end_y; i++) {
                buffer[index] = '\n';
                index++;
                strncpy(&buffer[index], E.row[i].chars, E.row[i].size);
                index += E.row[i].size;
            }

            buffer[index] = '\n';
            index++;
            strncpy(&buffer[index], E.row[E.selection_end_y].chars, E.selection_end_x); // Last characters on last line
            buffer[index + E.selection_end_x] = '\0'; // EOS
        }

        if(E.copy_buffer) free(E.copy_buffer);
        E.copy_buffer = buffer;
        E.copy_buffer_len = buffer_len;
        editorSetStatusMessage("Copied %d characters", buffer_len);

    } else {
        // Display message.
        editorSetStatusMessage("Copy failed: No selection (Ctrl-E & Arrow Keys)");
    }

    editorStopSelecting(); // Once copied, no need to be selecting anymore
}

/// @brief Paste characters from editor buffer
void editorPaste() {
    if (E.copy_buffer) {
        if(E.cy == E.numrows) {
            editorInsertRow(E.numrows, "", 0);
        }

        for (int i = 0; i < E.copy_buffer_len; i++) {
            if(E.copy_buffer[i] == '\n') {
                editorInsertNewline();
            } else {
                editorInsertChar(E.copy_buffer[i]);
            }
        }

        editorSetStatusMessage("Pasted %d characters @ %d,%d", E.copy_buffer_len, E.cx, E.cy);
    } else {
        editorSetStatusMessage("Paste failed: Copy buffer empty");
    }
}

/// @brief Delete a selection of multiple characters
void editorSelectionDelete() {
    int numCharsToDelete = editorCollectSelection();
    E.cx = E.selection_end_x;
    E.cy = E.selection_end_y;

    for(int i = 0; i < numCharsToDelete; i++) {
        editorDeleteChar();
    }
    editorStopSelecting();
}

void editorSelectionIndent() {
    //Tab on selection to mass-indent
    editorCollectSelection();
    editorRowInsertChar(&E.row[E.selection_start_y], E.selection_start_x, '\t');

    for(int i = 1; i <= E.selection_end_y - E.selection_start_y; i++)
    {
        editorRowInsertChar(&E.row[E.selection_start_y + i], E.selection_start_x, '\t');
    }
}

void editorSelectionUnindent() {
    editorCollectSelection();

    int first_indent = E.selection_start_x == 0 ? 0 : E.selection_start_x - 1;
    if(E.row[E.selection_start_y].chars[first_indent] == '\t') {
        editorRowDeleteChar(&E.row[E.selection_start_y], first_indent);
    }

    for(int i = 1; i <= E.selection_end_y - E.selection_start_y; i++)
    {
        if(E.row[E.selection_start_y + i].chars[0] == '\t') {
            editorRowDeleteChar(&E.row[E.selection_start_y + i], 0);
        }
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

    editorSelectSyntaxHighlight();

    FILE* fp = fopen(filename, "r");
    if(!fp) fail("fopen");

    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    if(linelen != -1)
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("No filename given. Save aborted.");
            return;
        }
    }

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
            editorSelectSyntaxHighlight();
        }
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Write failed. IO error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char* query, int key) {
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static char* saved_hl = NULL;

    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == RIGHT || key == DOWN) {
        direction = 1;
    } else if (key == LEFT || key == UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int current = last_match;

    int i;
    for (i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;

        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);

            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind() {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char* query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

    if (query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
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
    if (E.rx >= E.coloff + (E.screencols - MARGIN)) {
        E.coloff = E.rx - (E.screencols - MARGIN) + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;

        if(filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80]; // Welcome message buffer
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
                abAppend(ab, "~", 1); // Prefix for unused line
            }
        } else { // The line is in the used section of the editor.
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > (E.screencols - MARGIN)) len = (E.screencols - MARGIN);
            
            char* c = &E.row[filerow].render[E.coloff];
            unsigned char* hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;

            // margin line numbers
            char margin[7];
            margin[6] = '\0'; // Null terminate
            sprintf(margin, "%4d| ", filerow); // padding
            abAppend(ab, margin, 6);

            int j;
            for(j = 0; j < len; j++) {
                // Selection
                if(E.selecting) {
                    // editorSetStatusMessage("Selection: %d/%d -> %d/%d", E.selection_start_x, E.selection_start_y, E.selection_end_x, E.selection_end_y);
                    
                    // Check if current character is within selection bounds
                    if((filerow > E.selection_start_y || (filerow == E.selection_start_y && j >= E.selection_start_x)) &&
                    (filerow < E.selection_end_y || (filerow == E.selection_end_y && j < E.selection_end_x))) {
                        abAppend(ab, "\033[43m", 5);
                    } else if (filerow == E.selection_end_y && j == E.selection_end_x) {
                        abAppend(ab, "\033[0m", 4); // Final character in selection resets highlighting. (Should come after the character)
                    }
                }

                if (iscntrl(c[j])) {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                } else if (hl[j] == HL_NORMAL) {
                    if(current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\033[0m", 4); // Reset Highlighting
            abAppend(ab, "\x1b[39m", 5); // Reset Colour
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
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
        E.syntax ? E.syntax->filetype : ".?", E.cy + 1, E.numrows);
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
    abAppend(&ab, "\x1b[H", 3); // Position the cursor to home: (3 bytes)

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1 + MARGIN); // Cursor position
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

char* editorPrompt(char* promt, void (*callback)(char*, int)){
    size_t bufsize = 128;
    char* buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1) {
        editorSetStatusMessage(promt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
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

        if (callback) callback(buf, c);
    }
}

void editorDropCursor() {
    E.dropped_cursor_x = E.cx;
    E.dropped_cursor_y = E.cy;
}

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

    if (E.selecting) {
        editorCollectSelection();
    }
}

/// @brief Awaits keypress, then handles it.
void editorHandleKeyPress() {
    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;

        case CTRL_KEY('q'):
            if (E.copy_buffer != NULL) {free(E.copy_buffer);}
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDERR_FILENO, "\x1b[H", 3);

            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case CTRL_KEY('e'):
            if(E.selecting) {
                editorStopSelecting();
            } else {
                editorStartSelecting();
            }
            break;

        case CTRL_KEY('c'):
            editorSelectionCopy();
            break;

        case CTRL_KEY('v'):
            if(E.selecting) {
                editorSelectionDelete();
            }

            editorPaste();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL:
            if(E.selecting) {
                editorSelectionDelete();
            } else {
                if(c == DEL) editorMoveCursor(RIGHT);
                editorDeleteChar();
            }
            break;

        case P_UP:
        case P_DOWN:
            if (c == P_UP) {
                E.cy = E.rowoff;
            } else if (c == P_DOWN) {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > E.numrows) E.cy = E.numrows;
            }

            int times = E.screenrows;
            while (times--)
                editorMoveCursor(c == P_UP ? UP : DOWN);

            if (E.selecting) {editorCollectSelection();}
            break;
            

        case UP:
        case DOWN:
        case LEFT:
        case RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'): // We already refresh
        case '\x1b':        // Ignoring Escape Key
            break;

        // Fallthroughs to write char
        case '\t':
            if(E.selecting) {
                editorSetStatusMessage("Selection shift: <- U | I ->");
                editorRefreshScreen(); // Display message

                char tab_dir = editorReadKey();
                switch (tab_dir)
                {
                    case 'I':
                    case 'i':
                        editorSelectionIndent();
                        break;
                    
                    case 'U':
                    case 'u':
                        editorSelectionUnindent();
                        break;

                    default:
                        editorSetStatusMessage("Invalid selection shift direction: %c", tab_dir); 
                        break;
                }
            } else {
                editorInsertChar(c);
            }
            break;

        default:
            if(E.selecting) {
                // Replace a selection with the new characters!
                editorSelectionDelete();
            }

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
    E.syntax = NULL;

    E.dropped_cursor_x = 0;
    E.dropped_cursor_y = 0;
    E.selection_start_x = 0;
    E.selection_start_y = 0;
    E.selection_end_x = 0;
    E.selection_end_y = 0;
    E.selecting = 0;
    E.copy_buffer = NULL;
    E.copy_buffer_len = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) fail("getWindowSize");
    E.screenrows-=2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();

    if(argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-F = find | Ctrl-Q = quit");

    while(1) {
        editorRefreshScreen();
        editorHandleKeyPress();
    }

    return 0;
}
