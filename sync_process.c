#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>

#define FILE_NAME "counter.txt"
#define MAX_COUNT 10

/* Atomic flag for signal handling */
static volatile sig_atomic_t g_sig_received = 0;

/**
 * @brief Signal handler for synchronization signal.
 * @param signo Signal number received.
 */
static void sig_handler(int signo) {
    (void)signo;
    g_sig_received = 1;
}

/**
 * @brief Wait for the synchronization signal safely using sigsuspend.
 * @param wait_mask Signal mask with unblocked target signal.
 */
static void wait_for_signal(const sigset_t *wait_mask) {
    while (!g_sig_received) {
        sigsuspend(wait_mask);
    }
    g_sig_received = 0;
}

/**
 * @brief Read the last counter value from the file.
 * @return The integer counter value, or 0 if file is empty.
 */
static int read_last_counter(void) {
    FILE *fp = fopen(FILE_NAME, "r");
    if (!fp) {
        return 0;
    }

    char line[256];
    char last_line[256] = "";
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strlen(line) > 1) {
            strncpy(last_line, line, sizeof(last_line) - 1);
        }
    }
    fclose(fp);

    if (strlen(last_line) == 0) {
        return 0;
    }

    /* Expected line format: "PID: <pid> | Counter: <counter>" */
    int pid = 0;
    int counter = 0;
    if (sscanf(last_line, "PID: %d | Counter: %d", &pid, &counter) == 2) {
        return counter;
    }

    return 0;
}

/**
 * @brief Append a new record containing process PID and incremented counter.
 * @param current_pid Process ID performing the write.
 * @param new_counter New counter value to append.
 */
static void append_record(pid_t current_pid, int new_counter) {
    FILE *fp = fopen(FILE_NAME, "a");
    if (!fp) {
        perror("Failed to open file for appending");
        exit(EXIT_FAILURE);
    }

    fprintf(fp, "PID: %d | Counter: %d\n", (int)current_pid, new_counter);
    fflush(fp);
    fclose(fp);
}

int main(void) {
    /* Initialize file with clean state */
    FILE *init_fp = fopen(FILE_NAME, "w");
    if (!init_fp) {
        perror("Failed to initialize file");
        return EXIT_FAILURE;
    }
    fprintf(init_fp, "PID: %d | Counter: %d\n", (int)getpid(), 0);
    fclose(init_fp);

    printf("==================================================\n");
    printf("   PROCESS SYNCHRONIZATION VIA SIGNALS (SIGUSR1)  \n");
    printf("==================================================\n");
    printf("Shared File: %s | Max Counter: %d\n\n", FILE_NAME, MAX_COUNT);

    /* Setup signal handling and block SIGUSR1 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        perror("sigaction error");
        return EXIT_FAILURE;
    }

    sigset_t block_mask, wait_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGUSR1);

    /* Block SIGUSR1 to prevent race conditions */
    if (sigprocmask(SIG_BLOCK, &block_mask, &wait_mask) < 0) {
        perror("sigprocmask error");
        return EXIT_FAILURE;
    }

    /* Mask used in sigsuspend: unblock SIGUSR1 */
    sigdelset(&wait_mask, SIGUSR1);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork error");
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        /* ------------------------------------------------------------- */
        /* CHILD PROCESS (Process B)                                     */
        /* ------------------------------------------------------------- */
        pid_t self = getpid();
        pid_t parent = getppid();

        while (1) {
            /* Child waits for Parent's signal */
            wait_for_signal(&wait_mask);

            int last_cnt = read_last_counter();
            if (last_cnt >= MAX_COUNT) {
                /* Notify parent to finish and exit */
                kill(parent, SIGUSR1);
                break;
            }

            int new_cnt = last_cnt + 1;
            append_record(self, new_cnt);
            printf("[Child B  | PID: %d] Read counter: %d -> Wrote counter: %d\n",
                   (int)self, last_cnt, new_cnt);

            /* Signal Parent (Process A) to proceed */
            kill(parent, SIGUSR1);

            if (new_cnt >= MAX_COUNT) {
                break;
            }
        }

        exit(EXIT_SUCCESS);
    } else {
        /* ------------------------------------------------------------- */
        /* PARENT PROCESS (Process A)                                    */
        /* ------------------------------------------------------------- */
        pid_t self = getpid();
        pid_t child = pid;

        while (1) {
            int last_cnt = read_last_counter();
            if (last_cnt >= MAX_COUNT) {
                kill(child, SIGUSR1);
                break;
            }

            int new_cnt = last_cnt + 1;
            append_record(self, new_cnt);
            printf("[Parent A | PID: %d] Read counter: %d -> Wrote counter: %d\n",
                   (int)self, last_cnt, new_cnt);

            /* Signal Child (Process B) to proceed */
            kill(child, SIGUSR1);

            if (new_cnt >= MAX_COUNT) {
                break;
            }

            /* Parent waits for Child's signal */
            wait_for_signal(&wait_mask);
        }

        /* Wait for child process termination */
        wait(NULL);

        printf("\nSynchronization completed successfully.\n");
        printf("--------------------------------------------------\n");
        printf("Final content of %s:\n", FILE_NAME);
        printf("--------------------------------------------------\n");

        FILE *final_fp = fopen(FILE_NAME, "r");
        if (final_fp) {
            char buffer[256];
            while (fgets(buffer, sizeof(buffer), final_fp)) {
                printf("%s", buffer);
            }
            fclose(final_fp);
        }
        printf("--------------------------------------------------\n");
    }

    return EXIT_SUCCESS;
}