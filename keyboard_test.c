#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>

static struct termios original_termios;
static int terminal_configured = 0;
static int running = 1;

static void configure_terminal(void) {
    if (tcgetattr(STDIN_FILENO, &original_termios) == -1) {
        return;
    }
    
    struct termios raw = original_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
        terminal_configured = 1;
    }
}

static void restore_terminal(void) {
    if (terminal_configured) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
        terminal_configured = 0;
    }
}

static char check_keyboard(void) {
    if (!terminal_configured) return 0;
    
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        return c;
    }
    return 0;
}

static void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    running = 0;
    restore_terminal();
}

int main() {
    printf("Keyboard test - press 'q' to quit, Ctrl+C also works...\n");
    
    signal(SIGINT, signal_handler);
    configure_terminal();
    
    while (running) {
        char key = check_keyboard();
        if (key == 'q' || key == 'Q' || key == 27) {
            printf("Quit key pressed\n");
            break;
        }
        if (key && key != 'q' && key != 'Q') {
            printf("Key pressed: %c (%d)\n", key, key);
        }
        usleep(50000);
    }
    
    restore_terminal();
    printf("Test complete!\n");
    return 0;
}
