#include <stdio.h>      // Standard I/O functions
#include <stdlib.h>     // Standard library functions
#include <string.h>     // String manipulation functions
#include <stdint.h>     // Standard integer types
#include <unistd.h>     // POSIX API functions
#include <pthread.h>    // POSIX threads
#include <signal.h>     // Signal handling
#include <fcntl.h>      // File control options
#include <sys/mman.h>   // Memory management
#include <sys/stat.h>   // File status
#include <arpa/inet.h>  // Internet operations
#include <sys/socket.h> // Socket programming
#include <sys/types.h>  // Data types
#include <errno.h>      // Error handling
#include <netinet/in.h> // Internet address family
#include <sys/types.h>  // Data types
#include <time.h>       // Time functions

// Define constants for ICP-IP communication
#define PORT 3000            // Port number for the controller server
#define BUFFER_SIZE 1024     // Buffer size for sending/receiving messages

// Define shared memory structure
typedef struct {
  pthread_mutex_t mutex;           // Locked while accessing struct contents
  pthread_cond_t cond;             // Signalled when the contents change
  char current_floor[4];           // C string in the range B99-B1 and 1-999
  char destination_floor[4];       // Same format as above
  char status[8];                  // C string indicating the elevator's status
  uint8_t open_button;             // 1 if open doors button is pressed, else 0
  uint8_t close_button;            // 1 if close doors button is pressed, else 0
  uint8_t door_obstruction;        // 1 if obstruction detected, else 0
  uint8_t overload;                // 1 if overload detected
  uint8_t emergency_stop;          // 1 if stop button has been pressed, else 0
  uint8_t individual_service_mode; // 1 if in individual service mode, else 0
  uint8_t emergency_mode;          // 1 if in emergency mode, else 0
} car_shared_mem;

// Global variables
char *shm_name = NULL;       // Name of the shared memory segment
int shm_fd = -1;             // File descriptor for the shared memory object
car_shared_mem *shared_mem = NULL; // Pointer to the shared memory structure

int sockfd = -1;             // Socket file descriptor for network communication
int connected = 0;           // Connection status flag (0 = not connected, 1 = connected)
int delay_ms = 1000;         // Delay in milliseconds for elevator operations
pthread_t tcp_thread;     // Thread for TCP communication

char *lowest_floor = NULL; // Store lowest floor globally for use in TCP thread
char *highest_floor = NULL; // Store highest floor globally for use in TCP thread

volatile sig_atomic_t running = 1; // Flag to control the main loop (used in signal handling) (cannot be interrupted)

// Signal Handling
void handle_sigint(int sig) {
    running = 0;
}

void init_shared_memory(char *name, char *lowest_floor) {
    // Allocate memory for the shared memory name
    shm_name = malloc(strlen("/car") + strlen(name) + 1); // +1 for the null terminator
    if (shm_name == NULL) {
        perror("Failed to allocate memory for shared memory name");
        exit(EXIT_FAILURE);
    }

    // Construct the shared memory name
    sprintf(shm_name, "/car%s", name);

    // create the shared memory object
    shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666); // 0666 = read/write permissions for owner, group, and others
    if (shm_fd == -1) { // or < 0 ?
        perror("Failed to create/open shared memory object");
        free(shm_name);
        // shm_name = NULL; ?
        exit(EXIT_FAILURE);
    }

    // Set the size of the shared memory object and handle errors
    if (ftruncate(shm_fd, sizeof(car_shared_mem)) == -1) {
        perror("Failed to set size of shared memory object");
        shm_unlink(shm_name);
        free(shm_name);
        // shm_name = NULL; ?
        exit(EXIT_FAILURE);
    }

    // Map the shared memory object into the address space of the process
    shared_mem = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    // error handling
    if (shared_mem == MAP_FAILED) {
        perror("Failed to map shared memory");
        shm_unlink(shm_name);
        free(shm_name);
        exit(EXIT_FAILURE);
    }

    // Initialize the shared memory structure
    pthread_mutexattr_t mattr; // mattr_attr
    pthread_condattr_t cattr;  // cattr_attr

    // Initialize the mutex and condition variable attributes
    if (pthread_mutexattr_init(&mattr) != 0) {
        perror("Failed to initialize mutex attributes");
        // Handle cleanup
        exit(EXIT_FAILURE);
    }
    // Set the mutex as process-shared
    if (pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED) != 0) {
        perror("Failed to set mutex as process-shared");
        // Handle cleanup
        exit(EXIT_FAILURE);
    }

    // Initialize the condition variable attributes
    if (pthread_condattr_init(&cattr) != 0) {
        perror("Failed to initialize condition variable attributes");
        // Handle cleanup
        exit(EXIT_FAILURE);
    }
    // Set the condition variable as process-shared
    if (pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED) != 0) {
        perror("Failed to set condition variable as process-shared");
        // Handle cleanup
        exit(EXIT_FAILURE);
    }

    // Initialize the mutex and condition variable in the shared memory
    if (pthread_mutex_init(&shared_mem->mutex, &mattr) != 0) {
        perror("Failed to initialize mutex");
        // Handle cleanup
        exit(EXIT_FAILURE);
    }
    if (pthread_cond_init(&shared_mem->cond, &cattr) != 0) {
        perror("Failed to initialize condition variable");
        // Handle cleanup
        exit(EXIT_FAILURE);
    }

    // Destroy the attribute objects as they are no longer needed
    pthread_mutexattr_destroy(&mattr);
    pthread_condattr_destroy(&cattr);

    // Lock the mutex before modifying shared memory contents
    pthread_mutex_lock(&shared_mem->mutex);

    // Initialize other fields
    // Initialize the current and destination floor with the lowest floor
    strncpy(shared_mem->current_floor, lowest_floor, sizeof(shared_mem->current_floor));
    // shared_mem->current_floor[sizeof(shared_mem->current_floor) - 1] = '\0'; // Ensure null-termination ?
    strncpy(shared_mem->destination_floor, lowest_floor, sizeof(shared_mem->destination_floor));
    // shared_mem->destination_floor[sizeof(shared_mem->destination_floor) - 1] = '\0'; ?
    // Initialize the status with "Closed"
    strncpy(shared_mem->status, "Closed", sizeof(shared_mem->status));
    // shared_mem->status[sizeof(shared_mem->status) - 1] = '\0'; ?

    shared_mem->open_button = 0;
    shared_mem->close_button = 0;
    shared_mem->door_obstruction = 0;
    shared_mem->overload = 0;
    shared_mem->emergency_stop = 0;
    shared_mem->individual_service_mode = 0;
    shared_mem->emergency_mode = 0;

    // Signal any waiting processes that the shared memory has been initialized
    pthread_cond_broadcast(&shared_mem->cond);

    // Unlock the mutex after modification
    pthread_mutex_unlock(&shared_mem->mutex);
}

// Controller helper functions (1)
void recv_looped(int fd, void *buf, size_t sz) {
    char *ptr = buf;
    size_t remain = sz;
    while (remain > 0) {
        ssize_t received = read(fd, ptr, remain);
        if (received == -1) {
            perror("read()");
            exit(EXIT_FAILURE);
        } else if (received == 0) {
            // Connection closed by peer
            fprintf(stderr, "Connection closed by peer\n");
            exit(EXIT_FAILURE);
        }
        ptr += received;
        remain -= received;
    }
}
// Controller helper functions (2)
char *receive_msg(int fd) {
    uint32_t nlen;
    recv_looped(fd, &nlen, sizeof(nlen));
    uint32_t len = ntohl(nlen);
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        perror("malloc()");
        exit(EXIT_FAILURE);
    }
    recv_looped(fd, buf, len);
    buf[len] = '\0'; // Null-terminate the message
    return buf;
}
void send_looped(int fd, const void *buf, size_t sz) {
    const char *ptr = buf;
    size_t remain = sz;
    while (remain > 0) {
        ssize_t sent = write(fd, ptr, remain);
        if (sent == -1) {
            perror("write()");
            exit(EXIT_FAILURE);
        }
        ptr += sent;
        remain -= sent;
    }
}

void send_message(int fd, const char *buf) {
    uint32_t len = htonl(strlen(buf));
    send_looped(fd, &len, sizeof(len));
    send_looped(fd, buf, strlen(buf));
}

void *tcp_communication(void *arg) {

    char *name = (char *)arg;
    struct sockaddr_in serv_addr;
    char send_buffer[BUFFER_SIZE];
    char recv_buffer[BUFFER_SIZE];

    while (running) {
        // Attempt to connect if not connected
        if (!connected && !shared_mem->individual_service_mode && !shared_mem->emergency_mode) {
            // Create socket
            sockfd = socket(AF_INET, SOCK_STREAM, 0); // 0 for default protocol (TCP), otherwise use IPPROTO_TCP
            if (sockfd < 0) {
                perror("Socket creation failed");
                sleep(delay_ms / 1000);
                continue;
            }

            // Set up server address
            memset(&serv_addr, 0, sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;
            serv_addr.sin_port = htons(PORT);
            serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // htonl() ?

            //  should i use bind()?

            // Attempt to connect
            if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
                // perror("Connect failed");
                close(sockfd);
                sleep(delay_ms / 1000);
                continue;
            }

            connected = 1;
            printf("Connected to controller\n");

            // Send CAR initialization message
            pthread_mutex_lock(&shared_mem->mutex);
            char init_msg[BUFFER_SIZE];
            snprintf(init_msg, BUFFER_SIZE, "CAR %s %s %s", name, lowest_floor, highest_floor);
            pthread_mutex_unlock(&shared_mem->mutex);

            send_message(sockfd, init_msg);
        }

        // Send STATUS updates periodically
        static struct timespec last_status_time = {0, 0};
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        // struct timeval tv;
        // gettimeofday(&tv, NULL);
        // current_time.tv_sec = tv.tv_sec;
        // current_time.tv_nsec = tv.tv_usec * 1000;

        double elapsed_ms = (current_time.tv_sec - last_status_time.tv_sec) * 1000.0 +
                            (current_time.tv_nsec - last_status_time.tv_nsec) / 1000000.0;

        if (elapsed_ms >= delay_ms) {
            // Lock shared memory to read status
            pthread_mutex_lock(&shared_mem->mutex);
            char status_msg[BUFFER_SIZE];
            snprintf(status_msg, BUFFER_SIZE, "STATUS %s %s %s",
                     shared_mem->status, shared_mem->current_floor, shared_mem->destination_floor);
            pthread_mutex_unlock(&shared_mem->mutex);

            send_message(sockfd, status_msg);
            last_status_time = current_time;
        }

        // Receive messages from controller
        char *recv_buffer = NULL;
        recv_buffer = receive_msg(sockfd);
        if (recv_buffer == NULL) {
            // Connection closed or error
            printf("Controller disconnected\n");
            close(sockfd);
            connected = 0;
            sleep(delay_ms / 1000);
            continue;
        }
        // free(recv_buffer);

        // Handle controller message
        pthread_mutex_lock(&shared_mem->mutex);
        if (strncmp(recv_buffer, "FLOOR ", 6) == 0) {
            // Update destination floor
            char *floor = recv_buffer + 6;
            strncpy(shared_mem->destination_floor, floor, sizeof(shared_mem->destination_floor));
            shared_mem->destination_floor[sizeof(shared_mem->destination_floor) - 1] = '\0';
            pthread_cond_broadcast(&shared_mem->cond);
        }
        pthread_mutex_unlock(&shared_mem->mutex);
        free(recv_buffer);

        // Check for individual service mode or emergency mode to disconnect
        pthread_mutex_lock(&shared_mem->mutex);
        if ((shared_mem->individual_service_mode || shared_mem->emergency_mode) && connected) {
            // Send appropriate message before disconnecting
            char mode_msg[BUFFER_SIZE];
            if (shared_mem->individual_service_mode) {
                snprintf(mode_msg, BUFFER_SIZE, "INDIVIDUAL SERVICE");
            } else if (shared_mem->emergency_mode) {
                snprintf(mode_msg, BUFFER_SIZE, "EMERGENCY");
            }
            send_message(sockfd, mode_msg);
            close(sockfd);
            connected = 0;
            printf("Disconnected from controller due to mode change\n");
        }
        pthread_mutex_unlock(&shared_mem->mutex);

        usleep(100000); // Sleep for 100ms
    }
    
    // Clean up
    if (connected) {
        close(sockfd);
    }
    return NULL;

}

int main(int argc, char *argv[]) {
    // ... (argument parsing and initialization)


    // Signal handling
    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE signals

    // Initialize shared memory
    init_shared_memory(name, lowest_floor);

    // Start TCP communication thread
    pthread_create(&tcp_thread, NULL, tcp_communication, (void *)name);

    // // Start elevator main loop
    // elevator_main_loop();

    // Wait for TCP thread to finish
    pthread_join(tcp_thread, NULL);

    // Clean up resources
    // ... (unmap shared memory, close file descriptors)

    return 0;
}


