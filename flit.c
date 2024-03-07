/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/

struct termios old_termios;

/*** terminal ***/

/// @brief Program exits with error. 
/// @param s error message
void fail(const char* s) {
    perror(s); // Print error message
    exit(1);
}

/// @brief Disable Raw Terminal Mode
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios) == -1){
        fail("tcsetattr");
    }
}

/// @brief Enable Raw Terminal Mode
void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &old_termios) == -1) fail("tcgetattr");
    atexit(disableRawMode); // Restore the terminal attributes upon exit

    struct termios raw = old_termios;

    // NB the &= corresponds to AND then assign. Similar to *=
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // Disable input for control characters.
    raw.c_oflag &= ~(OPOST); // Disable output processing
    raw.c_cflag |= (CS8); // Character size: 8 bits per byte
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN); // Disabling echo, canon' input, signals, etc. NOT the bitflags
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) fail("tcsetattr"); // Apply change after pending output written to terminal
}



int main() {
    enableRawMode();

    while(1) {
        // Character read from keyboard input
        char c = "\0";

        // While input is not 'quit'
        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) fail("read");
        if (iscntrl(c)) { // Catch control characters
            printf("%d\r\n", c);
        } else {
            printf("%d ('%c')\r\n", c, c);
        }

        if (c == 'q') {
            break;
        }
    }

    return 0;
}