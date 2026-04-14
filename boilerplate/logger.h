#ifndef LOGGER_H
#define LOGGER_H

#include <pthread.h>

#define SHM_NAME "/jackfruit_logger"
#define MAX_LOGS 100
#define LOG_MSG_SIZE 256

// A single log entry
typedef struct {
    char container_id[32];
    char message[LOG_MSG_SIZE];
} log_entry_t;

// The Shared Ring Buffer (Bounded Buffer)
typedef struct {
    log_entry_t buffer[MAX_LOGS];
    int head; // Consumer index
    int tail; // Producer index
    int count; // Current number of logs
    
    // Synchronization
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} ring_buffer_t;

#endif
