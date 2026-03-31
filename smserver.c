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
    MODE_SEND,
    MODE_RECV,
    FROM,
    TO,
    SUB,
    BODY,
    QUIT,
};

typedef struct {
    char* username;
    char* password;
    int inboxCnt;
} cliinfo;

typedef struct {
    char* from;
    char** to;
    char* subject;
    char** body;
} messageinfo;

// Helper functions - as TCP is a stream
int send_all(int fd, char* msg) {
    // assumes msg already has "\r\n" in it
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
        buf[i++] = c;
        if (c == '\n' && i >= 2 && buf[i - 2] == '\r') break; // got the terminator \r\n
    }
    buf[i] = '\0';
    return i + 1;
}

char* gettime() {
    time_t rawtime;
    struct tm *timeinfo;
    char* buffer = malloc(80);

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buffer, 80, "%Y-%m-%d %H:%M:%S", timeinfo);
    return buffer;
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

void readfile(char* filename, cliinfo users_info[]) {
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
        users_info[nclients].username = strdup(name);
        users_info[nclients].password = strdup(space + 1);
        users_info[nclients].inboxCnt = 0;
        nclients++;
    }
    fclose(fptr);

    printf("[%s] Loaded %d users from %s\n", gettime(), nclients, filename);
}

int findindex(cliinfo users_info[], char* name) {
    for (int i = 0; i < nclients; i++) {
        if (strcmp(users_info[i].username, name) == 0) return i;
    }
    return -1;
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

    printf("[%s] Server started on port %d\n", gettime(), PORT);

    // read users.txt now
    cliinfo users_info[MAX_CLIENTS];
    readfile(users, users_info);

    // start select() loop now
    fd_set master_set, read_set; // will be used in select()
    FD_ZERO(&master_set);
    FD_SET(listen_fd, &master_set);
    int max_fd = listen_fd;

    int client_fd[MAX_CLIENTS], state[MAX_CLIENTS];
    messageinfo msg[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        state[i] = QUIT;
        client_fd[i] = -1;
        msg[i].from = NULL;
        msg[i].to = NULL;
        msg[i].subject = NULL;
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
                    printf("[%s] New connection from %s : %d\n", gettime(), inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
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
                printf("[%s] Client %d disconnected abruptly.\n", gettime(), i);
                close(client_fd[i]);
                FD_CLR(client_fd[i], &master_set);
                continue;
            }
            // handle the client request here
            if (strcmp(req, "MODE SEND") == 0) {
                if (state[i] != QUIT) {
                    send_all(client_fd[i], "ERR Bad sequence\r\n");
                    FD_CLR(client_fd[i], &master_set);
                    close(client_fd[i]);
                    client_fd[i] = -1;
                } else {
                    state[i] = MODE_SEND;
                    send_all(client_fd[i], "OK\r\n");
                    printf("[%s] Client %d selected MODE SEND\n", gettime(), i);
                }
            }
            else if (strcmp(req, "MODE RECV") == 0) {
                if (state[i] != QUIT) {
                    send_all(client_fd[i], "ERR Bad sequence\r\n");
                    FD_CLR(client_fd[i], &master_set);
                    close(client_fd[i]);
                    client_fd[i] = -1;
                } else {
                    state[i] = MODE_RECV;
                    send_all(client_fd[i], "OK\r\n");
                    printf("[%s] Client %d selected MODE RECV\n", gettime(), i);
                }
            }
            else if (strncmp(req, "FROM ", 5) == 0) {
                if (state[i] != MODE_SEND) {
                    send_all(client_fd[i], "ERR Bad sequence\r\n");
                    FD_CLR(client_fd[i], &master_set);
                    close(client_fd[i]);
                    client_fd[i] = -1;
                } else {
                    state[i] = FROM;
                    msg[i].from = strdup(req + 5);
                    send_all(client_fd[i], "OK Sender accepted\r\n");
                }
            }
            else if (strncmp(req, "TO ", 3) == 0) {
                if (state[i] != FROM && state[i] != TO) {
                    // as multiple recipients are allowed
                    send_all(client_fd[i], "ERR Bad sequence\r\n");
                    FD_CLR(client_fd[i], &master_set);
                    close(client_fd[i]);
                    client_fd[i] = -1;
                } else {
                    // change state only if this recipient is valid
                    // OK Recipient accepted or ERR No such user
                    char* recipient = strdup(req + 3);
                    bool found = (findindex(users_info, recipient) != -1);
                    if (!found) {
                        send_all(client_fd[i], "ERR No such user\r\n");
                    }
                    else {
                        state[i] = TO;
                        // add recipient to msg[i].to
                        int count = 0;
                        if (msg[i].to) {
                            while (msg[i].to[count]) count++;
                        }
                        msg[i].to = realloc(msg[i].to, sizeof(char*) * (count + 2));
                        msg[i].to[count] = strdup(recipient);
                        msg[i].to[count + 1] = NULL;
                        send_all(client_fd[i], "OK Recipient accepted\r\n");
                    }
                }
            }
            else if (strncmp(req, "SUB ", 4) == 0) {
                if (state[i] != TO) {
                    send_all(client_fd[i], "ERR Bad sequence\r\n");
                    FD_CLR(client_fd[i], &master_set);
                    close(client_fd[i]);
                    client_fd[i] = -1;
                } else {
                    state[i] = SUB;
                    msg[i].subject = strdup(req + 4);
                    send_all(client_fd[i], "OK Subject accepted\r\n");
                }
            }
            else if (strcmp(req, "BODY") == 0) {
                // OK Send body, end with CRLF.CRLF
                if (state[i] != SUB) {
                    send_all(client_fd[i], "ERR Bad sequence\r\n");
                    FD_CLR(client_fd[i], &master_set);
                    close(client_fd[i]);
                    client_fd[i] = -1;
                } else {
                    state[i] = BODY;
                    send_all(client_fd[i], "OK Send body, end with CRLF.CRLF\r\n");
                }
            }
            else if (strcmp(req, "QUIT") == 0) {
                // clear everything in msg[i] and set state to QUIT
                send_all(client_fd[i], "BYE\r\n");
                free(msg[i].from);
                msg[i].from = NULL;
                free(msg[i].subject);
                msg[i].subject = NULL;
                if (msg[i].to) {
                    for (int j = 0; msg[i].to[j]; j++) {
                        free(msg[i].to[j]);
                    }
                    free(msg[i].to);
                    msg[i].to = NULL;
                }
                msg[i].body = NULL;
                state[i] = QUIT;
                printf("[%s] Client %d disconnected gracefully.\n", gettime(), i);
                close(client_fd[i]);
                FD_CLR(client_fd[i], &master_set);
                client_fd[i] = -1;
            }
            else {
                // keep appending to body until we get the terminator CRLF.CRLF
                if (state[i] != BODY) {
                    send_all(client_fd[i], "ERR Bad sequence\r\n");
                    FD_CLR(client_fd[i], &master_set);
                    close(client_fd[i]);
                    client_fd[i] = -1;
                } else {
                    if (strcmp(req, ".\r\n") == 0) {
                        // save the message to the mailboxes of all recipients
                        int recipient_count = 0;
                        for (int j = 0; msg[i].to[j]; j++) {
                            char path[MAX_LINE];
                            int k = findindex(users_info, msg[i].to[j]);
                            users_info[k].inboxCnt++;
                            snprintf(path, MAX_LINE, "/mailboxes/%s/%d.txt", msg[i].to[j], users_info[k].inboxCnt);
                            FILE* fptr = fopen(path, "a");
                            if (fptr) {
                                // use gettime to get the current time
                                char* ttime = gettime();
                                // write MULTIPLE fprintf for better readability
                                fprintf(fptr, "From: %s\n", msg[i].from);
                                fprintf(fptr, "To: %s\n", msg[i].to[j]);
                                fprintf(fptr, "Subject: %s\n", msg[i].subject);
                                fprintf(fptr, "Date: %s\n", ttime);
                                fprintf(fptr, "---\n");
                                for (int j = 0; msg[i].body[j]; j++) {
                                    fprintf(fptr, "%s", msg[i].body[j]); // ".\r\n" is not appended to body
                                }
                                free(ttime);
                                fclose(fptr);
                                recipient_count++;
                            }
                        }
                        char res[MAX_LINE];
                        snprintf(res, MAX_LINE, "OK Delivered to %d mailboxes\r\n", recipient_count);
                        send_all(client_fd[i], res);
                        printf("[%s] Mail delivered from \"%s\" to [", gettime(), msg[i].from);
                        for (int j = 0; msg[i].to[j]; j++) {
                            printf("%s", msg[i].to[j]);
                            if (msg[i].to[j + 1]) printf(", ");
                        }
                        printf("] (%d recipient%s)\n", recipient_count, recipient_count > 1 ? "s" : "");
                    }
                    else {
                        // append this line to body
                        // body is multiple lines, char**
                        char* line = req;
                        if (line[0] == '.') line++; // if the line starts with '.', remove it (dot-stuffing)
                        int len = strlen(req);
                        int count = 0;
                        if (msg[i].body) {
                            while (msg[i].body[count]) count++;
                            msg[i].body = realloc(msg[i].body, (count + 2) * sizeof(char*));
                            msg[i].body[count] = malloc(len + 1);
                            strcpy(msg[i].body[count], line);
                            msg[i].body[count + 1] = NULL;
                        } else {
                            msg[i].body = malloc(2 * sizeof(char*));
                            msg[i].body[0] = malloc(len + 1);
                            strcpy(msg[i].body[0], line);
                            msg[i].body[1] = NULL;
                        }
                    }
                }
            }
        }
    }
    exit(0);
}