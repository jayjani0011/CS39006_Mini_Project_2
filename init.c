#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Usage: %s <user_file>\n", argv[0]);
        return 1;
    }

    // make the mailboxes directory if it does not exist
    if (mkdir("./mailboxes", 0777) == -1) { // does it create the directory in the current directory?
        if (errno == EEXIST) {
            printf("Directory mailboxes already exists\n");
        } else {
        perror("Error creating directory");
            return 1;
        }
    } else {
        printf("Directory mailboxes created successfully\n");
    }

    char* filename = argv[1];
    FILE* fptr = fopen(filename, "r");
    if (fptr == NULL) {
        printf("File %s does not exist.\n", filename);
        return 1;
    }

    // scan users.txt and create a directory for each user in mailboxes
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
        char path[60];
        snprintf(path, 60, "./mailboxes/%s", name);
        if (mkdir(path, 0777) == -1) {
            if (errno == EEXIST) {
                printf("Directory %s already exists\n", name);
            } else {
                perror("Error creating directory");
                return 1;
            }
        } else {
            printf("Directory %s created successfully\n", name);
        }

        // insert a file 0.txt in this directory, and write the current mail number to be used, here = 1
        char path2[60];
        snprintf(path2, 60, "./mailboxes/%s/0.txt", name);
        // don't create the file if it already exists, otherwise it will reset the mail number to 0, and make this whole thing meaningless
        if (access(path2, F_OK) == -1) {
            FILE* fptr2 = fopen(path2, "w");
            if (fptr2 == NULL) {
                perror("Error creating file");
                return 1;
            }
            fprintf(fptr2, "0\n");
            fclose(fptr2);
        }
    }

    printf("Directories created successfully for all users in %s\n", filename);

    return 0;
}