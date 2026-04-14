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
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 10
#define SOCKET_PATH "/tmp/jackfruit.sock"

typedef struct {
    char id[32];
    pid_t host_pid;
    char state[16];
    char rootfs[256];
    char command[256]; // <--- NEW: Stores the actual command to run
    time_t start_time;
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;

// ==========================================
// CONTAINER EXECUTION
// ==========================================
int container_main(void *arg) {
    container_t *config = (container_t *)arg;
    
    if (sethostname(config->id, strlen(config->id)) != 0) {
        perror("sethostname failed");
    }

    if (chroot(config->rootfs) != 0 || chdir("/") != 0) {
        perror("chroot failed");
        return 1;
    }

    mount("proc", "/proc", "proc", 0, NULL);

    // THE FIX: Execute the specific command passed from the CLI
    char *const child_args[] = {"/bin/sh", "-c", config->command, NULL};
    execvp(child_args[0], child_args);
    
    perror("execvp failed");
    return 1;
}

// ==========================================
// SIGNAL HANDLERS
// ==========================================
void handle_sigchld(int sig) {
    (void)sig; // Silences the unused parameter warning
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

void handle_sigint(int sig) {
    (void)sig; // Silences the unused parameter warning
    printf("\n[Supervisor] Caught shutdown signal. Terminating containers...\n");
    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].state, "running") == 0) {
            kill(containers[i].host_pid, SIGKILL);
        }
    }
    unlink(SOCKET_PATH);
    printf("[Supervisor] Teardown complete. Exiting.\n");
    exit(0);
}

// ==========================================
// CONTAINER LAUNCHER
// ==========================================
void start_container(const char *id, const char *rootfs, const char *cmd) {
    if (container_count >= MAX_CONTAINERS) return;

    container_t *c = &containers[container_count];
    strncpy(c->id, id, 31);
    strncpy(c->rootfs, rootfs, 255);
    strncpy(c->command, cmd, 255);
    strcpy(c->state, "running");
    c->start_time = time(NULL);

    char *stack = malloc(STACK_SIZE);
    pid_t pid = clone(container_main, stack + STACK_SIZE, 
                    CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, c);

    if (pid > 0) {
        c->host_pid = pid;
        container_count++;
    } else {
        free(stack);
    }
}

// ==========================================
// MAIN ENTRY POINT
// ==========================================
int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s supervisor <base-rootfs> OR %s <cmd> ...\n", argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        signal(SIGCHLD, handle_sigchld);
        signal(SIGINT, handle_sigint);

        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        unlink(SOCKET_PATH); 
        if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind failed");
            return 1;
        }
        listen(server_fd, 5);
        printf("[Supervisor] Daemon running. Listening on %s...\n", SOCKET_PATH);

        while (1) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR) continue; 
                break;
            }

            char buffer[1024] = {0};
            char reply[4096] = {0};
            
            if (read(client_fd, buffer, sizeof(buffer)) <= 0) {
                close(client_fd);
                continue;
            }
            
            // --- Parse the CLI Command ---
            if (strncmp(buffer, "ps", 2) == 0) {
                sprintf(reply, "ID\tPID\tSTATE\tROOTFS\n");
                for(int i=0; i<container_count; i++) {
                    char line[512]; 
                    snprintf(line, sizeof(line), "%s\t%d\t%s\t%s\n", 
                            containers[i].id, containers[i].host_pid, 
                            containers[i].state, containers[i].rootfs);
                    strcat(reply, line);
                }
            } 
            else if (strncmp(buffer, "start", 5) == 0) {
                char req_cmd[16], id[32], rootfs[256];
                int offset = 0;
                
                // THE FIX: Properly extract the rest of the string as the command
                if (sscanf(buffer, "%s %s %s %n", req_cmd, id, rootfs, &offset) >= 3) {
                    char *exec_cmd = buffer + offset;
                    while(*exec_cmd == ' ') exec_cmd++; // trim leading spaces
                    
                    if (strlen(exec_cmd) > 0) {
                        start_container(id, rootfs, exec_cmd);
                        snprintf(reply, sizeof(reply), "[OK] Started container '%s' running '%s'", id, exec_cmd);
                    } else {
                        sprintf(reply, "[ERROR] Missing command parameter.");
                    }
                } else {
                    sprintf(reply, "[ERROR] Invalid start command arguments.");
                }
            } 
            else if (strncmp(buffer, "stop", 4) == 0) {
                char req_cmd[16], id[32];
                if (sscanf(buffer, "%s %s", req_cmd, id) >= 2) {
                    int found = 0;
                    for (int i = 0; i < container_count; i++) {
                        if (strcmp(containers[i].id, id) == 0 && strcmp(containers[i].state, "running") == 0) {
                            kill(containers[i].host_pid, SIGKILL);
                            sprintf(reply, "[OK] Sent stop signal to '%s'", id);
                            found = 1;
                            break;
                        }
                    }
                    if (!found) sprintf(reply, "[ERROR] Container '%s' not found or not running.", id);
                } else {
                    sprintf(reply, "[ERROR] Usage: stop <id>");
                }
            }
            else {
                sprintf(reply, "[ERROR] Unknown command.");
            }

            if (write(client_fd, reply, strlen(reply)) < 0) perror("write failed");
            close(client_fd);
        }
        unlink(SOCKET_PATH);
    } 
    else {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "Error: Could not connect to supervisor. Is it running?\n");
            return 1;
        }

        char cmd_buffer[1024] = {0};
        for (int i = 1; i < argc; i++) {
            strcat(cmd_buffer, argv[i]);
            if (i < argc - 1) strcat(cmd_buffer, " ");
        }

        if (write(sock, cmd_buffer, strlen(cmd_buffer)) < 0) perror("write failed");
        
        char response[4096] = {0};
        if (read(sock, response, sizeof(response)) < 0) perror("read failed");
        printf("%s\n", response);

        close(sock);
    }
    return 0;
}
