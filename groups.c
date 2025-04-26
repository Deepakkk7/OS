#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h> 
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/msg.h>


#define MAX_GROUPS 30
#define MAX_USERS 50
#define MAX_TEXT_SIZE 256
#define MAX_MESSAGE_TIMESTAMP 2147000000

#define READ_END 0
#define WRITE_END 1

#define NO_USER 0
#define ACTIVE 1
#define OVER 2
#define KILLED 3


typedef struct{
    long mtype;
    int timestamp;
    int user;
    char mtext[MAX_TEXT_SIZE];
    int modifyingGroup;
} Message;

typedef struct{
    int timestamp;
    char text[MAX_TEXT_SIZE];
    int userNum;
} MsgToGroup;

typedef struct{
	long mtype; //using mtype = 1 here
	int group_Number;
} Msg_GrpToApp;

typedef struct{
	long mtype;
	int group_id;
	int user_id;
	int timestamp;
	char mtext[256];
	int Delete_user;
	int Group_status;
}Msg_GrpToMod;


// Min-heap structure
typedef struct {
    MsgToGroup *data;
    int size;
    int capacity;
} MinHeap;

// Min-heap functions
MinHeap *createHeap(int capacity) {
    MinHeap *heap = (MinHeap *)malloc(sizeof(MinHeap));
    heap->data = (MsgToGroup *)malloc(capacity * sizeof(MsgToGroup));
    heap->size = 0;
    heap->capacity = capacity;
    return heap;
}

void insertHeap(MinHeap *heap, MsgToGroup msg) {
    int i = heap->size++;
    while (i > 0 && heap->data[(i - 1) / 2].timestamp > msg.timestamp) {
        heap->data[i] = heap->data[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    heap->data[i] = msg;
}

MsgToGroup removeMin(MinHeap *heap) {
    MsgToGroup min = heap->data[0];
    MsgToGroup last = heap->data[--heap->size];

    int i = 0, child;
    while ((child = 2 * i + 1) < heap->size) {
        if (child + 1 < heap->size && heap->data[child + 1].timestamp < heap->data[child].timestamp)
            child++;
        if (last.timestamp <= heap->data[child].timestamp)
            break;
        heap->data[i] = heap->data[child];
        i = child;
    }
    heap->data[i] = last;
    return min;
}


int extract_Y(const char *filepath) {
    int X, Y;  
    if (sscanf(filepath, "users/user_%d_%d.txt", &X, &Y) == 2) {
        return Y;
    }
    return -1;  // Return -1 if format is incorrect
}

void clean_and_pad_text(char *str) {
    int len = strlen(str);
    // Trim trailing whitespace
    while (len > 0 && isspace((unsigned char)str[len - 1])) {
        str[--len] = '\0';
    }
    // Ensure string is exactly MAX_TEXT_SIZE by padding with '\0'
    while (len < MAX_TEXT_SIZE - 1) {
        str[len++] = '\0';
    }
    str[MAX_TEXT_SIZE - 1] = '\0';  // Ensure null termination
}

void readFromPipeToHeap(int X, int u, int pipefds[][2], MinHeap *heap, int *userStatuses, int *activeUsers) { //Reads msg from pipe and inserts into heap. If pipe is closed, updates statuses
    MsgToGroup msg;
    if (read(pipefds[u][READ_END], &msg, sizeof(msg)) > 0) {
        if(msg.timestamp != -1){
            insertHeap(heap, msg);
            //printf("***Group %d read %s from user %d and added to heap\n", X, msg.text, u);    
        }else{
            close(pipefds[u][READ_END]);
            userStatuses[u] = OVER;
            (*activeUsers)--;
            //printf("***Group %d User %d pipe finished (termination signal received)\n", X, u);
        }

    } else {  // User process is done
        close(pipefds[u][READ_END]);
        userStatuses[u] = OVER;
        (*activeUsers)--;
        //printf("***Group %d User %d nothing more in pipe\n", X, u);
    }
}


int main(int argc, char *argv[]){

    //Receive X(group number) and Testcase number from app.c
    if(argc != 3){
        printf("Incorrect no. of args"); exit(1);
    }
    int testcaseNum = atoi(argv[1]);
    int X = atoi(argv[2]);

    //Get message queue IDs from input.txt
    int key_validation, key_app, key_moderator;
    char input_filepath[100];
    snprintf(input_filepath, sizeof(input_filepath), "testcase_%d/input.txt", testcaseNum);
    FILE* input_file = fopen(input_filepath, "r");
    if(!input_file){
        printf("Error opening input.txt");  exit(1);
    }
    int skip;
    fscanf(input_file, "%d", &skip);  // Read and ignore the first line
    fscanf(input_file, "%d", &key_validation);  
    fscanf(input_file, "%d", &key_app);  
    fscanf(input_file, "%d", &key_moderator);  

        //printf("***groups read keys %d %d %d\n", key_validation, key_app, key_moderator);  // ***

    int msgID_app, msgID_moderator, msgID_validation;
    msgID_validation = msgget(key_validation, 0666|IPC_CREAT);
    if(msgID_validation == -1){
        printf("msgget failed\n");  exit(1);
    }
    msgID_moderator = msgget(key_moderator, 0666|IPC_CREAT);
    if(msgID_moderator == -1){
        printf("msgget failed\n");  exit(1);
    }
    msgID_app= msgget(key_app, 0666|IPC_CREAT);
    if(msgID_app == -1){
        printf("msgget failed\n");  exit(1);
    }

        //printf("***groups message IDs created\n");  // ***

    //Send Message to validation.out
    Message grpCreated;
    grpCreated.mtype = 1;
    grpCreated.modifyingGroup = X;
    if(msgsnd(msgID_validation, &grpCreated, sizeof(Message)-sizeof(long), 0) == -1){
        printf("Error in sending grpCreated to validation - Group %d\n", X);  exit(1);
    }

        //printf("***groups creation message sent to validation\n");  // ***
    
    //Read no. of users from group_X.txt
    char group_filepath[100];
    snprintf(group_filepath, sizeof(group_filepath), "testcase_%d/groups/group_%d.txt", testcaseNum, X);
    FILE* group_file = fopen(group_filepath, "r");
    if(group_file == NULL){
        printf("Error opening group_%d %s\n", X, group_filepath);   exit(1);
    }
    int M;  //no. of users
    fscanf(group_file, "%d\n", &M);

    if(M > MAX_USERS){
        printf("Maximum Group capacity is %d\n", MAX_USERS);    exit(1);
    }

        //printf("***group %d no. of users %d\n", X, M);  // ***

    //Create array to keep track of user statuses
    int userStatuses[MAX_USERS];
    for(int i=0; i<MAX_USERS; i++)
        userStatuses[i] = NO_USER;

    //declare pipe fds
    int pipefds[MAX_USERS][2];  //use the Y'th pipefd for the Y'th user

    //declare child pids
    pid_t childPIDs[MAX_USERS];

    //Fork each user process
    char user_filepath[100];
    pid_t pid;
    for(int i=0; i<M; i++){
        fscanf(group_file, "%s", user_filepath);
        char user_filepath_extended[300];
        snprintf(user_filepath_extended, sizeof(user_filepath_extended), "testcase_%d/%s", testcaseNum, user_filepath);
        int Y = extract_Y(user_filepath);   //get user no.

            //printf("*** extracted Y=%d\n", Y);      //***

        //Create a pipe
        if(pipe(pipefds[Y]) == -1){
            printf("Pipe creation failed\n");   exit(1);
        }
        
        pid = fork();
        if(pid < 0){
            printf("Fork failed - user %d\n", i);   exit(1);
        } 
        else if(pid == 0){
        // Child Process(User)
                //printf("***user %d_%d created\n", X, Y);  // ***

            //Open user_X_Y.txt
            
            FILE* user_file = fopen(user_filepath_extended, "r");

            if(user_file == NULL){
                printf("Error opening %s\n", user_filepath_extended);  exit(1);
            }

                //printf("***user filepath extended %s", user_filepath_extended);

            //Set up pipe
            close(pipefds[Y][READ_END]);

            //Inform validation.out
            Message userCreated;
            userCreated.mtype = 2;
            userCreated.user = Y;
            userCreated.modifyingGroup = X;
            if(msgsnd(msgID_validation, (void*)&userCreated, sizeof(Message)-sizeof(long), 0) == -1){
                printf("Error in sending userCreated to validation - Group %d\n", X);   exit(1);
            }

                //printf("***Group %d user %d created - informed validation\n", X, Y);  // ***

            //Read timestamps and texts, send to Parent
            int timestamp;
            char line[MAX_TEXT_SIZE], text[MAX_TEXT_SIZE];
            MsgToGroup msgtoGrp;

            while(fgets(line, sizeof(line), user_file)){    //while lines are there in user_X_Y.txt
                if(sscanf(line, "%d %s", &timestamp, text) == 2){    //extract timestamp and text, put in pipe
                    clean_and_pad_text(text);

                    msgtoGrp.timestamp = timestamp;
                    strcpy(msgtoGrp.text, text);
                    msgtoGrp.userNum = Y;

                    write(pipefds[Y][WRITE_END], &msgtoGrp, sizeof(MsgToGroup));

                    //printf("***User %d Group %d written to pipe\n", Y, X);        //***
                }
            }
            //after lines are over
            msgtoGrp.timestamp = -1;
            write(pipefds[Y][WRITE_END], &msgtoGrp, sizeof(MsgToGroup));
            close(pipefds[Y][WRITE_END]);

            //printf("***User %d (Group %d) finished reading file\n", Y, X);          /// ***
            exit(0);
        }

        //Parent Process - for each user
        userStatuses[Y] = ACTIVE;
        close(pipefds[Y][WRITE_END]);
        childPIDs[Y] = pid;
    }
    
        //printf("***Group %d finished parent initializations\n", X);         /// ***

        // Parent Process(Group):
    MsgToGroup msg;
    int activeUsers = M;
    int violatingUsers = 0;
    MinHeap *heap = createHeap(MAX_USERS);

    //first, take in one message each from all users
    for(int u=0; u<MAX_USERS; u++){     //take in message from each user
        if(userStatuses[u] == ACTIVE){
            readFromPipeToHeap(X, u, pipefds, heap, userStatuses, &activeUsers);
        }
    }

    //send out oldest msg, look for next msg from the same user. If not available, go to next user - repeat while 2 or more users are active
    
    while(activeUsers >= 2){
        if(heap->size > 0){
            //get lowest timestamp msg
            MsgToGroup msg_out = removeMin(heap);

            if(userStatuses[msg_out.userNum] == ACTIVE){
                //printf("\t***Heap sent out %s from user %d (Group %d) at %d\n", msg_out.text, msg_out.userNum, X, msg_out.timestamp);
                //send msg to moderator.c
                Msg_GrpToMod msgToMod;
                msgToMod.mtype = 1;
                msgToMod.group_id = X;
                msgToMod.user_id = msg_out.userNum;
                msgToMod.timestamp = msg_out.timestamp;
                strcpy(msgToMod.mtext, msg_out.text);
                msgToMod.Group_status = 0;  //Group_status = 0 for messages
                if(msgsnd(msgID_moderator, &msgToMod, sizeof(Msg_GrpToMod)-sizeof(long), 0) == -1){
                    printf("Error in sending message to moderator - Group %d\n", X);  exit(1);
                }

                //printf("***Group %d User %d: sent msg to moderator - %s\n", X, msg_out.userNum, msg_out.text);                  /// ***

                //receive ok/not ok from moderator.c
                Msg_GrpToMod msg_fromMod;
                if(msgrcv(msgID_moderator, &msg_fromMod, sizeof(msg_fromMod)-sizeof(long), (MAX_GROUPS + X), 0) == -1){
                    printf("Error in receiving from moderator");    exit(1);
                }
                int userOK = !msg_fromMod.Delete_user;
                //printf("***Group %d User %d: received %s from moderator\n", X, msg_out.userNum, userOK?"ok":"NOT ok");      /// ***           

                if(userOK){ //if ok, send text to validation.out, else, kill user process and update statuses
                    Message msgToVal;
                    msgToVal.mtype = (MAX_GROUPS + X);
                    msgToVal.timestamp = msg_out.timestamp;
                    msgToVal.user = msg_out.userNum;
                    strcpy(msgToVal.mtext, msg_out.text);
                    if(msgsnd(msgID_validation, (void*)&msgToVal, sizeof(Message)-sizeof(long), 0) == -1){
                        printf("Error in sending message to validation - Group %d\n", X);  exit(1);
                    }
                   // printf("***Group %d User %d: sent msg to validation - %s\n", X, msg_out.userNum, msgToVal.mtext);             /// ***

                    //get next msg from same user
                    readFromPipeToHeap(X, msg_out.userNum, pipefds, heap, userStatuses, &activeUsers);
                    //printf("***Group %d User %d: next msg from user; (User Status %d)\n", X, msg_out.userNum, userStatuses[msg_out.userNum]);                               /// ***
                } else{
                    Message msgToVal;
                    msgToVal.mtype = (MAX_GROUPS + X);
                    msgToVal.timestamp = msg_out.timestamp;
                    msgToVal.user = msg_out.userNum;
                    strcpy(msgToVal.mtext, msg_out.text);
                    if(msgsnd(msgID_validation, (void*)&msgToVal, sizeof(Message)-sizeof(long), 0) == -1){
                        printf("Error in sending message to validation - Group %d\n", X);  exit(1);
                    }
                    //printf("***Group %d User %d: sent msg to validation - %s\n", X, msg_out.userNum, msgToVal.mtext); 
                    
                    kill(childPIDs[msg_out.userNum], SIGTERM);
                    userStatuses[msg_out.userNum] = KILLED;
                    violatingUsers++;
                    activeUsers--;
                    //readFromPipeToHeap(X, msg_out.userNum, pipefds, heap, userStatuses, &activeUsers);
                    //printf("***Group %d user %d Killed\n", X, msg_out.userNum);                                  /// ***
                } 
            }

        }

    }

    //Group termination
    for (int i = 0; i < MAX_USERS; i++) {
		close(pipefds[i][READ_END]);
		close(pipefds[i][WRITE_END]);
	}

    //printf("***Group %d to be terminated\n", X);                                                            /// ***
    
    //send info to moderator.c
    Msg_GrpToMod grpTerminated_toMod;
    grpTerminated_toMod.mtype = 1;
    grpTerminated_toMod.Group_status = 1;
    if(msgsnd(msgID_moderator, (void*)&grpTerminated_toMod, sizeof(Msg_GrpToMod)-sizeof(long), 0) == -1){
        printf("Error in sending grpTerminated_toMod - Group %d\n", X);  exit(1);
    }
    //printf("***Group %d termination sent to moderator\n", X);                                               /// ***

    //send info to validation.out
    Message grpTerminated;
    grpTerminated.mtype = 3;
    grpTerminated.user = violatingUsers;
    grpTerminated.modifyingGroup = X;
    if(msgsnd(msgID_validation, (void*)&grpTerminated, sizeof(Message)-sizeof(long), 0) == -1){
        printf("Error in sending grpTerminated to validation - Group %d\n", X);  exit(1);
    }
    //printf("***Group %d termination sent to validation\n", X);                                              /// ***

    //send info to app.c
    Msg_GrpToApp grpTerminated_toApp;
    grpTerminated_toApp.mtype = 1;
    grpTerminated_toApp.group_Number = X;
    if(msgsnd(msgID_app, (void*)&grpTerminated_toApp, sizeof(Msg_GrpToApp)-sizeof(long), 0) == -1){
        printf("Error in sending grpTerminated to App - Group %d\n", X);  exit(1);
    }
    //printf("***Group %d termination sent to app\n", X);                                                     /// ***

    exit(0);
}

