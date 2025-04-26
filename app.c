#include <sys/types.h>  
#include <sys/ipc.h>    
#include <sys/msg.h>  
#include <stdio.h>      
#include <stdlib.h>    
#include <string.h>    
#include <unistd.h>    
#include <ctype.h>

#define MAX_GROUPS 30
typedef struct{
	long mtype; //using mtype = 1 here
	int group_Number;
} Msg_GrpToApp;

Msg_GrpToApp message; 
//function to get ipc_key
int getkey(FILE *file){
    int key=0;
    int count=0;
	
	while(count<3){
	
			int scan_status=0;
			scan_status=fscanf(file,"%d",&key);
			if(scan_status!=1){
				perror("error getting ipc key value.");
				fclose(file);
				exit(1);
			}
			
		count++;
	}
    return key;
}

void cleanup_msgqueues(int msgid) {
    if (msgctl(msgid, IPC_RMID, NULL) == -1) {
        perror("msgctl failed");
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) { 
        printf("Usage: %s <test_case_number>\n", argv[0]);
        return 1;
    }
    
    char filepath[256];
    
    snprintf(filepath, sizeof(filepath), "testcase_%s/input.txt", argv[1]); //Creating file path

    int N; // Number of groups to be created

    
    FILE *file = fopen(filepath, "r"); //opening file in parent process 
    if (file == NULL) {
        perror("Error opening file");
        return EXIT_FAILURE;
    }
    
    if (fscanf(file, "%d", &N) != 1) {
        printf("Error reading integer from file\n");
        fclose(file);
        return EXIT_FAILURE;
    }
    rewind(file);  //file pointer moves to starting point

    if (N > MAX_GROUPS) {
        printf("Can't create more than %d groups\n", MAX_GROUPS);
        return EXIT_FAILURE;
    }

    // For each group, fork a child process
    for (int i = 0; i < N; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("Forking a group failed");
            return EXIT_FAILURE;
        } else if (pid == 0) {  // Child process
            // Each child reopens the input file so that it starts from the beginning.
            FILE *childFile = fopen(filepath, "r");
            if (childFile == NULL) {
                perror("Error opening file in child");
                exit(EXIT_FAILURE);
            }
            
            
            int group_id = i + 1;
            int target_line = 5 + group_id;
            int current_line = 0;
            char line[256];
            
            // Skip lines until reaching the target line.
            while (fgets(line, sizeof(line), childFile)) { 
                current_line++;
                if (current_line == target_line) {
                    line[strcspn(line, "\n")] = 0; // Remove the newline character at the end
                    //printf("%s\n", line);   
                    break;
                }
            }
            if (current_line < target_line) {
                printf("Error: group_id %d exceeds available groups.\n", group_id);
                fclose(childFile);
                exit(EXIT_FAILURE);
            }
            char *group_file_name = strrchr(line, '/');
            if (!group_file_name) {
                printf("Invalid group file path.\n\n");
                return EXIT_FAILURE;
            }
            group_file_name++; //Moving past '/'
            
            //Extracting groupNumber , but we aren't using it.
            int groupNumber;
            if (sscanf(line, "groups/group_%d.txt", &groupNumber) == 1) {
                //  printf("%d\n", groupNumber);
            } else {
                printf("Failed to extract group number.\n");
            }
            fclose(childFile);

            char arg2[20];
            snprintf(arg2, sizeof(arg2), "%d", groupNumber);
            
            // sending testcase number and string containing "group_groupNumber"
            execl("./groups.out", "groups.out", argv[1], arg2, NULL);            //***Changed group_file_name to groupNumber
            perror("Error executing groups.out");
            exit(EXIT_FAILURE);
        }
    }
    
    int ipc_key = getkey(file);
    // printf("%d",ipc_key);
    // Tracking Active Groups
    int active_groups = N;
    int msgid = msgget(ipc_key, 0666 | IPC_CREAT);
    	if (msgid == -1) {
		perror("msgget failed");
		exit(1);
    	}
        // Receiving messages from groups.c
	while(1){
        int receive_status= msgrcv(msgid, &message, sizeof(message) - sizeof(long), 1, 0);
        if (receive_status == -1) {
                perror("msgrcv failed");
                exit(1);
            }
            else{
                active_groups -- ;
                printf("All users terminated. Exiting group process %d \n",message.group_Number);
                //printf("No.of active groups:%d \n",active_groups); 
            }
        
        if(active_groups == 0){
            printf("App terminated!!!");
            break;
        }
    }
    //signal(SIGINT, cleanup_handler);
    
    // Add proper wait for child processes
   
    
    // Cleanup message queues before exit
    cleanup_msgqueues(msgid);
    return 0;
}
