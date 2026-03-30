#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/select.h>

const int MAX_LINE = 512, MAX_CLIENTS = 100;
int nclients = 0;

enum state {
    MODE,
    FROM,
    TO,
    SUB,
    BODY,
    QUIT,
};

// Helper functions - as TCP is a stream
int send_all(int fd, char* msg) {
    // assumes msg already has '\n' in it
    int len = strlen(msg), sent = 0;
    while (sent < len) {
        int n = send(fd, msg + sent, len - sent, 0);
        if (n <= 0) {
            printf("***Send error\n");
            exit(1);
        }
        sent += n;
    }
    return sent;
}

int recv_line(int fd, char* buf) {
    int i = 0;
    char c;
    while (1) {
        int n = recv(fd, &c, 1, 0);
        if (n == 0) return 0;
        if (n < 0) return -1;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i + 1;
}

void printtime() {
    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    printf("[%s] ", buffer);
}

int directory_exists(const char* path) {
    struct stat info;

    if (stat(path, &info) != 0) {
        // stat() failed, check errno for the reason
        if (errno == ENOENT || errno == ENOTDIR) {
            return 0; // Directory does not exist or part of the path is not a directory
        } else {
            perror("stat error");
            return -1; // Other error (e.g., permissions, etc.)
        }
    } else {
        // Path exists, check if it is a directory using the S_ISDIR macro
        return S_ISDIR(info.st_mode) ? 1 : 0;
    }
}

void check_directory(char* name) {
    char path[MAX_LINE];
    snprintf(path, MAX_LINE, "/mailboxes/%s", name);

    struct stat info;
    if (stat(path, &info) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            printf("***Directory %s does not exist\n", path);
            exit(1);
        } else {
            perror("stat error");
            exit(1);
        }
    }
}

void readfile(char* filename, char* username[], char* password[]) {
    FILE* fptr = fopen(filename, "r");
    if (fptr == NULL) {
        printf("***File %s does not exist.\n", filename);
        exit(1);
    }

    char buffer[60];
    while (fgets(buffer, 60, fptr) != NULL) {
        buffer[strcspn(buffer, "\n")] = '\0';
        char name[21];
        bool bad = false;
        for (int i = 0; i < strlen(buffer); i++) {
            if (buffer[i] == ' ') {
                name[i] = '\0';
                break;
            }
            if (buffer[i] < 'a' || buffer[i] > 'z') bad = true;
            name[i] = buffer[i];
        }
        char* space = strchr(buffer, ' ');
        if (!space || bad || strlen(name) > 20) continue; // leave, this username is bad
        for (int i = 1; i < strlen(space); i++) {
            if ((space[i] < 'a' || space[i] > 'z') && (space[i] < '0' || space[i] > '9')) bad = true;
        }
        if (bad) continue;
        check_directory(name);
        username[nclients] = strdup(name);
        password[nclients] = strdup(space + 1);
        nclients++;
    }
    fclose(fptr);

    printtime();
    printf("Loaded %d users from %s\n", nclients, filename);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("***Usage : %s <PORT> <userfile>\n", argv[0]);
        exit(1);
    }

    int PORT = atoi(argv[1]);
    char* users = strdup(argv[2]);

    // socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }

    // bind
    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(PORT);
    servaddr.sin_family = AF_INET;
    if (bind(listen_fd, (struct sockaddr *)&servaddr, sizeof(servaddr) < 0)) {
        perror("bind");
        exit(1);
    }

    // listen
    if (listen(listen_fd, 5)) {
        perror("listen");
        exit(1);
    }

    printtime();
    printf("Server started on port %d\n", PORT);

    // read users.txt now
    char* username[MAX_CLIENTS];
    char* password[MAX_CLIENTS];
    readfile(users, username, password);

    // start select() loop now
    fd_set master_set, read_set; // will be used in select()
    FD_ZERO(&master_set);
    FD_SET(listen_fd, &master_set);
    int max_fd = listen_fd;

    int client_fd[MAX_CLIENTS], sender_state[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        sender_state[i] = -1;
        client_fd[i] = -1;
    }

    while (1) {
        read_set = master_set;

        int ready = select(max_fd + 1, &read_set, NULL, NULL, NULL);
        if (ready < 0) {
            perror("select");
            exit(1);
        }

        // Check if there is a new incoming connection - check this first
        if (FD_ISSET(listen_fd, &read_set)) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (client_fd[i] == -1) {
                    struct sockaddr_in client_addr;
                    socklen_t len = sizeof(client_addr);
                    client_fd[i] = accept(listen_fd, (struct sockaddr *)&client_addr, &len);
                    if (client_fd[i] < 0) {
                        perror("accept");
                        client_fd[i] = -1;
                        break;
                    }
                    printtime();
                    printf("New connection from %s : %d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                    FD_SET(client_fd[i], &master_set);
                    if (client_fd[i] > max_fd) max_fd = client_fd[i];
                    send_all(client_fd[i], "WELCOME SimpleMail v1.0\r\n");
                    break;
                }
            }
        }

        // Check if any client has sent something - then this
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fd[i] == -1 || !FD_ISSET(client_fd[i], &read_set)) continue;
            char req[MAX_LINE];
            int n = recv_line(client_fd[i], req);
            if (n == 0) {
                printtime();
                printf("Client %d disconnected abruptly.\n");
                close(client_fd[i]);
                FD_CLR(client_fd[i], &master_set);
                continue;
            }

            
        }
    }

    exit(0);
}