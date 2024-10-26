#include <stdio.h>          // Standard I/O functions
#include <stdlib.h>         // Standard library functions
#include <string.h>         // String manipulation functions
#include <unistd.h>         // POSIX API functions
#include <arpa/inet.h>      // Internet operations
#include <sys/socket.h>     // Socket programming
#include <sys/types.h>      // Data types
#include <netinet/in.h>     // Internet address family
#include <errno.h>          // Error handling

// Define constants
#define PORT 3000           // Port number for the controller server
#define BUFFER_SIZE 1024    // Buffer size for sending/receiving messages

// Function prototypes
int is_floor_valid(const char *floor);
void send_message(int sockfd, const char *message);
char *receive_message(int sockfd);


int main(int argc, char *argv[]) {

    // validate/check if the correct number of command-line arguments are provided = 2
    if (argc != 3) {
        fprintf(stderr, "Usage: %s {source floor} {destination floor}\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *source_floor = argv[1];
    const char *destination_floor = argv[2];

    // validate the source and destination floors
    if (!is_floor_valid(source_floor) || !is_floor_valid(destination_floor)) {
        fprintf(stderr, "Invalid floor(s) specified.\n");
        exit(EXIT_FAILURE);
    }

    // validate that the source and destination floors are not the same
    if (strcmp(source_floor, destination_floor) == 0) {
    fprintf(stderr, "You are already on that floor!\n");
    exit(EXIT_FAILURE);
    }

    // variables for socket and server
    int sockfd;
    struct sockaddr_in serv_addr;

    // set up the socket (using IPv4)
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // set up the server address (using IPv4)

    // initialize serv_addr structure to zero and set family
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    
    // convert address from text to binary and check validity
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
    fprintf(stderr, "Invalid address / address not supported\n");
    close(sockfd);
    exit(EXIT_FAILURE);
    }

    // attempt to connect to the controller server
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    fprintf(stderr, "Unable to connect to elevator system.\n");
    close(sockfd);
    exit(EXIT_FAILURE);
    }

    // Prepare the CALL message
    char call_message[BUFFER_SIZE];
    snprintf(call_message, BUFFER_SIZE, "CALL %s %s", source_floor, destination_floor);

    // Send the CALL message to the controller
    send_message(sockfd, call_message);

    // Receive the message from the controller
    char *response = receive_message(sockfd);

    // print message if valid
    if (strncmp(response, "CAR ", 4) == 0) {
    char car_name[BUFFER_SIZE];
    sscanf(response + 4, "%s", car_name);
    printf("Car %s is arriving.\n", car_name);
    // otherwise handle invalid message
    } else if (strcmp(response, "UNAVAILABLE") == 0) {
        printf("Sorry, no car is available to take this request.\n");
    } else {
        printf("Received unexpected response from controller: %s\n", response);
    }
    // free the response buffer
    free(response);

    // close socket and exit
    close(sockfd);
    return 0;

}   

// Helper Functions

// check if the provided floor label is valid
// function to validate floor labels - // allowable floors = B1-B99 or 1-999
int is_floor_valid(const char *floor) {

    // check if the floor is NULL or empty
    if (floor == NULL || strlen(floor) == 0) {
        return 0;
    }

    // check if the floor is a basement floor (starts with 'B')
    if (floor[0] == 'B') {
        // the rest of the string should be a number between 1 and 99
        char *endptr;
        int floor_num = (int)strtol(floor + 1, &endptr, 10);
        if (*endptr != '\0' || floor_num < 1 || floor_num > 99) {
            return 0;
        }
    } else {
        // the string should be a number between 1 and 999
        char *endptr;
        int floor_num = (int)strtol(floor, &endptr, 10);
        if (*endptr != '\0' || floor_num < 1 || floor_num > 999) {
            return 0;
        }
    }

    // Floor label is valid
    return 1;

}

// function to send a message to the server
void send_message(int sockfd, const char *message) {
    // calculate length of the message and convert to network byte order
    uint32_t msg_len = strlen(message);
    uint32_t net_len = htonl(msg_len);

    // send the message length prefix
    ssize_t sent_bytes = send(sockfd, &net_len, sizeof(net_len), 0);
    if (sent_bytes != sizeof(net_len)) {
        perror("Failed to send message length");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    // sned the message
    sent_bytes = send(sockfd, message, msg_len, 0);
    if (sent_bytes != msg_len) {
        perror("Failed to send message");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
}

// function to receive length prefixed message from the server
char *receive_message(int sockfd) {
    // buffer to store the message length prefix
    uint32_t net_len;
    ssize_t received_bytes = recv(sockfd, &net_len, sizeof(net_len), MSG_WAITALL);
    // check if server is closed
    if (received_bytes == 0) {
        fprintf(stderr, "Connection closed by server\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    // check if message length was not received
    } else if (received_bytes != sizeof(net_len)) {
        perror("Failed to receive message length");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // convert the length from network byte order to host byte order
    uint32_t msg_len = ntohl(net_len);

    // allocate memory to store the incoming message
    char *message = malloc(msg_len + 1); // +1 for null terminator
    if (message == NULL) {
        perror("Memory allocation failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // receive the message
    received_bytes = recv(sockfd, message, msg_len, MSG_WAITALL);
    if (received_bytes != msg_len) {
        perror("Failed to receive message");
        free(message);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // null-terminate the received message and return
    message[msg_len] = '\0';
    return message;
}