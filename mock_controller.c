// mock_controller.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>

void recv_looped(int fd, void *buf, size_t sz) {
    char *ptr = buf;
    size_t remain = sz;
    while (remain > 0) {
        ssize_t received = read(fd, ptr, remain);
        if (received == -1) {
            perror("read()");
            exit(1);
        } else if (received == 0) {
            // Connection closed by peer
            fprintf(stderr, "Connection closed by peer\n");
            exit(1);
        }
        ptr += received;
        remain -= received;
    }
}

char *receive_msg(int fd) {
    uint32_t nlen;
    recv_looped(fd, &nlen, sizeof(nlen));
    uint32_t len = ntohl(nlen);
    char *buf = malloc(len + 1);
    if (!buf) {
        perror("malloc()");
        exit(1);
    }
    recv_looped(fd, buf, len);
    buf[len] = '\0';
    return buf;
}

int main() {
    int listensockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listensockfd == -1) {
        perror("socket()");
        exit(1);
    }

    int opt_enable = 1;
    if (setsockopt(listensockfd, SOL_SOCKET, SO_REUSEADDR, &opt_enable,
                   sizeof(opt_enable)) == -1) {
        perror("setsockopt()");
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(3000);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listensockfd, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind()");
        exit(1);
    }

    if (listen(listensockfd, 10) == -1) {
        perror("listen()");
        exit(1);
    }

    printf("Mock controller is running and listening on port 3000...\n");

    for (;;) {
        struct sockaddr_in clientaddr;
        socklen_t clientaddr_len = sizeof(clientaddr);
        int clientfd = accept(listensockfd, (struct sockaddr *)&clientaddr,
                              &clientaddr_len);
        if (clientfd == -1) {
            perror("accept()");
            continue;
        }

        char *msg = receive_msg(clientfd);
        printf("Received message from client: %s\n", msg);
        free(msg);

        // For testing, send a FLOOR command to the car
        const char *floor_cmd = "FLOOR 5";
        uint32_t len = htonl(strlen(floor_cmd));
        if (write(clientfd, &len, sizeof(len)) != sizeof(len)) {
            perror("write()");
        }
        if (write(clientfd, floor_cmd, strlen(floor_cmd)) != (ssize_t)strlen(floor_cmd)) {
            perror("write()");
        }

        if (shutdown(clientfd, SHUT_RDWR) == -1) {
            perror("shutdown()");
        }
        if (close(clientfd) == -1) {
            perror("close()");
        }
    }

    return 0;
}