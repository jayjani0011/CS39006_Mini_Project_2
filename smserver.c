#define _DEFAULT_SOURCE
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
#include <dirent.h>

// not using const int here, because we need compile-time constraints for array sizes
#define MAX_CLIENTS 10
#define MAX_LINE 509 // as a dot-stuffed line will be .<line>\r\n
#define MAX_LINE_SNPRINTF 800 // to leave space for \r\n and \0
#define MAX_ATTEMPTS 3

int nusers = 0;

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
    int id; // unique id for each message, can be generated using a global counter that increments with each new message
    char from[40];
    char to[2 * MAX_CLIENTS][21]; // array of recipient usernames, max MAX_CLIENTS recipients
    char subject[100];
    char body[50][100]; // assuming max 10 lines of body, each up to 100 characters
    char date[20];
} messageinfo;

typedef struct {
    char username[21];
    char password[31];
    int inboxCnt;
    int unique_id;
    messageinfo inbox[100]; // keep adding messages to this, when LIST command is received, just return the info in this array
} usinfo;


typedef struct {
    int fd;
    char username[50];
    char nonce[9];
    enum state state;
    messageinfo msg;
    int auth_chances; // to limit the number of authentication attempts
} cliinfo;

void copymsg(messageinfo* dest, messageinfo* src) {
    dest->id = src->id;
    strcpy(dest->from, src->from);
    int to_count = 0;
    for (int i = 0; i < MAX_CLIENTS && src->to[i][0] != '\0'; i++) {
        strcpy(dest->to[i], src->to[i]);
        to_count++;
    }
    dest->to[to_count][0] = '\0';
    strcpy(dest->subject, src->subject);
    // no need to copy body, we will read it from the file when the client sends the READ command, to save memory
}

void add_message_to_inbox(messageinfo* msgs, messageinfo newmsg, int count) {
    copymsg(&msgs[count - 1], &newmsg);
    msgs[count].from[0] = '\0'; // mark the end of the array
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
    fprintf(fptr, "C: %s\n", buf);
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
    snprintf(path, MAX_LINE, "./mailboxes/%s", name);

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
            if ((space[i] < 'a' || space[i] > 'z') && (space[i] < '0' || space[i] > '9') && (space[i] < 'A' || space[i] > 'Z')) bad = true;
        }
        if (bad) continue;
        check_directory(name);
        strcpy(users_info[nusers].username, name);
        strcpy(users_info[nusers].password, space + 1);
        // update users_info[nusers].inboxCnt by counting the number of files in the directory ./mailboxes/<username>/
        char path[MAX_LINE];
        snprintf(path, MAX_LINE, "./mailboxes/%s", name);
        int count = 0;
        struct stat st = {0};
        if (stat(path, &st) == -1) {
            perror("stat error");
            exit(1);
        }
        if (S_ISDIR(st.st_mode)) {
            // It's a directory, count the number of files in it
            DIR* dir = opendir(path);
            if (dir == NULL) {
                perror("opendir error");
                exit(1);
            }
            struct dirent* entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_type == DT_REG) { // Only count regular files
                    count++;
                }
            }
            closedir(dir);
        }
        // and users_info[nusers].inbox by reading the info of each message from these files
        count--; // as 0.txt is not to be counted
        users_info[nusers].inboxCnt = count;
        int idx = 0;
        DIR* dir = opendir(path);
        if (dir == NULL) {
            perror("opendir error");
            exit(1);
        }
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) { // Only count regular files
                char msg_path[MAX_LINE_SNPRINTF];
                snprintf(msg_path, MAX_LINE_SNPRINTF, "%s/%s", path, entry->d_name);
                FILE* msg_file = fopen(msg_path, "r");
                if (msg_file == NULL) {
                    perror("fopen error");
                    exit(1);
                }
                // if d->name is 0.txt, initialize unique_id to the number written in it
                if (strcmp(entry->d_name, "0.txt") == 0) {
                    char id_str[10];
                    fgets(id_str, 10, msg_file);
                    id_str[strcspn(id_str, "\n")] = '\0';
                    users_info[nusers].unique_id = atoi(id_str);
                    fclose(msg_file);
                    continue;
                }
                char line[MAX_LINE];
                messageinfo *msg = &users_info[nusers].inbox[idx++];
                msg->id = atoi(entry->d_name); // filename is the id of the message
                fgets(line, MAX_LINE, msg_file);
                line[strcspn(line, "\n")] = '\0';
                strcpy(msg->from, line + 6); // first line is From: <name>
                fgets(line, MAX_LINE, msg_file);
                line[strcspn(line, "\n")] = '\0';
                strcpy(msg->to[0], line + 4); // second line is To: <name>
                fgets(line, MAX_LINE, msg_file);
                line[strcspn(line, "\n")] = '\0';
                strcpy(msg->subject, line + 9); // third line is Subject: <subject>
                fgets(line, MAX_LINE, msg_file);
                line[strcspn(line, "\n")] = '\0';
                strcpy(msg->date, line + 6); // fourth line is Date: <date>
                fclose(msg_file);
            }
        }
        closedir(dir);
        nusers++;
    }
    fclose(fptr);

    printf("[%s] Loaded %d users from %s\n", gettime(), nusers, filename);
}

int findindex(usinfo *users_info, char* name) {
    for (int i = 0; i < nusers; i++) {
        if (strcmp(users_info[i].username, name) == 0) return i;
    }
    return -1;
}

void reset_client(cliinfo* client) {
    client->msg.from[0] = '\0';
    client->msg.subject[0] = '\0';
    client->msg.to[0][0] = '\0';
    client->msg.body[0][0] = '\0';
    client->msg.date[0] = '\0';
    client->msg.id = 0;
    client->state = QUIT;
    client->fd = -1;
    client->auth_chances = MAX_ATTEMPTS;
    client->username[0] = '\0';
    client->nonce[0] = '\0';
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

    #ifdef DEBUG
    // clear debug file
    FILE* fptr = fopen("debug.log", "w");
    fclose(fptr);
    #endif

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
    if (bind(listen_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
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
        reset_client(clients + i);
    }
    
    printf("[%s] Waiting for clients to connect...\n", gettime());

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
                        reset_client(&clients[i]);
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
                reset_client(&clients[i]);
                continue;
            }
            // handle the client request here
            if (strcmp(req, "MODE RECV") == 0) {
                if (clients[i].state != QUIT) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    reset_client(&clients[i]);
                } else {
                    clients[i].state = MODE_RECV;
                    send_all(clients[i].fd, "OK\r\n");
                    char nonce[9];
                    for (int i = 0; i < 8; i++) {
                        srand(time(NULL) + i); // seed rand with current time + i to get different nonce for different clients
                        nonce[i] = "0123456789abcdefghijklmnopqrstuvwxyz"[rand() % 36];
                    }
                    nonce[8] = '\0';
                    strcpy(clients[i].nonce, nonce);
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
                    reset_client(&clients[i]);
                } else {
                    char* space1 = strchr(req + 5, ' ');
                    if (!space1) {
                        send_all(clients[i].fd, "ERR Bad sequence\r\n");
                        FD_CLR(clients[i].fd, &master_set);
                        close(clients[i].fd);
                        reset_client(&clients[i]);
                    } else {
                        *space1 = '\0';
                        char* username = req + 5;
                        char* password_hash = space1 + 1;
                        int idx = findindex(users_info, username);
                        if (idx == -1) {
                            send_all(clients[i].fd, "ERR No such user\r\n");
                            FD_CLR(clients[i].fd, &master_set);
                            close(clients[i].fd);
                            reset_client(&clients[i]);
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
                                strcpy(clients[i].username, username);
                                char welcome_msg[MAX_LINE];
                                snprintf(welcome_msg, MAX_LINE, "OK Welcome %s\r\n", username);
                                send_all(clients[i].fd, welcome_msg);
                                printf("[%s] Authentication successful for user %s\n", gettime(), username);
                            } else {
                                clients[i].auth_chances--;
                                char auth_fail_msg[MAX_LINE];
                                snprintf(auth_fail_msg, MAX_LINE, "ERR Authentication failed %d\r\n", clients[i].auth_chances);
                                send_all(clients[i].fd, auth_fail_msg);
                                if (clients[i].auth_chances == 0) {
                                    FD_CLR(clients[i].fd, &master_set);
                                    close(clients[i].fd);
                                    reset_client(&clients[i]);
                                    printf("[%s] Authentication failed for user %s, remaining attempts: %d, close connection\n", gettime(), username, clients[i].auth_chances);
                                }
                                else {
                                    printf("[%s] Authentication failed for user %s, remaining attempts: %d\n", gettime(), username, clients[i].auth_chances);
                                }
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
                    reset_client(&clients[i]);
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
                    reset_client(&clients[i]);
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
                        // assignment pdf has not used any delimiters, but since subject & from can have spaces, we need a delimiter to separate these fields, let's use '~'
                        snprintf(list_msg, MAX_LINE, "%d~%s~%s~%s\r\n", users_info[idx].inbox[j].id, users_info[idx].inbox[j].from, users_info[idx].inbox[j].subject, users_info[idx].inbox[j].date);
                        send_all(clients[i].fd, list_msg);
                    }
                }
            }
            else if (strncmp(req, "READ ", 5) == 0) {
                if (clients[i].state != AUTHENTICATED) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    reset_client(&clients[i]);
                } else {
                    int msg_id = atoi(req + 5);
                    // fetch the message from the path /mailboxes/<username>/<msg_id>.txt and just sent it as it is
                    // send each line of the body with \r\n and end with .\r\n
                    // need to use dot-stuffing if any line in the body starts with '.', we will prepend another '.' to it, and the client will remove it when receiving

                    char path[MAX_LINE];
                    snprintf(path, MAX_LINE, "./mailboxes/%s/%d.txt", clients[i].username, msg_id);
                    FILE* fptr = fopen(path, "r");

                    if (fptr == NULL) {
                        send_all(clients[i].fd, "ERR No such message\r\n");
                    } else {
                        send_all(clients[i].fd, "OK\r\n");
                        char line[MAX_LINE];
                        while (fgets(line, MAX_LINE, fptr) != NULL) {
                            // apply dot-stuffing
                            line[strcspn(line, "\n")] = '\0';
                            char stuffed_line[MAX_LINE + 3];
                            if (line[0] == '.') {
                                snprintf(stuffed_line, MAX_LINE + 3, ".%s\r\n", line);
                            } else {
                                snprintf(stuffed_line, MAX_LINE + 3, "%s\r\n", line);
                            }
                            send_all(clients[i].fd, stuffed_line);
                        }
                        send_all(clients[i].fd, ".\r\n");
                        fclose(fptr);
                        printf("[%s] User %s READ message %d\n", gettime(), clients[i].username, msg_id);
                    }
                }
            }
            else if (strncmp(req, "DELETE ", 7) == 0) {
                if (clients[i].state != AUTHENTICATED) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    reset_client(&clients[i]);
                } else {
                    int idx = findindex(users_info, clients[i].username);
                    int msg_id = atoi(req + 7);
                    char path[MAX_LINE];
                    snprintf(path, MAX_LINE, "./mailboxes/%s/%d.txt", clients[i].username, msg_id);
                    if (remove(path) == 0) {
                        // also remove this message from users_info[idx].inbox and decrease inboxCnt
                        for (int j = 0; j < users_info[idx].inboxCnt; j++) {
                            if (users_info[idx].inbox[j].id == msg_id) {
                                // shift all messages after this to the left by one
                                for (int k = j; k < users_info[idx].inboxCnt - 1; k++) {
                                    // users_info[idx].inbox[k] = users_info[idx].inbox[k + 1]; can't do this directly noob!
                                    users_info[idx].inbox[k].id = users_info[idx].inbox[k + 1].id;
                                    strcpy(users_info[idx].inbox[k].from, users_info[idx].inbox[k + 1].from);
                                    int to_count = 0;
                                    while (users_info[idx].inbox[k + 1].to[to_count][0] != '\0') to_count++;
                                    for (int t = 0; t < to_count; t++) {
                                        strcpy(users_info[idx].inbox[k].to[t], users_info[idx].inbox[k + 1].to[t]);
                                    }
                                    users_info[idx].inbox[k].to[to_count][0] = '\0';
                                    strcpy(users_info[idx].inbox[k].subject, users_info[idx].inbox[k + 1].subject);
                                    strcpy(users_info[idx].inbox[k].date, users_info[idx].inbox[k + 1].date);
                                }
                                users_info[idx].inboxCnt--;
                                users_info[idx].inbox[users_info[idx].inboxCnt].from[0] = '\0'; // mark the end of the array
                                break;
                            }
                        }
                        send_all(clients[i].fd, "OK Deleted\r\n");
                        printf("[%s] User %s DELETE message %d\n", gettime(), clients[i].username, msg_id);
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
                    reset_client(&clients[i]);
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
                    reset_client(&clients[i]);
                } else {
                    clients[i].state = FROM;
                    strcpy(clients[i].msg.from, req + 5);
                    send_all(clients[i].fd, "OK Sender accepted\r\n");
                }
            }
            else if (strncmp(req, "TO ", 3) == 0) {
                if (clients[i].state != FROM && clients[i].state != TO) {
                    // as multiple recipients are allowed
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    reset_client(&clients[i]);
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
                            while (clients[i].msg.to[count][0] != '\0') count++;
                        }
                        strcpy(clients[i].msg.to[count], recipient);
                        clients[i].msg.to[count + 1][0] = '\0';
                        send_all(clients[i].fd, "OK Recipient accepted\r\n");
                    }
                }
            }
            else if (strncmp(req, "SUB ", 4) == 0) {
                if (clients[i].state != TO) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    reset_client(&clients[i]);
                } else {
                    clients[i].state = SUB;
                    strcpy(clients[i].msg.subject, req + 4);
                    send_all(clients[i].fd, "OK Subject accepted\r\n");
                }
            }
            else if (strcmp(req, "BODY") == 0) {
                // OK Send body, end with CRLF.CRLF
                if (clients[i].state != SUB) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    reset_client(&clients[i]);
                } else {
                    clients[i].state = BODY;
                    send_all(clients[i].fd, "OK Send body, end with CRLF.CRLF\r\n");
                }
            }
            else if (strcmp(req, "QUIT") == 0) {
                // clear everything in msg[i] and set state to QUIT
                send_all(clients[i].fd, "BYE\r\n");
                close(clients[i].fd);
                FD_CLR(clients[i].fd, &master_set);
                reset_client(&clients[i]);
                printf("[%s] Client %d disconnected gracefully.\n", gettime(), i);
            }
            else {
                // keep appending to body until we get the terminator CRLF.CRLF
                if (clients[i].state != BODY) {
                    send_all(clients[i].fd, "ERR Bad sequence\r\n");
                    FD_CLR(clients[i].fd, &master_set);
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else {
                    if (strcmp(req, ".") == 0) {
                        // save the message to the mailboxes of all recipients
                        int recipient_count = 0;
                        for (int j = 0; clients[i].msg.to[j][0] != '\0'; j++) {
                            char path[MAX_LINE];
                            int k = findindex(users_info, clients[i].msg.to[j]);
                            users_info[k].inboxCnt++; // this will decrease on deletion, but unique_id will always increase, so we can use unique_id to generate unique filenames for each message
                            users_info[k].unique_id++; // increment unique_id for each new message
                            snprintf(path, MAX_LINE, "./mailboxes/%s/%d.txt", clients[i].msg.to[j], users_info[k].unique_id);
                            FILE* fptr = fopen(path, "w");
                            printf("[%s] Delivering mail to %s's mailbox with id %d\n", gettime(), clients[i].msg.to[j], users_info[k].unique_id);
                            if (fptr) {
                                // use gettime to get the current time
                                strcpy(clients[i].msg.date, gettime());
                                fprintf(fptr, "From: %s\n", clients[i].msg.from);
                                fprintf(fptr, "To: %s\n", clients[i].msg.to[j]);
                                fprintf(fptr, "Subject: %s\n", clients[i].msg.subject);
                                fprintf(fptr, "Date: %s\n", clients[i].msg.date);
                                fprintf(fptr, "---\n");
                                for (int j = 0; clients[i].msg.body[j][0] != '\0'; j++) {
                                    fprintf(fptr, "%s\n", clients[i].msg.body[j]); // ".\r\n" is not appended to body
                                }
                                fclose(fptr);
                                recipient_count++;
                                add_message_to_inbox(users_info[k].inbox, clients[i].msg, users_info[k].inboxCnt);
                                printf("[%s] Mail added to %s's inbox in memory\n", gettime(), clients[i].msg.to[j]);
                            }
                            // change the unique_id in 0.txt to the new unique_id
                            char id_path[MAX_LINE];
                            snprintf(id_path, MAX_LINE, "./mailboxes/%s/0.txt", clients[i].msg.to[j]);
                            FILE* id_fptr = fopen(id_path, "w");
                            if (id_fptr) {
                                fprintf(id_fptr, "%d", users_info[k].unique_id);
                                fclose(id_fptr);
                            }
                            else {
                                perror("fopen error");
                                continue;
                            }
                        }
                        char res[MAX_LINE];
                        snprintf(res, MAX_LINE, "OK Delivered to %d mailboxes\r\n", recipient_count);
                        send_all(clients[i].fd, res);
                        printf("[%s] Mail delivered from \"%s\" to [", gettime(), clients[i].msg.from);
                        for (int j = 0; clients[i].msg.to[j][0] != '\0'; j++) {
                            printf("%s", clients[i].msg.to[j]);
                            if (clients[i].msg.to[j + 1][0] != '\0') printf(", ");
                        }
                        printf("] (%d recipient%s)\n", recipient_count, recipient_count > 1 ? "s" : "");
                        clients[i].state = QUIT; // clearing will be done when QUIT command is received, but we can set state to QUIT here to avoid accepting more lines for this message
                    }
                    else {
                        // append this line to body
                        // body is multiple lines, char**
                        char* line = req;
                        if (line[0] == '.') line++; // if the line starts with '.', remove it (dot-stuffing)
                        int count = 0;
                        while (clients[i].msg.body[count][0] != '\0') count++;
                        strcpy(clients[i].msg.body[count], line);
                        clients[i].msg.body[count + 1][0] = '\0'; // mark the end of the array
                    }
                }
            }
        }
    }
    exit(0);
}