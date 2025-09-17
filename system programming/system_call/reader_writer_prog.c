#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>

#define MSG_SIZE 256
#define NUM_CONSUMERS 3

// Semaphore union for semctl
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

// Message structure for System V message queues
struct message {
    long mtype;      // Message type (required for System V)
    char mtext[MSG_SIZE];
};

// Global variables for cleanup
int msgid = -1;
int semid = -1;
pid_t child_pids[NUM_CONSUMERS + 1];
int num_children = 0;

// Signal handler for cleanup
void cleanup_handler(int sig) {
    printf("\nReceived signal %d, cleaning up...\n", sig);
    
    // Kill all child processes
    for (int i = 0; i < num_children; i++) {
        if (child_pids[i] > 0) {
            kill(child_pids[i], SIGTERM);
        }
    }
    
    // Remove message queue
    if (msgid != -1) {
        msgctl(msgid, IPC_RMID, NULL);
    }
    
    // Remove semaphore set
    if (semid != -1) {
        semctl(semid, 0, IPC_RMID, 0);
    }
    
    exit(0);
}

// Initialize semaphore
int init_semaphore(key_t key) {
    union semun arg;
    int sem_id;
    
    // Create semaphore set with 1 semaphore
    sem_id = semget(key, 1, IPC_CREAT | 0666);
    if (sem_id == -1) {
        perror("semget");
        return -1;
    }
    
    // Initialize semaphore value to 1 (binary semaphore for mutual exclusion)
    arg.val = 1;
    if (semctl(sem_id, 0, SETVAL, arg) == -1) {
        perror("semctl SETVAL");
        return -1;
    }
    
    return sem_id;
}

// Semaphore wait (P operation)
void sem_wait(int sem_id) {
    struct sembuf sb;
    sb.sem_num = 0;      // Semaphore number in set
    sb.sem_op = -1;      // Decrement semaphore
    sb.sem_flg = 0;      // Wait if necessary
    
    if (semop(sem_id, &sb, 1) == -1) {
        perror("semop wait");
        exit(1);
    }
}

// Semaphore signal (V operation)
void sem_signal(int sem_id) {
    struct sembuf sb;
    sb.sem_num = 0;      // Semaphore number in set
    sb.sem_op = 1;       // Increment semaphore
    sb.sem_flg = 0;      // No special flags
    
    if (semop(sem_id, &sb, 1) == -1) {
        perror("semop signal");
        exit(1);
    }
}

// Producer function
void producer(int msgid, int semid) {
    struct message msg;
    char messages[][MSG_SIZE] = {
        "Hello Consumer 1",
        "Hello Consumer 2", 
        "Hello Consumer 3",
        "Hello Consumer 1",
        "Hello Consumer 2",
        "Hello Consumer 3",
        "Hello Consumer 2",
        "Hello Consumer 1",
        "Hello Consumer 3"
    };
    
    printf("Producer [PID %d]: Started\n", getpid());
    
    int num_messages = sizeof(messages) / sizeof(messages[0]);
    
    for (int i = 0; i < num_messages; i++) {
        // Wait for semaphore (mutual exclusion)
        sem_wait(semid);
        
        msg.mtype = 1;  // Use type 1 for all messages
        strcpy(msg.mtext, messages[i]);
        
        if (msgsnd(msgid, &msg, strlen(msg.mtext) + 1, 0) == -1) {
            perror("Producer: msgsnd");
            sem_signal(semid);  // Release semaphore on error
            break;
        }
        
        printf("Producer: Sent message: %s\n", messages[i]);
        
        // Signal semaphore (release mutual exclusion)
        sem_signal(semid);
        
        sleep(1);  // Simulate production time
    }
    
    // Send quit signals for all consumers
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        sem_wait(semid);
        msg.mtype = 1;
        strcpy(msg.mtext, "QUIT");
        if (msgsnd(msgid, &msg, strlen(msg.mtext) + 1, 0) == -1) {
            perror("Producer: msgsnd QUIT");
        }
        sem_signal(semid);
    }
    
    printf("Producer: Finished\n");
}

// Consumer function with proper synchronization
void consumer(int consumer_id, int msgid, int semid) {
    struct message msg;
    char target_message[50];
    int messages_processed = 0;
    int max_idle_cycles = 50;  // Prevent infinite loops
    int idle_cycles = 0;
    
    // Create target message pattern
    snprintf(target_message, sizeof(target_message), "Hello Consumer %d", consumer_id);
    
    printf("Consumer %d [PID %d]: Started, looking for: %s\n", 
           consumer_id, getpid(), target_message);
    
    while (1) {
        // Wait for semaphore before accessing message queue
        sem_wait(semid);
        
        // Try to receive message (non-blocking to prevent deadlock)
        ssize_t result = msgrcv(msgid, &msg, MSG_SIZE, 1, IPC_NOWAIT);
        
        if (result == -1) {
            if (errno == ENOMSG) {
                // No message available
                sem_signal(semid);
                idle_cycles++;
                if (idle_cycles > max_idle_cycles) {
                    printf("Consumer %d: No messages for too long, exiting\n", consumer_id);
                    break;
                }
                usleep(100000);  // Sleep 0.1 seconds
                continue;
            } else if (errno == EIDRM) {
                printf("Consumer %d: Message queue removed\n", consumer_id);
                sem_signal(semid);
                break;
            } else {
                perror("Consumer: msgrcv");
                sem_signal(semid);
                break;
            }
        }
        
        printf("Consumer %d: Read message: %s\n", consumer_id, msg.mtext);
        idle_cycles = 0;  // Reset idle counter
        
        // Check if it's a quit message
        if (strcmp(msg.mtext, "QUIT") == 0) {
            printf("Consumer %d: Received QUIT signal\n", consumer_id);
            sem_signal(semid);
            break;
        }
        
        // Check if message is for this consumer
        if (strcmp(msg.mtext, target_message) == 0) {
            printf("Consumer %d: *** MESSAGE FOR ME - PROCESSING ***\n", consumer_id);
            messages_processed++;
            sem_signal(semid);
        } else {
            printf("Consumer %d: Message not for me, putting back in queue\n", consumer_id);
            
            // Put message back in queue
            struct message put_back_msg;
            put_back_msg.mtype = 1;
            strcpy(put_back_msg.mtext, msg.mtext);
            
            if (msgsnd(msgid, &put_back_msg, strlen(put_back_msg.mtext) + 1, 0) == -1) {
                perror("Consumer: msgsnd (put back)");
            }
            
            sem_signal(semid);
        }
        
        // Small delay to prevent busy waiting
        usleep(50000);  // Sleep 0.05 seconds
    }
    
    printf("Consumer %d: Processed %d messages, finished\n", consumer_id, messages_processed);
}

int main() {
    key_t msg_key, sem_key;
    
    // Setup signal handlers for cleanup
    signal(SIGINT, cleanup_handler);
    signal(SIGTERM, cleanup_handler);
    
    // Generate unique keys
    msg_key = ftok(".", 'M');
    sem_key = ftok(".", 'S');
    
    if (msg_key == -1 || sem_key == -1) {
        perror("ftok");
        exit(1);
    }
    
    // Create message queue
    msgid = msgget(msg_key, IPC_CREAT | 0666);
    if (msgid == -1) {
        perror("msgget");
        exit(1);
    }
    
    // Initialize semaphore
    semid = init_semaphore(sem_key);
    if (semid == -1) {
        msgctl(msgid, IPC_RMID, NULL);
        exit(1);
    }
    
    printf("=== Synchronized Producer-Consumer System by Darsan ===\n");
    printf("Message Queue ID: %d\n", msgid);
    printf("Semaphore ID: %d\n", semid);
    printf("Starting 1 Producer and %d Consumers\n\n", NUM_CONSUMERS);
    
    // Create producer process
    child_pids[0] = fork();
    if (child_pids[0] == 0) {
        producer(msgid, semid);
        exit(0);
    } else if (child_pids[0] < 0) {
        perror("fork producer");
        cleanup_handler(0);
    }
    num_children++;
    
    // Small delay to ensure producer starts first
    sleep(1);
    
    // Create consumer processes
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        child_pids[i + 1] = fork();
        if (child_pids[i + 1] == 0) {
            consumer(i + 1, msgid, semid);
            exit(0);
        } else if (child_pids[i + 1] < 0) {
            perror("fork consumer");
            cleanup_handler(0);
        }
        num_children++;
        usleep(200000);  // Small delay between consumer creation
    }
    
    // Wait for all processes to finish
    for (int i = 0; i < NUM_CONSUMERS + 1; i++) {
        int status;
        pid_t finished_pid = waitpid(child_pids[i], &status, 0);
        if (finished_pid > 0) {
            printf("Process %d finished with status %d\n", finished_pid, status);
        }
    }
    
    // Clean up resources
    if (msgctl(msgid, IPC_RMID, NULL) == -1) {
        perror("msgctl IPC_RMID");
    } else {
        printf("Message queue cleaned up successfully\n");
    }
    
    if (semctl(semid, 0, IPC_RMID, 0) == -1) {
        perror("semctl IPC_RMID");
    } else {
        printf("Semaphore cleaned up successfully\n");
    }
    
    printf("\n=== All processes finished successfully ===\n");
    return 0;
}
