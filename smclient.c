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
    // assumes msg already has "\r\n" in it
    int len = strlen(msg), sent = 0;
    while (sent < len) {
        int n = send(fd, msg + sent, len - sent, 0);
        if (n <= 0) {
            printf("***Send error\n");
            close(fd);
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
        if (n == 0) {
            printf("***Connection closed by Server abruptly\n");
            close(fd);
            exit(1);
        }
        if (n < 0) {
            printf("***Recv error\n");
            close(fd);
            exit(1);
        }
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

unsigned long djb2(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash;
}

void smtp2(int sockfd) {
    // send a mail
    // first send MODE SEND
    send_all(sockfd, "MODE SEND\r\n");
    char resp[MAX_LINE];
    recv_line(sockfd, resp);
    if (strcmp(resp, "OK") != 0) {
        printf("***Server error: %s\n", resp);
        close(sockfd);
        exit(1);
    }
    // then FROM <name>, TO <user>, SUB <subject>, BODY, QUIT.
    /*
    From (your name): _
    To (recipient username, empty line to finish): _
    Subject: _
    Body (type '.' on a line by itself to finish):
    _
    */
    char from[MAX_LINE], to[MAX_LINE], subject[MAX_LINE];
    printf("From (your name): ");
    fgets(from, MAX_LINE, stdin);
    from[strcspn(from, "\n")] = 0;
    char from_cmd[MAX_LINE];
    snprintf(from_cmd, MAX_LINE, "FROM %s\r\n", from);
    send_all(sockfd, from_cmd);
    recv_line(sockfd, resp);
    if (strcmp(resp, "OK Sender accepted") != 0) {
        printf("***Server error: %s\n", resp);
        close(sockfd);
        exit(1);
    }

    char to_cmd[MAX_LINE];
    int to_count = 0;
    while (true) {
        printf("To (recipient username, empty line to finish): ");
        fgets(to, MAX_LINE, stdin);
        to[strcspn(to, "\n")] = 0;
        if (strlen(to) == 0) {
            if (to_count > 0) break;
            else {
                printf("At least one recipient is required.\n");
                continue;
            }
        }
        snprintf(to_cmd, MAX_LINE, "TO %s\r\n", to);
        send_all(sockfd, to_cmd);
        recv_line(sockfd, resp);
        if (strcmp(resp, "OK Recipient accepted") == 0) {
            printf("\t-> Recipient '%s' accepted.\n", to);
            to_count++;
        }
        else if (strcmp(resp, "ERR No such user") == 0) {
            printf("\t-> Error: user '%s' does not exist on this server.\n", to);
        }
        else {
            printf("***Server error: %s\n", resp);
            close(sockfd);
            exit(1);
        }
    }

    printf("Subject: ");
    fgets(subject, MAX_LINE, stdin);
    subject[strcspn(subject, "\n")] = 0;
    char sub_cmd[MAX_LINE];
    snprintf(sub_cmd, MAX_LINE, "SUB %s\r\n", subject);
    send_all(sockfd, sub_cmd);
    recv_line(sockfd, resp);
    if (strcmp(resp, "OK Subject accepted") != 0) {
        printf("***Server error: %s\n", resp);
        close(sockfd);
        exit(1);
    }

    printf("Body (type '.' on a line by itself to finish):\n");
    send_all(sockfd, "BODY\r\n");
    recv_line(sockfd, resp);
    if (strcmp(resp, "OK Send body, end with CRLF.CRLF") != 0) {
        printf("***Server error: %s\n", resp);
        close(sockfd);
        exit(1);
    }
    char line[MAX_LINE];
    while (true) {
        fgets(line, MAX_LINE, stdin);
        line[strcspn(line, "\n")] = 0;
        char body_line[MAX_LINE];
        snprintf(body_line, MAX_LINE, "%s\r\n", line);
        // printf("\t-> Sending: %s\n", line);
        send_all(sockfd, body_line);
        if (strcmp(body_line, ".\r\n") == 0) {
            break;
        }
    }
    printf("Sending mail...\n");

    recv_line(sockfd, resp);
    if (strncmp(resp, "OK Delivered to ", 16) != 0) {
        printf("***Server error: %s\n", resp);
        close(sockfd);
        exit(1);
    }

    char* space = strchr(resp + 16, ' ');
    if (space) {
        space[0] = '\0';
    }
    to_count = atoi(resp + 16);
    printf("Mail delivered to %d recipient%s.\n", to_count, to_count > 1 ? "s" : "");
    // send QUIT command to server to end the session
    send_all(sockfd, "QUIT\r\n");
    recv_line(sockfd, resp);
    if (strcmp(resp, "BYE") != 0) {
        printf("***Server error: %s\n", resp);
        close(sockfd);
        exit(1);
    }
}

void handle_receiver(int sockfd, char* username) {
    send_all(sockfd, "COUNT\r\n");
    char resp[MAX_LINE];
    recv_line(sockfd, resp);
    if (strncmp(resp, "OK ", 3) != 0) {
        printf("***Server error: %s\n", resp);
        close(sockfd);
        exit(1);
    }
    int msg_count = atoi(resp + 3);
    printf("Mailbox for %s (%d messages)\n", username, msg_count);

    bool logged_in = true;
    while (logged_in) {
        /*Mailbox for alice (3 messages)
        1. List all messages
        2. Read a message
        3. Delete a message
        4. Logout
        >*/
        printf("\n1. List all messages\n");
        printf("2. Read a message\n");
        printf("3. Delete a message\n");
        printf("4. Logout\n");
        printf("> ");

        int choice;
        scanf("%d", &choice);
        while (getchar() != '\n'); // clear input buffer

        switch (choice) {
            case 1: {
                send_all(sockfd, "LIST\r\n");
                /* List like this here:
                    ID From Subject Date
                    --- ---- ------- ----
                    1 Alice Smith Meeting Tomorrow 2026-03-15 10:00:45
                */
                recv_line(sockfd, resp);
                if (strncmp(resp, "OK ", 3) != 0) {
                    printf("***Server error: %s\n", resp);
                    close(sockfd);
                    exit(1);
                }
                char* space = strchr(resp + 3, ' ');
                space[0] = '\0';
                printf("ID   From           Subject             Date\n");
                printf("---  ----           -------             ----\n");
                int listed_count = atoi(resp + 3);
                for (int i = 0; i < listed_count; i++) {
                    recv_line(sockfd, resp);
                    // parse the response using '~' as delimiter, as server sends in this format: <id>~<from>~<subject>~<date>
                    char* token = strtok(resp, "~");
                    printf("%-5s ", token); // id
                    token = strtok(NULL, "~");
                    printf("%-15s ", token); // from
                    token = strtok(NULL, "~");
                    printf("%-20s ", token); // subject
                    token = strtok(NULL, "~");
                    printf("%s\n", token); // date
                }
                break;
            }
            case 2: {
                printf("Enter message ID: ");
                int msg_id;
                scanf("%d", &msg_id);
                while (getchar() != '\n'); // clear input buffer
                char read_cmd[MAX_LINE];
                snprintf(read_cmd, MAX_LINE, "READ %d\r\n", msg_id);
                send_all(sockfd, read_cmd);
                recv_line(sockfd, resp);
                if (strncmp(resp, "OK", 2) != 0) {
                    printf("***Server error: %s\n", resp);
                    close(sockfd);
                    exit(1);
                }

                char res[MAX_LINE];
                while (true) {
                    recv_line(sockfd, res);
                    if (strcmp(res, ".") == 0) {
                        break;
                    }
                    // if the line starts with '.', remove it (dot-stuffing)
                    if (res[0] == '.') {
                        printf("%s\n", res + 1);
                    } else {
                        printf("%s\n", res);
                    }
                }
                break;
            }
            case 3: {
                printf("Enter message ID: ");
                int del_msg_id;
                scanf("%d", &del_msg_id);
                while (getchar() != '\n'); // clear input buffer
                char del_cmd[MAX_LINE];
                snprintf(del_cmd, MAX_LINE, "DELETE %d\r\n", del_msg_id);
                send_all(sockfd, del_cmd);
                recv_line(sockfd, resp);
                if (strcmp(resp, "OK Deleted") == 0) {
                    printf("Message %d deleted.\n", del_msg_id);
                }
                else {
                    printf("***Server error: %s\n", resp);
                    close(sockfd);
                    exit(1);
                }
                break;
            }
            case 4: {
                send_all(sockfd, "QUIT\r\n");
                recv_line(sockfd, resp);
                if (strcmp(resp, "BYE") != 0) {
                    printf("***Server error: %s\n", resp);
                    close(sockfd);
                    exit(1);
                }
                printf("Logged out.\n");
                logged_in = false;
                break;
            }
            default:
                printf("Invalid choice. Please try again.\n");
        }
    }
}

void smp(int sockfd) {
    // send MODE RECV command first
    send_all(sockfd, "MODE RECV\r\n");
    char recv_resp[MAX_LINE];
    recv_line(sockfd, recv_resp);
    if (strcmp(recv_resp, "OK") != 0) {
        printf("***Server error: %s\n", recv_resp);
        close(sockfd);
        exit(1);
    }

    recv_line(sockfd, recv_resp);
    if (strncmp(recv_resp, "AUTH REQUIRED ", 14) != 0) {
        printf("***Server error: %s\n", recv_resp);
        close(sockfd);
        exit(1);
    }

    char nonce[9];
    strncpy(nonce, recv_resp + 14, 8);
    nonce[8] = '\0';
    char username[21], password[31];
    
    do {
        printf("Username: ");
        fgets(username, 21, stdin);
        username[strcspn(username, "\n")] = 0;
        printf("Password: ");
        fgets(password, 31, stdin);
        password[strcspn(password, "\n")] = 0;
        char combined[MAX_LINE];
        snprintf(combined, MAX_LINE, "%s%s", password, nonce);
        unsigned long hash = djb2(combined);
        
        char auth_cmd[MAX_LINE]; // cannot pass password directly over the network, need to hash it first
        snprintf(auth_cmd, MAX_LINE, "AUTH %s %lu\r\n", username, hash);
        send_all(sockfd, auth_cmd);
        recv_line(sockfd, recv_resp);
        if (strncmp(recv_resp, "OK Welcome ", 11) == 0) {
            printf("%s\n", recv_resp + 3); // print welcome message from server
            break; // authenticated successfully
        }
        else if (strcmp(recv_resp, "ERR No such user") == 0) {
            printf("***Error: No such user '%s'. Please try again.\n", username);
            close(sockfd);
            exit(1);
        }
        else if (strncmp(recv_resp, "ERR Authentication failed ", 26) == 0) {
            int remaining_attempts = atoi(recv_resp + 26); // extract remaining attempts from the response
            printf("***Error: Authentication failed for user '%s'. Attempts left : %d.\n", username, remaining_attempts);
            if (remaining_attempts == 0) {
                printf("***Authentication failed for user '%s', no remaining attempts. Closing connection.\n", username);
                close(sockfd);
                exit(1);
            }
            else {
                // ask for username and password again, and resend the AUTH command with the same nonce
                printf("Please try again.\n");
            }
        }
        else {
            printf("***Server error: %s\n", recv_resp);
            send_all(sockfd, "QUIT\r\n");
            recv_line(sockfd, recv_resp);
            if (strcmp(recv_resp, "BYE") != 0) {
                printf("***Server error: %s\n", recv_resp);
            }
            close(sockfd);
            exit(1);
        }
    } while (1);

    handle_receiver(sockfd, username);
}

int connect_to_server(struct sockaddr_in* servaddr) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        close(sockfd);
        exit(1);
    }

    if (connect(sockfd, (struct sockaddr *)servaddr, sizeof(*servaddr)) < 0) {
        perror("connect");
        close(sockfd);
        exit(1);
    }
    return sockfd;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("***Usage : %s <server_ip> <PORT>\n", argv[0]);
        exit(1);
    }

    char* server_ip = strdup(argv[1]);
    int PORT = atoi(argv[2]);

    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_port = htons(PORT);
    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip, &servaddr.sin_addr);

    int sockfd = connect_to_server(&servaddr);
    char welcome_msg[MAX_LINE];
    recv_line(sockfd, welcome_msg);
    if (strcmp(welcome_msg, "WELCOME SimpleMail v1.0") != 0) {
        printf("***Server error: %s\n", welcome_msg);
        close(sockfd);
        exit(1);
    }
    else {
        printf("%s\n", welcome_msg); // print welcome message from server
    }

    // send/recv
    printf("Connected to SimpleMail server\n");
    
    /*Interface:
    Connected to SimpleMail server.
    1. Send a mail
    2. Check my mailbox
    3. Quit
    >
    */
   
    while (true) {
        printf("1. Send a mail\n");
        printf("2. Check my mailbox\n");
        printf("3. Quit\n");
        printf("> ");

        int choice;
        scanf("%d", &choice);
        while (getchar() != '\n'); // clear input buffer
        // use switch case for choice

        switch (choice) {
            case 1: {
                smtp2(sockfd);
                // reconnect to server for next operations, as server closes connection after QUIT
                close(sockfd);
                sockfd = connect_to_server(&servaddr);
                char welcome_msg[MAX_LINE];
                recv_line(sockfd, welcome_msg);
                if (strcmp(welcome_msg, "WELCOME SimpleMail v1.0") != 0) {
                    printf("***Server error: %s\n", welcome_msg);
                    close(sockfd);
                    exit(1);
                }
                break;
            }
            case 2: {
                smp(sockfd);
                // logged out from mailbox, so reconnect to server for next operations, as server closes connection after QUIT
                close(sockfd);
                sockfd = connect_to_server(&servaddr);
                char welcome_msg[MAX_LINE];
                recv_line(sockfd, welcome_msg);
                if (strcmp(welcome_msg, "WELCOME SimpleMail v1.0") != 0) {
                    printf("***Server error: %s\n", welcome_msg);
                    close(sockfd);
                    exit(1);
                }
                break;
            }
            case 3: {
                send_all(sockfd, "QUIT\r\n");
                char resp[MAX_LINE];
                recv_line(sockfd, resp);
                if (strcmp(resp, "BYE") != 0) {
                    printf("***Server error: %s\n", resp);
                    close(sockfd);
                    exit(1);
                }
                close(sockfd);
                printf("Goodbye!\n");
                exit(0);
                break;
    }
            default:
                printf("Invalid choice. Please try again.\n");
        }
    }

    exit(0);
}