#include <stdio.h>          // Standard I/O functions
#include <stdlib.h>         // Standard library functions
#include <string.h>         // String manipulation functions
#include <stdint.h>         // Standard integer types
#include <unistd.h>         // POSIX API functions
#include <fcntl.h>          // File control options
#include <sys/mman.h>       // Memory management
#include <sys/stat.h>       // File status
#include <pthread.h>        // POSIX threads

// define the shared memory structure
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

// function prototypes
int is_valid_operation(const char *operation);
int get_next_floor(const char *current_floor, char *next_floor, const char *direction);
int is_doors_closed(const car_shared_mem *shared_mem);
int is_elevator_moving(const car_shared_mem *shared_mem);
int floor_label_to_number(const char *floor_label);
void floor_number_to_label(int floor_num, char *floor_label, size_t label_size);

char *shm_name = NULL;

// begin main function
int main(int argc, char *argv[]) {
    // check/validate the correct number of arguments
    if (argc != 3) {
        fprintf(stderr, "Usage: %s {car name} {operation}\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // extract car name and operation
    const char *car_name = argv[1];
    const char *operation = argv[2];

    // validate the specified operation
    if (!is_valid_operation(operation)) {
        fprintf(stderr, "Invalid operation.\n");
        exit(EXIT_FAILURE);
    }

    // construct the shared memory name
    // char shm_name[32];
    // snprintf(shm_name, sizeof(shm_name), "/car%s", car_name);
    shm_name = malloc(strlen("/car") + strlen(car_name) + 1);

    // open the shared memory segment
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        fprintf(stderr, "Unable to access car %s.\n", car_name);
        exit(EXIT_FAILURE);
    }

    // map the shared memory segment to the car
    car_shared_mem *shared_mem = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_mem == MAP_FAILED) {
        perror("Failed to map shared memory");
        close(shm_fd);
        exit(EXIT_FAILURE);
    }

    // lock the mutex before accessing shared memory
    pthread_mutex_lock(&shared_mem->mutex);

    // perform requested operation
    // set open_button to 1
    if (strcmp(operation, "open") == 0) {
        shared_mem->open_button = 1;
    // set close_button to 1
    } else if (strcmp(operation, "close") == 0) {
        shared_mem->close_button = 1;
    // set emergency_stop to 1
    } else if (strcmp(operation, "stop") == 0) {
        shared_mem->emergency_stop = 1;
    // set individual_service_mode to 1 and emergency_mode to 0
    } else if (strcmp(operation, "service_on") == 0) {
        shared_mem->individual_service_mode = 1;
        shared_mem->emergency_mode = 0;
    // set individual_service_mode to 0
    } else if (strcmp(operation, "service_off") == 0) {
        shared_mem->individual_service_mode = 0;
    // set destination_floor to the next floor and handle errors
    } else if (strcmp(operation, "up") == 0 || strcmp(operation, "down") == 0) {
        // check if the elevator is in individual service mode
        if (shared_mem->individual_service_mode == 0) {
            fprintf(stderr, "Operation only allowed in service mode.\n");
            pthread_mutex_unlock(&shared_mem->mutex);
            munmap(shared_mem, sizeof(car_shared_mem));
            close(shm_fd);
            exit(EXIT_FAILURE);
        }
        // check if the doors are closed
        if (!is_doors_closed(shared_mem)) {
            fprintf(stderr, "Operation not allowed while doors are open.\n");
            pthread_mutex_unlock(&shared_mem->mutex);
            munmap(shared_mem, sizeof(car_shared_mem));
            close(shm_fd);
            exit(EXIT_FAILURE);
        }
        // check if the elevator is moving
        if (is_elevator_moving(shared_mem)) {
            fprintf(stderr, "Operation not allowed while elevator is moving.\n");
            pthread_mutex_unlock(&shared_mem->mutex);
            munmap(shared_mem, sizeof(car_shared_mem));
            close(shm_fd);
            exit(EXIT_FAILURE);
        }
        // compute next floor
        char next_floor[4];
        int result = get_next_floor(shared_mem->current_floor, next_floor, operation);
        if (result == -1) {
            fprintf(stderr, "Cannot move %s from floor %s.\n", operation, shared_mem->current_floor);
            pthread_mutex_unlock(&shared_mem->mutex);
            munmap(shared_mem, sizeof(car_shared_mem));
            close(shm_fd);
            exit(EXIT_FAILURE);
        }
        // set destination_floor to the next floor
        strncpy(shared_mem->destination_floor, next_floor, sizeof(shared_mem->destination_floor));
        shared_mem->destination_floor[sizeof(shared_mem->destination_floor) - 1] = '\0'; // Ensure null-termination
    }

    // signal the condition variable if necessary
    pthread_cond_broadcast(&shared_mem->cond);

    // unlock the mutex
    pthread_mutex_unlock(&shared_mem->mutex);

    // unmap the shared memory and close the file descriptor
    munmap(shared_mem, sizeof(car_shared_mem));
    close(shm_fd);

    // program terminates after performing the operation
    return 0;
}

// HELPER FUNCTIONS

// function to validate if the operation is one of the allowed commands
int is_valid_operation(const char *operation) {
    const char *valid_operations[] = {
        "open",
        "close",
        "stop",
        "service_on",
        "service_off",
        "up",
        "down"
    };
    size_t num_operations = sizeof(valid_operations) / sizeof(valid_operations[0]);
    for (size_t i = 0; i < num_operations; i++) {
        if (strcmp(operation, valid_operations[i]) == 0) {
            return 1; // Valid operation
        }
    }
    return 0; // Invalid operation
}

// function to calculate the next floor up or down based on the current floor
int get_next_floor(const char *current_floor, char *next_floor, const char *direction) {
    // convert current floor label to a number
    int floor_num = floor_label_to_number(current_floor);

    // check for conversion errors
    if (floor_num == -9999) {
        return -1; // Invalid floor label
    }

    // calculate the next floor number
    if (strcmp(direction, "up") == 0) {
        floor_num += 1;
    } else if (strcmp(direction, "down") == 0) {
        floor_num -= 1;
    }

    // check if the next floor is within valid range (B99 to 999)
    if (floor_num < -99 || floor_num > 999 || floor_num == 0) {
        return -1; // Cannot move further in this direction
    }

    // convert floor number back to floor label
    floor_number_to_label(floor_num, next_floor, 4);

    return 0; // Success
}

// function to check if the doors are closed
int is_doors_closed(const car_shared_mem *shared_mem) {
    return (strcmp(shared_mem->status, "Closed") == 0);
}

// function to check if the elevator is moving (status is "Between")
int is_elevator_moving(const car_shared_mem *shared_mem) {
    return (strcmp(shared_mem->status, "Between") == 0);
}

// helper function to convert floor label to a number
int floor_label_to_number(const char *floor_label) {
    if (floor_label == NULL || strlen(floor_label) == 0) {
        return -9999; // Error code for invalid input
    }
    int floor_num = 0;
    if (floor_label[0] == 'B') {
        // Basement floor
        floor_num = -(atoi(floor_label + 1));
    } else {
        // Regular floor
        floor_num = atoi(floor_label);
    }
    // Check for invalid conversion
    if (floor_num == 0 && strcmp(floor_label, "0") != 0) {
        return -9999; // Error code for invalid input
    }
    return floor_num;
}

// Helper function to convert floor number to floor label
void floor_number_to_label(int floor_num, char *floor_label, size_t label_size) {
    if (floor_num < 0) {
        // Basement floor
        snprintf(floor_label, label_size, "B%d", -floor_num);
    } else {
        // Regular floor
        snprintf(floor_label, label_size, "%d", floor_num);
    }
    floor_label[label_size - 1] = '\0'; // Ensure null-termination
}