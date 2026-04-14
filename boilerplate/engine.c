#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <signal.h>
#include <time.h>

#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 10

typedef struct {
    char id[32];
    pid_t host_pid;
    char state[16];
    char rootfs[256];
    time_t start_time;
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;

// Function running INSIDE the container
int container_main(void *arg) {
    container_t *config = (container_t *)arg;
    sethostname(config->id, strlen(config->id));

    // Filesystem isolation
    if (chroot(config->rootfs) != 0 || chdir("/") != 0) {
        perror("chroot failed");
        return 1;
    }

    // Mount /proc so 'ps' works
    mount("proc", "/proc", "proc", 0, NULL);

    char *const child_args[] = {"/bin/sh", NULL};
    execvp(child_args[0], child_args);
    return 0;
}

// Reap zombies
void handle_sigchld(int sig) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < container_count; i++) {
            if (containers[i].host_pid == pid) {
                strcpy(containers[i].state, "stopped");
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s supervisor <base-rootfs>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        signal(SIGCHLD, handle_sigchld);
        printf("[Supervisor] Running. Starting 'alpha' test container...\n");

        // Temporary Task 1 manual start
        container_t *c = &containers[container_count++];
        strcpy(c->id, "alpha");
        strcpy(c->rootfs, "./rootfs-alpha");
        strcpy(c->state, "running");

        char *stack = malloc(STACK_SIZE);
        clone(container_main, stack + STACK_SIZE, 
              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, c);

        while (1) pause();
    }
    return 0;
}
