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
#include <sys/resource.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include "logger.h"
#include "monitor_ioctl.h"

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
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_val;
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
        int i=0, line_start=0;
        for(; i<n; i++){
            if(read_buf[i] == '\n' || read_buf[i] == '\0'){
                char line[LOG_MSG_SIZE];
                int len = i - line_start + 1;
                if(len > LOG_MSG_SIZE - 1) len = LOG_MSG_SIZE - 1;
                strncpy(line, &read_buf[line_start], len);
                line[len] = '\0';
                push_log(args->container_id, line);
                line_start = i + 1;
            }
        }
        if(line_start < n) {
            char line[LOG_MSG_SIZE] = {0};
            strncpy(line, &read_buf[line_start], n - line_start);
            push_log(args->container_id, line);
        }
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
    
    dup2(config->pipe_fd, 1);
    dup2(config->pipe_fd, 2);
    close(config->pipe_fd);

    if (config->nice_val != 0) {
        setpriority(PRIO_PROCESS, 0, config->nice_val);
    }

    if (chroot(config->rootfs) != 0 || chdir("/") != 0) {
        return 1;
    }
    mount("proc", "/proc", "proc", 0, NULL);
    
    // Command comes with quotes sometimes, just simple execlp/sh -c
    // Let sh handle split if it was provided as string
    execl("/bin/sh", "sh", "-c", config->command, NULL);
    return 1;
}

void register_kernel_monitor(pid_t pid, unsigned long soft, unsigned long hard, const char* id) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if(fd >= 0) {
        struct monitor_request req;
        req.pid = pid;
        req.soft_limit_bytes = soft;
        req.hard_limit_bytes = hard;
        strncpy(req.container_id, id, MONITOR_NAME_LEN-1);
        req.container_id[MONITOR_NAME_LEN-1] = '\0';
        ioctl(fd, MONITOR_REGISTER, &req);
        close(fd);
    }
}

void unregister_kernel_monitor(pid_t pid) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if(fd >= 0) {
        struct monitor_request req;
        req.pid = pid;
        ioctl(fd, MONITOR_UNREGISTER, &req);
        close(fd);
    }
}

void start_container(int client_fd, const char *id, const char *rootfs, const char *cmd, unsigned long soft_mib, unsigned long hard_mib, int nice_val, int is_run) {
    // Find free slot
    int slot = -1;
    for(int i=0; i<MAX_CONTAINERS; i++){
        if(container_count < MAX_CONTAINERS && i == container_count) { slot = container_count; container_count++; break; }
        if(strcmp(containers[i].state, "stopped") == 0 || strcmp(containers[i].state, "killed") == 0 || containers[i].state[0]=='\0') { slot = i; break; }
    }
    if (slot == -1) return;

    int pipefd[2];
    pipe(pipefd);

    container_t *c = &containers[slot];
    strncpy(c->id, id, 31);
    strncpy(c->rootfs, rootfs, 255);
    strncpy(c->command, cmd, 255);
    c->soft_limit_bytes = soft_mib * 1024 * 1024;
    c->hard_limit_bytes = hard_mib * 1024 * 1024;
    c->nice_val = nice_val;
    c->pipe_fd = pipefd[1];
    strcpy(c->state, "running");
    
    char *stack = malloc(STACK_SIZE);
    pid_t pid = clone(container_main, stack + STACK_SIZE, 
                    CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, c);

    if (pid > 0) {
        c->host_pid = pid;
        close(pipefd[1]);

        register_kernel_monitor(pid, c->soft_limit_bytes, c->hard_limit_bytes, c->id);

        proxy_args_t *p_args = malloc(sizeof(proxy_args_t));
        p_args->pipe_fd = pipefd[0];
        strncpy(p_args->container_id, id, 31);
        pthread_t tid;
        pthread_create(&tid, NULL, container_proxy_thread, p_args);
        pthread_detach(tid);

        if(!is_run) {
            char reply[128];
            sprintf(reply, "[OK] Started %s\n", id);
            write(client_fd, reply, strlen(reply));
        } else {
            int status;
            waitpid(pid, &status, 0);
            strcpy(c->state, "stopped");
            unregister_kernel_monitor(pid);
            char reply[128];
            int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status));
            sprintf(reply, "Container exited with status %d\n", exit_code);
            write(client_fd, reply, strlen(reply));
        }
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
            if (containers[i].host_pid == pid) {
                strcpy(containers[i].state, WIFSIGNALED(status) ? "killed" : "stopped");
                unregister_kernel_monitor(pid);
            }
        }
    }
}

void handle_sigint(int sig) {
    (void)sig;
    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].state, "running") == 0) kill(containers[i].host_pid, SIGKILL);
    }
    unlink(SOCKET_PATH);
    shm_unlink(SHM_NAME);
    exit(0);
}

void parse_args_start_run(char *buffer, char *id, char *rfs, char *cmd, unsigned long *s, unsigned long *h, int *n) {
    *s = 40; *h = 64; *n = 0; // Default configs
    char* arg = strtok(buffer, " ");
    arg = strtok(NULL, " "); strcpy(id, arg ? arg : "");
    arg = strtok(NULL, " "); strcpy(rfs, arg ? arg : "");

    // The remaining part might have commands inside quotes.
    // Basic parser for demonstration purposes:
    char raw_cmd[256] = {0};
    int parsing_cmd = 0;
    while((arg = strtok(NULL, " ")) != NULL) {
        if(strcmp(arg, "--soft-mib") == 0) *s = atoi(strtok(NULL, " "));
        else if(strcmp(arg, "--hard-mib") == 0) *h = atoi(strtok(NULL, " "));
        else if(strcmp(arg, "--nice") == 0) *n = atoi(strtok(NULL, " "));
        else {
            if(!parsing_cmd) {
                // If it starts with quote, remove it to match exactly
                if(arg[0] == '"') arg++;
                parsing_cmd = 1;
            }
            strcat(raw_cmd, arg);
            strcat(raw_cmd, " ");
        }
    }
    // Remove trailing quote/space
    int len = strlen(raw_cmd);
    if(len > 0) {
        if(raw_cmd[len-2] == '"') raw_cmd[len-2] = '\0';
        else raw_cmd[len-1] = '\0';
    }
    strcpy(cmd, raw_cmd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0) {
        signal(SIGCHLD, handle_sigchld);
        signal(SIGINT, handle_sigint);
        signal(SIGTERM, handle_sigint);
        
        init_log_buffer(); 
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
                    if(containers[i].state[0] == '\0') continue;
                    char line[128];
                    sprintf(line, "%s\t%d\t%s\n", containers[i].id, containers[i].host_pid, containers[i].state);
                    strcat(reply, line);
                }
                write(client_fd, reply, strlen(reply));
            } else if (strncmp(buffer, "start", 5) == 0 || strncmp(buffer, "run", 3) == 0) {
                char id[32], rfs[256], cmd[256];
                unsigned long s, h; int n;
                int is_run = (strncmp(buffer, "run", 3) == 0);
                parse_args_start_run(buffer, id, rfs, cmd, &s, &h, &n);
                start_container(client_fd, id, rfs, cmd, s, h, n, is_run);
            } else if (strncmp(buffer, "stop", 4) == 0) {
                char id[32];
                sscanf(buffer, "%*s %s", id);
                for (int i = 0; i < container_count; i++) {
                    if (strcmp(containers[i].id, id) == 0 && strcmp(containers[i].state, "running") == 0) {
                        kill(containers[i].host_pid, SIGKILL);
                        sprintf(reply, "[OK] Stopped %s\n", id);
                    }
                }
                write(client_fd, reply, strlen(reply));
            } else if (strncmp(buffer, "logs", 4) == 0) {
                char id[32], filename[64];
                sscanf(buffer, "%*s %s", id);
                snprintf(filename, sizeof(filename), "container_%s.log", id);
                FILE *log_file = fopen(filename, "r");
                if (log_file) {
                    char line[256];
                    while(fgets(line, sizeof(line), log_file)) {
                        write(client_fd, line, strlen(line));
                    }
                    fclose(log_file);
                } else {
                    write(client_fd, "[Error] No logs found\n", 22);
                }
            }
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
        int n;
        while((n = read(sock, resp, sizeof(resp)-1)) > 0) {
            resp[n] = '\0';
            printf("%s", resp);
        }
        close(sock);
    }
    return 0;
}
