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
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include "logger.h"

#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 10
#define SOCKET_PATH "/tmp/jackfruit.sock"

typedef struct {
    char id[32];
    pid_t host_pid;
    char state[16];
    char rootfs[256];
    char command[256];
    int pipe_fd;
    time_t start_time;
} container_t;

typedef struct {
    int pipe_fd;
    char container_id[32];
} proxy_args_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;
ring_buffer_t *shared_log_buffer = NULL;

// ==========================================
// LOGGING SYSTEM (PRODUCER / CONSUMER)
// ==========================================

void init_log_buffer() {
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, sizeof(ring_buffer_t));
    shared_log_buffer = (ring_buffer_t *)mmap(NULL, sizeof(ring_buffer_t),
                                            PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    memset(shared_log_buffer, 0, sizeof(ring_buffer_t));

    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shared_log_buffer->lock, &mutex_attr);

    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&shared_log_buffer->not_empty, &cond_attr);
    pthread_cond_init(&shared_log_buffer->not_full, &cond_attr);
}

void push_log(const char *container_id, const char *msg) {
    if (!shared_log_buffer) return;
    pthread_mutex_lock(&shared_log_buffer->lock);
    while (shared_log_buffer->count == MAX_LOGS) {
        pthread_cond_wait(&shared_log_buffer->not_full, &shared_log_buffer->lock);
    }
    log_entry_t *entry = &shared_log_buffer->buffer[shared_log_buffer->tail];
    strncpy(entry->container_id, container_id, 31);
    strncpy(entry->message, msg, LOG_MSG_SIZE - 1);
    shared_log_buffer->tail = (shared_log_buffer->tail + 1) % MAX_LOGS;
    shared_log_buffer->count++;
    pthread_cond_signal(&shared_log_buffer->not_empty);
    pthread_mutex_unlock(&shared_log_buffer->lock);
}

void *logger_thread(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&shared_log_buffer->lock);
        while (shared_log_buffer->count == 0) {
            pthread_cond_wait(&shared_log_buffer->not_empty, &shared_log_buffer->lock);
        }
        log_entry_t *entry = &shared_log_buffer->buffer[shared_log_buffer->head];
        char filename[64];
        snprintf(filename, sizeof(filename), "container_%s.log", entry->container_id);
        FILE *log_file = fopen(filename, "a");
        if (log_file) {
            fprintf(log_file, "[%ld] %s", time(NULL), entry->message);
            fclose(log_file);
        }
        shared_log_buffer->head = (shared_log_buffer->head + 1) % MAX_LOGS;
        shared_log_buffer->count--;
        pthread_cond_signal(&shared_log_buffer->not_full);
        pthread_mutex_unlock(&shared_log_buffer->lock);
    }
    return NULL;
}

void *container_proxy_thread(void *arg) {
    proxy_args_t *args = (proxy_args_t *)arg;
    char read_buf[LOG_MSG_SIZE];
    while (1) {
        ssize_t n = read(args->pipe_fd, read_buf, sizeof(read_buf) - 1);
        if (n <= 0) break; 
        read_buf[n] = '\0';
        push_log(args->container_id, read_buf);
    }
    close(args->pipe_fd);
    free(args);
    return NULL;
}

// ==========================================
// CONTAINER CORE
// ==========================================

int container_main(void *arg) {
    container_t *config = (container_t *)arg;
    sethostname(config->id, strlen(config->id));
    
    // Task 3: Redirect output to the supervisor's pipe
    dup2(config->pipe_fd, 1);
    dup2(config->pipe_fd, 2);
    close(config->pipe_fd);

    if (chroot(config->rootfs) != 0 || chdir("/") != 0) {
        perror("chroot failed");
        return 1;
    }
    mount("proc", "/proc", "proc", 0, NULL);
    char *const child_args[] = {"/bin/sh", "-c", config->command, NULL};
    execvp(child_args[0], child_args);
    return 1;
}

void start_container(const char *id, const char *rootfs, const char *cmd) {
    if (container_count >= MAX_CONTAINERS) return;

    int pipefd[2];
    pipe(pipefd); // Task 3: Create the logging pipe

    container_t *c = &containers[container_count];
    strncpy(c->id, id, 31);
    strncpy(c->rootfs, rootfs, 255);
    strncpy(c->command, cmd, 255);
    c->pipe_fd = pipefd[1];
    strcpy(c->state, "running");
    
    char *stack = malloc(STACK_SIZE);
    pid_t pid = clone(container_main, stack + STACK_SIZE, 
                    CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, c);

    if (pid > 0) {
        c->host_pid = pid;
        container_count++;
        close(pipefd[1]); // Supervisor closes write end

        proxy_args_t *p_args = malloc(sizeof(proxy_args_t));
        p_args->pipe_fd = pipefd[0];
        strncpy(p_args->container_id, id, 31);
        pthread_t tid;
        pthread_create(&tid, NULL, container_proxy_thread, p_args);
        pthread_detach(tid);
    }
}

// ==========================================
// SUPERVISOR CONTROL
// ==========================================

void handle_sigchld(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < container_count; i++) {
            if (containers[i].host_pid == pid) strcpy(containers[i].state, "stopped");
        }
    }
}

void handle_sigint(int sig) {
    (void)sig;
    printf("\n[Supervisor] Shutting down...\n");
    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].state, "running") == 0) kill(containers[i].host_pid, SIGKILL);
    }
    unlink(SOCKET_PATH);
    shm_unlink(SHM_NAME);
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0) {
        signal(SIGCHLD, handle_sigchld);
        signal(SIGINT, handle_sigint);
        
        init_log_buffer(); // Task 3: Shared Memory
        pthread_t log_tid;
        pthread_create(&log_tid, NULL, logger_thread, NULL);
        pthread_detach(log_tid);

        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
        unlink(SOCKET_PATH);
        bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
        listen(server_fd, 5);
        printf("[Supervisor] Running. Listening on %s...\n", SOCKET_PATH);

        while (1) {
            int client_fd = accept(server_fd, NULL, NULL);
            char buffer[1024] = {0}, reply[4096] = {0};
            if (read(client_fd, buffer, sizeof(buffer)) <= 0) { close(client_fd); continue; }
            
            if (strncmp(buffer, "ps", 2) == 0) {
                sprintf(reply, "ID\tPID\tSTATE\n");
                for(int i=0; i<container_count; i++) {
                    char line[128];
                    sprintf(line, "%s\t%d\t%s\n", containers[i].id, containers[i].host_pid, containers[i].state);
                    strcat(reply, line);
                }
            } else if (strncmp(buffer, "start", 5) == 0) {
                char req[16], id[32], rfs[256]; int off = 0;
                if (sscanf(buffer, "%s %s %s %n", req, id, rfs, &off) >= 3) {
                    start_container(id, rfs, buffer + off);
                    sprintf(reply, "[OK] Started %s", id);
                }
            } else if (strncmp(buffer, "stop", 4) == 0) {
                char req[16], id[32];
                sscanf(buffer, "%s %s", req, id);
                for (int i = 0; i < container_count; i++) {
                    if (strcmp(containers[i].id, id) == 0) {
                        kill(containers[i].host_pid, SIGKILL);
                        sprintf(reply, "[OK] Stopped %s", id);
                    }
                }
            }
            write(client_fd, reply, strlen(reply));
            close(client_fd);
        }
    } else {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
        connect(sock, (struct sockaddr *)&addr, sizeof(addr));
        char cmd[1024] = {0};
        for (int i = 1; i < argc; i++) { strcat(cmd, argv[i]); strcat(cmd, " "); }
        write(sock, cmd, strlen(cmd));
        char resp[4096] = {0};
        read(sock, resp, sizeof(resp));
        printf("%s\n", resp);
        close(sock);
    }
    return 0;
}
