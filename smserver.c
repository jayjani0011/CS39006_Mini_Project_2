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
    MODE_RECV,
    AUTHENTICATED,
    MODE_SEND,
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
    int unique_id;
    messageinfo* inbox; // keep adding messages to this, when LIST command is received, just return the info in this array
} usinfo;

typedef struct {
    int id; // unique id for each message, can be generated using a global counter that increments with each new message
    char* from;
    char** to;
    char* subject;
    char** body;
    char* date;
} messageinfo;

typedef struct {
    int fd;
    char* username;
    char* nonce;
    enum state state;
    messageinfo msg;
} cliinfo;

void copymsg(messageinfo* dest, messageinfo* src) {
    dest->id = src->id;
    dest->from = strdup(src->from);
    int to_count = 0;
    if (src->to) {
        while (src->to[to_count]) to_count++;
    }
    dest->to = malloc(sizeof(char*) * (to_count + 1));
    for (int i = 0; i < to_count; i++) {
        dest->to[i] = strdup(src->to[i]);
    }
    dest->to[to_count] = NULL;
    dest->subject = strdup(src->subject);
    // no need to copy body, we will read it from the file when the client sends the READ command, to save memory
}

void add_message_to_inbox(messageinfo* msgs, messageinfo newmsg) {
    int count = 0;
    if (msgs) {
        while (msgs[count].from) count++;
    }
    msgs = realloc(msgs, sizeof(messageinfo) * (count + 2));
    copymsg(&msgs[count], &newmsg);
    msgs[count + 1].from = NULL; // mark the end of the array
}


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
        if (c == '\n' && i >= 1 && buf[i - 1] == '\r') {
            i--;
            break; // got the terminator \r\n
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    #ifdef DEBUG
    // print in some other file
    FILE* fptr = fopen("debug.log", "a");
    fprintf(fptr, "S: %s\n", buf);
    fclose(fptr);
    #endif
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

void readfile(char* filename, usinfo users_info[]) {
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
        users_info[nclients].inbox = NULL;
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

unsigned long djb2(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash;
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
    usinfo users_info[MAX_CLIENTS];
    readfile(users, users_info);

    // start select() loop now
    fd_set master_set, read_set; // will be used in select()
    FD_ZERO(&master_set);
    FD_SET(listen_fd, &master_set);
    int max_fd = listen_fd;

    cliinfo clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].state = QUIT;
        clients[i].fd = -1;
        clients[i].msg.from = NULL;
        clients[i].msg.to = NULL;
        clients[i].msg.subject = NULL;
        clients[i].msg.body = NULL;
        clients[i].nonce = NULL;
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
                if (clients[i].fd == -1) {
                    struct sockaddr_in client_addr;
                    socklen_t len = sizeof(client_addr);
                    clients[i].fd = accept(listen_fd, (struct sockaddr *)&client_addr, &len);
                    if (clients[i].fd < 0) {
                        perror("accept");
                        clients[i].fd = -1;
                        break;
                    }
                    printf("[%s] New connection from %s : %d\n", gettime(), inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                    FD_SET(clients[i].fd, &master_set);
                    if (clients[i].fd > max_fd) max_fd = clients[i].fd;
                    send_all(clients[i].fd, "WELCOME SimpleMail v1.0\r\n");
                    break;
                }
            }
        }

        // Check if any client has sent something - then this
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd == -1 || !FD_ISSET(clients[i].fd, &read_set)) continue;
            char req[MAX_LINE];
            int n = recv_line(clients[i].fd, req);
            if (n == 0) {
                printf("[%s] Client %d disconnected abruptly.\n", gettime(), i);
                close(clients[i].fd);
                FD_CLR(clients[i].fd, &master_set);
                continue;
            }
            // handle the client request here
            if (strcmp(req, "MODE RECV") == 0) {
                if (clients[i].state != QUIT) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    clients[i].state = MODE_RECV;
                    send_all(clients[i].fd, "OK\r\n");
                    char nonce[9];
                    for (int i = 0; i < 8; i++) {
                        srand(time(NULL) + i); // seed rand with current time + i to get different nonce for different clients
                        nonce[i] = "0123456789abcdefghijklmnopqrstuvwxyz"[rand() % 36];
                    }
                    nonce[8] = '\0';
                    clients[i].nonce = strdup(nonce);
                    char auth_msg[MAX_LINE];
                    snprintf(auth_msg, MAX_LINE, "AUTH REQUIRED %s\r\n", nonce);
                    send_all(clients[i].fd, auth_msg);
                    printf("[%s] Client %d selected MODE RECV\n", gettime(), i);
                }
            }
            else if (strncmp(req, "AUTH ", 5) == 0) {
                // AUTH <username> <password_hash>
                if (clients[i].state != MODE_RECV) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    char* space1 = strchr(req + 5, ' ');
                    if (!space1) {
                        send_all(clients[i].fd, "ERR Bad sequence\r\n");
                        FD_CLR(clients[i].fd, &master_set);
                        close(clients[i].fd);
                        clients[i].fd = -1;
                    } else {
                        *space1 = '\0';
                        char* username = req + 5;
                        char* password_hash = space1 + 1;
                        int idx = findindex(users_info, username);
                        if (idx == -1) {
                            send_all(clients[i].fd, "ERR No such user\r\n");
                            FD_CLR(clients[i].fd, &master_set);
                            close(clients[i].fd);
                            clients[i].fd = -1;
                        } else {
                            // compute the hash of the password in users_info[idx].password with the nonce and compare with password_hash
                            char combined[80];
                            snprintf(combined, 80, "%s%s", users_info[idx].password, clients[i].nonce); // clients[i].nonce is the nonce
                            unsigned long hash_val = djb2(combined);
                            char computed_hash[80];
                            snprintf(computed_hash, 80, "%lu", hash_val);
                            // simple hash function: sum of ASCII values of combined mod 256, repeated 32 times in hex
                            if (strcmp(computed_hash, password_hash) == 0) {
                                clients[i].state = AUTHENTICATED;
                                char welcome_msg[MAX_LINE];
                                snprintf(welcome_msg, MAX_LINE, "OK Welcome %s\r\n", username);
                                send_all(clients[i].fd, welcome_msg);
                                printf("[%s] Authentication successful for user %s\n", gettime(), username);
                            } else {
                                send_all(clients[i].fd, "ERR Authentication failed\r\n");
                                FD_CLR(clients[i].fd, &master_set);
                                close(clients[i].fd);
                                clients[i].fd = -1;
                            }
                        }
                    }
                }
            }
            else if (strcmp(req, "COUNT") == 0) {
                if (clients[i].state != AUTHENTICATED) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    int idx = findindex(users_info, clients[i].username);
                    char list_msg[MAX_LINE];
                    snprintf(list_msg, MAX_LINE, "OK %d\r\n", users_info[idx].inboxCnt);
                    send_all(clients[i].fd, list_msg);
                }
            }
            else if (strcmp(req, "LIST") == 0) {
                if (clients[i].state != AUTHENTICATED) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    // send in this format
                    // OK <count> messages
                    // while (count--) <id> <from_display_name> <subject> <date>
                    int idx = findindex(users_info, clients[i].username);
                    char msg[MAX_LINE];
                    snprintf(msg, MAX_LINE, "OK %d messages\r\n", users_info[idx].inboxCnt);
                    send_all(clients[i].fd, msg);
                    for (int j = 0; j < users_info[idx].inboxCnt; j++) {
                        char list_msg[MAX_LINE];
                        // assignment pdf has not used any delimiters, but since subject & from can have spaces, we need a delimiter to separate these fields, let's use ';'
                        snprintf(list_msg, MAX_LINE, "%s;%s;%s;%s\r\n", users_info[idx].inbox[j].id, users_info[idx].inbox[j].from, users_info[idx].inbox[j].subject, users_info[idx].inbox[j].date);
                        send_all(clients[i].fd, list_msg);
                    }
                }
            }
            else if (strncmp(req, "READ ", 5) == 0) {
                if (clients[i].state != AUTHENTICATED) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    int idx = findindex(users_info, clients[i].username);
                    int msg_id = atoi(req + 5);
                    // fetch the message from the path /mailboxes/<username>/<msg_id>.txt and just sent it as it is
                    // send each line of the body with \r\n and end with .\r\n
                    // need to use dot-stuffing if any line in the body starts with '.', we will prepend another '.' to it, and the client will remove it when receiving

                    char path[MAX_LINE];
                    snprintf(path, MAX_LINE, "/mailboxes/%s/%d.txt", clients[i].username, msg_id);
                    FILE* fptr = fopen(path, "r");

                    if (fptr == NULL) {
                        send_all(clients[i].fd, "ERR No such message\r\n");
                    } else {
                        send_all(clients[i].fd, "OK\r\n");
                        char line[MAX_LINE];
                        while (fgets(line, MAX_LINE, fptr) != NULL) {
                            // apply dot-stuffing
                            if (line[0] == '.') {
                                char stuffed_line[MAX_LINE];
                                snprintf(stuffed_line, MAX_LINE, ".%s", line);
                                send_all(clients[i].fd, stuffed_line);
                            } else {
                                send_all(clients[i].fd, line);
                            }
                        }
                        send_all(clients[i].fd, ".\r\n");
                        fclose(fptr);
                    }
                }
            }
            else if (strncmp(req, "DELETE, ", 7) == 0) {
                if (clients[i].state != AUTHENTICATED) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    int idx = findindex(users_info, clients[i].username);
                    int msg_id = atoi(req + 7);
                    char path[MAX_LINE];
                    snprintf(path, MAX_LINE, "/mailboxes/%s/%d.txt", clients[i].username, msg_id);
                    if (remove(path) == 0) {
                        send_all(clients[i].fd, "OK Deleted\r\n");
                        // also remove this message from users_info[idx].inbox and decrease inboxCnt
                        for (int j = 0; j < users_info[idx].inboxCnt; j++) {
                            if (users_info[idx].inbox[j].id == msg_id) {
                                // shift all messages after this to the left by one
                                for (int k = j; k < users_info[idx].inboxCnt - 1; k++) {
                                    users_info[idx].inbox[k] = users_info[idx].inbox[k + 1];
                                }
                                users_info[idx].inboxCnt--;
                                break;
                            }
                        }
                    } else {
                        send_all(clients[i].fd, "ERR No such message\r\n");
                    }
                }
            }
            else if (strcmp(req, "MODE SEND") == 0) {
                if (clients[i].state != QUIT) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    clients[i].state = MODE_SEND;
                    send_all(clients[i].fd, "OK\r\n");
                    printf("[%s] Client %d selected MODE SEND\n", gettime(), i);
                }
            }
            else if (strncmp(req, "FROM ", 5) == 0) {
                if (clients[i].state != MODE_SEND) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    clients[i].state = FROM;
                    clients[i].msg.from = strdup(req + 5);
                    send_all(clients[i].fd, "OK Sender accepted\r\n");
                }
            }
            else if (strncmp(req, "TO ", 3) == 0) {
                if (clients[i].state != FROM && clients[i].state != TO) {
                    // as multiple recipients are allowed
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    // change state only if this recipient is valid
                    // OK Recipient accepted or ERR No such user
                    char* recipient = strdup(req + 3);
                    bool found = (findindex(users_info, recipient) != -1);
                    if (!found) {
                        send_all(clients[i].fd, "ERR No such user\r\n");
                    }
                    else {
                        clients[i].state = TO;
                        // add recipient to msg[i].to
                        int count = 0;
                        if (clients[i].msg.to) {
                            while (clients[i].msg.to[count]) count++;
                        }
                        clients[i].msg.to = realloc(clients[i].msg.to, sizeof(char*) * (count + 2));
                        clients[i].msg.to[count] = strdup(recipient);
                        clients[i].msg.to[count + 1] = NULL;
                        send_all(clients[i].fd, "OK Recipient accepted\r\n");
                    }
                }
            }
            else if (strncmp(req, "SUB ", 4) == 0) {
                if (clients[i].state != TO) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    clients[i].state = SUB;
                    clients[i].msg.subject = strdup(req + 4);
                    send_all(clients[i].fd, "OK Subject accepted\r\n");
                }
            }
            else if (strcmp(req, "BODY") == 0) {
                // OK Send body, end with CRLF.CRLF
                if (clients[i].state != SUB) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    clients[i].state = BODY;
                    send_all(clients[i].fd, "OK Send body, end with CRLF.CRLF\r\n");
                }
            }
            else if (strcmp(req, "QUIT") == 0) {
                // clear everything in msg[i] and set state to QUIT
                send_all(clients[i].fd, "BYE\r\n");
                free(clients[i].msg.from);
                clients[i].msg.from = NULL;
                free(clients[i].msg.subject);
                clients[i].msg.subject = NULL;
                if (clients[i].msg.to) {
                    for (int j = 0; clients[i].msg.to[j]; j++) {
                        free(clients[i].msg.to[j]);
                    }
                    free(clients[i].msg.to);
                    clients[i].msg.to = NULL;
                }
                clients[i].msg.body = NULL;
                clients[i].state = QUIT;
                printf("[%s] Client %d disconnected gracefully.\n", gettime(), i);
                close(clients[i].fd);
                FD_CLR(clients[i].fd, &master_set);
                clients[i].fd = -1;
            }
            else {
                // keep appending to body until we get the terminator CRLF.CRLF
                if (clients[i].state != BODY) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    if (strcmp(req, ".\r\n") == 0) {
                        // save the message to the mailboxes of all recipients
                        int recipient_count = 0;
                        for (int j = 0; clients[i].msg.to[j]; j++) {
                            char path[MAX_LINE];
                            int k = findindex(users_info, clients[i].msg.to[j]);
                            users_info[k].inboxCnt++; // this will decrease on deletion, but unique_id will always increase, so we can use unique_id to generate unique filenames for each message
                            users_info[k].unique_id++; // increment unique_id for each new message
                            snprintf(path, MAX_LINE, "/mailboxes/%s/%d.txt", clients[i].msg.to[j], users_info[k].unique_id);
                            FILE* fptr = fopen(path, "a");
                            if (fptr) {
                                // use gettime to get the current time
                                clients[i].msg.date = strdup(gettime());
                                add_message_to_inbox(users_info[k].inbox, clients[i].msg);
                                fprintf(fptr, "From: %s\n", clients[i].msg.from);
                                fprintf(fptr, "To: %s\n", clients[i].msg.to[j]);
                                fprintf(fptr, "Subject: %s\n", clients[i].msg.subject);
                                fprintf(fptr, "Date: %s\n", clients[i].msg.date);
                                fprintf(fptr, "---\n");
                                for (int j = 0; clients[i].msg.body[j]; j++) {
                                    fprintf(fptr, "%s\n", clients[i].msg.body[j]); // ".\r\n" is not appended to body
                                }
                                fclose(fptr);
                                recipient_count++;
                            }
                        }
                        char res[MAX_LINE];
                        snprintf(res, MAX_LINE, "OK Delivered to %d mailboxes\r\n", recipient_count);
                        send_all(clients[i].fd, res);
                        printf("[%s] Mail delivered from \"%s\" to [", gettime(), clients[i].msg.from);
                        for (int j = 0; clients[i].msg.to[j]; j++) {
                            printf("%s", clients[i].msg.to[j]);
                            if (clients[i].msg.to[j + 1]) printf(", ");
                        }
                        printf("] (%d recipient%s)\n", recipient_count, recipient_count > 1 ? "s" : "");
                        clients[i].state = QUIT; // clearing will be done when QUIT command is received, but we can set state to QUIT here to avoid accepting more lines for this message
                    }
                    else {
                        // append this line to body
                        // body is multiple lines, char**
                        char* line = req;
                        if (line[0] == '.') line++; // if the line starts with '.', remove it (dot-stuffing)
                        int len = strlen(req);
                        int count = 0;
                        if (clients[i].msg.body) {
                            while (clients[i].msg.body[count]) count++;
                            clients[i].msg.body = realloc(clients[i].msg.body, (count + 2) * sizeof(char*));
                            clients[i].msg.body[count] = malloc(len + 1);
                            strcpy(clients[i].msg.body[count], line);
                            clients[i].msg.body[count + 1] = NULL;
                        } else {
                            clients[i].msg.body = malloc(2 * sizeof(char*));
                            clients[i].msg.body[0] = malloc(len + 1);
                            strcpy(clients[i].msg.body[0], line);
                            clients[i].msg.body[1] = NULL;
                        }
                    }
                }
            }
        }
    }
    exit(0);
}