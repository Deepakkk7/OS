#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/ipc.h>    
#include <sys/msg.h>  
#include <unistd.h>  


#define MAX_FILTERED_WORDS 50
#define MAX_FILTERED_WORD_LENGTH 20
#define MAX_TIMESTAMP 2147000000
#define MAX_TEXT_SIZE 256
#define MAX_GRP_SIZE 30

int TrackViolations[30][50]={{0}}; // Tracks total violations of all users in all groups
int Threshold; // stores threshold value given in input.txt file of a testcase
int filteredWordCount; //stores number of filtered words in filtered_words.txt for testcase 
char** filteredWords=NULL; //To store the filtered words
int NumberOfGroups; // Number of groups in a test case

//struct used to communicate with groups.c using message queue
typedef struct{
	long mtype;
	int group_id;
	int user_id;
	int timestamp;
	char mtext[256];
	int Delete_user;
	int Group_status;
}msg;

msg message; // global variable to store the message of type msg received from groups.c 

//Function to convert string to lowercase
void toLowerCase(char *str) {
    for (int i = 0; str[i]; i++)
        str[i] = tolower(str[i]);
}

//To get key for the message queue between groups.c and moderator.c from input.txt file (also stores values of No.of groups and Threshold in global variables)
int getInputParameters(FILE * file){
	
	int key=0;
	int violations_threshold=0;
	int count=0;
	
	while(count<4){
			int scan_status=0;
			if(count==0){
				
				scan_status=fscanf(file,"%d",&NumberOfGroups);// no.of groups in testcase
				if(scan_status!=1){
				perror("error getting number of groups value.");
				fclose(file);
				exit(1);
			        }
			        count++;
				continue;
			}
			
			scan_status=fscanf(file,"%d",&key); // ipc key value for message queue between groups.c and moderator.c
			if(scan_status!=1){
				perror("error getting ipc key value.");
				fclose(file);
				exit(1);
			}
			
		count++;
	}
	
		int scan_status=0;
		scan_status=fscanf(file,"%d",&violations_threshold); // Threshold for violations
		if(scan_status!=1){
				perror("error getting threshold key value.");
				fclose(file);
				exit(1);
		}
	
	Threshold=violations_threshold;
	fclose(file);
	return key;
}

// Function to read and store filtered words from filtered_words.txt (also stores filteredWordscount for later use)
void getFilteredWords(FILE * file){
	
	char word[MAX_FILTERED_WORD_LENGTH];
	int filtered_words_count=0;
	
	char tempfilteredWords[50][20];
	int count=0;
	
	//counts total number of filtered words in file
	 while( fscanf(file, "%s", word) != EOF){
	 
         	filtered_words_count++; 
         }
         
         rewind(file);// takes pointer back to top of file
         
         filteredWordCount=filtered_words_count; // giving count value to global variable
         
         count=0;
         
         filteredWords = (char **)malloc(filteredWordCount * sizeof(char *));
         //Reads filtered words from file and converts to lowercase and stores them in filteredWords array of strings
         while(count<filtered_words_count){
         	filteredWords[count] = (char *)malloc(MAX_FILTERED_WORD_LENGTH * sizeof(char));
         	fscanf(file, "%s", tempfilteredWords[count]);
         	filteredWords[count] =tempfilteredWords[count];
         	toLowerCase(filteredWords[count]);
         	count++;
         }
         
         fclose(file);
}

// Function to count violations commited by user in a message and update total violations of the user in TrackViolations[][]
int countViolations(char *msg, char **filteredWords, int filteredWordCount, int group_id, int user_id) {
    //if(TrackViolations[group_id][user_id] >= Threshold
    int count = 0;
    
    // repeats process for each filtered words
    for (int i = 0; i < filteredWordCount; i++) {
        const char *p = msg;
        
        // Count all occurrences of filteredWords[i] in msg
        while ((p = strstr(p, filteredWords[i])) != NULL) {
            count++;  // Found one occurrence
            p ++;
        }
    }
    
    TrackViolations[group_id][user_id] += count;
    
    // Returns 1 if user violations >=Threshold, else 0
    return (TrackViolations[group_id][user_id] >= Threshold) ? 1 : 0;
}


int main(int argc, char *argv[]) {
    
    if (argc < 2) { 
        printf("Usage: %s <test_case_number>\n", argv[0]);
        return 1;
    }
    
    //to read from the filteredwords.txt file of X test case
    FILE *file;
    char filepath[256];
    
    snprintf(filepath, sizeof(filepath), "testcase_%s/filtered_words.txt", argv[1]);
    file = fopen(filepath, "r");
    if (file == NULL) {
        perror("Error opening file");
        return EXIT_FAILURE;
    }
    
    //calls function by giving the opened filteredwords.txt file pointer
    getFilteredWords(file);
    
    //to read from the input.txt file of X test case
    FILE *file1;
    char filepath1[256];
    
    snprintf(filepath1, sizeof(filepath1), "testcase_%s/input.txt", argv[1]);
    file1 = fopen(filepath1, "r");
    if (file1 == NULL) {
        perror("Error opening file");
        return EXIT_FAILURE;
    }
    
    //calling function using file pointer of input.txt
    int ipc_key=getInputParameters(file1);
    
    //generate or access message queue
	int msgid = msgget(ipc_key, 0666 | IPC_CREAT);
    	if (msgid == -1) {
		perror("msgget failed");
		exit(1);
    	}
    	
    	//To track no.of active groups left
	int activeGroups=NumberOfGroups;
	
	//loop runs until all groups are terminated
	while(1){
		printf("\n");
	
		// Receiving message from groups.c 
		int receive_status= msgrcv(msgid, &message, sizeof(message) - sizeof(long), 1, 0);
		//printf("receive success: msg text: %s\n",message.mtext);
		if (receive_status == -1) {
			perror("msgrcv failed");
			exit(1);
	    	}
	    	
	    	//updates number of active groups left (i.e if a group is terminated then it sends a message with Group_status set as 1
	    	activeGroups=activeGroups-message.Group_status;
	    	
	    	// exits loop if active groups are 0
		if(activeGroups==0){
			break;
		}
		
		
		if(message.Group_status==1){
		//printf("group terminated: %d\n",message.group_id);
				continue;
		}
		
	    	toLowerCase(message.mtext); // converts user msg text to lowercase
	    	int violationFlag = countViolations(message.mtext, filteredWords, filteredWordCount, message.group_id, message.user_id);
	    	//printf("violation flag status: %d\n",violationFlag);
	    	message.mtype = message.group_id + MAX_GRP_SIZE; // different mtypes for different groups
		message.Delete_user= violationFlag;// 1 if user should be deleted
		
		msg message1=message;
		//send message to groups.c
		int send_status = msgsnd(msgid, &message1, sizeof(message1) - sizeof(long), 0);
		//printf("sent success: \n");
		if (send_status == -1) {
		        perror("msgsnd failed");
		        exit(1);
		}
		
		// Print status if user is removed
		if (violationFlag) {
		    printf("User %d from group %d has been removed due to %d violations.\n",
		           message.user_id, message.group_id, 
		           TrackViolations[message.group_id][message.user_id]);
		           fflush(stdout); 
		}
        }
    
    return 0;
}
