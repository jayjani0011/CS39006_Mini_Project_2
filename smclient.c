#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdbool.h>

const int MAX_CLIENTS = 10, MAX_LINE = 1024;

// Helper functions - as TCP is a stream
int send_all(int fd, char* msg) {
    strcat(msg, "\n");
    int len = strlen(msg), sent = 0;
    while (sent < len) {
        int n = send(fd, msg + sent, len - sent, 0);
        if (n <= 0) {
            printf("***Send error\n");
            exit(1);
        }
        sent += n;
    }
    msg[len - 1] = '\0';
    return sent;
}

int recv_line(int fd, char* buf) {
    int i = 0;
    char c;
    while (1) {
        int n = recv(fd, &c, 1, 0);
        if (n == 0) return 0;
        if (n < 0) {
            printf("***Recv error\n");
            exit(1);
        }
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i + 1;
}


int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("***Usage : %s <server_ip> <PORT>\n", argv[0]);
        exit(1);
    }

    char* server_ip = strdup(argv[1]);
    int PORT = atoi(argv[2]);

    // socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(1);
    }

    // connect
    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_port = htons(PORT);
    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip, &servaddr.sin_addr);
    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect");
        exit(1);
    }
    // printf("Client connected to server successfully\n");

    // send/recv

    // close
    printf("Closing client sockfd...\n");
    close(sockfd);
    printf("Leaving...\n");

    return 0;
}