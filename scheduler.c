#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <stdbool.h>
#include <pthread.h>

#define MAX_CARGO_COUNT 200
#define MAX_NEW_REQUESTS 100
#define MAX_DOCKS 30
#define MAX_SHIP_REQUESTS 1100

typedef struct {
    int dockId;
    int solverIdx;
    int threadId;
    int numThreads;
    int stringLength;
    bool success;
    char authString[100];
} ThreadData;

typedef struct ShipRequest {
    int shipId;
    int timestep;
    int category;
    int direction;
    int emergency;
    int waitingTime;
    int numCargo;
    int cargo[MAX_CARGO_COUNT];
} ShipRequest;

typedef struct MainSharedMemory {
    char authStrings[MAX_DOCKS][100];
    ShipRequest newShipRequests[MAX_NEW_REQUESTS];
} MainSharedMemory;

typedef struct MessageStruct {
    long mtype;
    int timestep;
    int shipId;
    int direction;
    int dockId;
    int cargoId;
    int isFinished;
    union {
        int numShipRequests;
        int craneId;
    };
} MessageStruct;

typedef struct SolverRequest {
    long mtype;
    int dockId;
    char authStringGuess[100];
} SolverRequest;

typedef struct SolverResponse {
    long mtype;
    int guessIsCorrect;
} SolverResponse;

typedef struct Dock {
    int id;
    int category;
    int *craneCapacities;
    bool isOccupied;
    int occupiedByShipId;
    int occupiedByDirection;
    int dockingTimestep;
    int lastCargoMovedTimestep;
    bool allCargoMoved;
    int maxCraneCapacity; // Added to track max crane capacity
} Dock;

typedef struct Ship {
    int id;
    int direction;
    int category;
    int emergency;
    int waitingTime;
    int arrivalTimestep;
    int numCargo;
    int *cargo;
    bool cargoMoved[MAX_CARGO_COUNT];
    bool isServiced;
    bool isAssignedDock;
    int assignedDockId;
    int cargosMovedCount;
    int maxCargoWeight; // Added to track max cargo weight
    int priority;       // Added to help prioritize ships
} Ship;

// Global variables
int mainQueueId;
int shmId;
MainSharedMemory *sharedMemory;
int solverQueueIds[8];
int numSolvers;
int numDocks;
Dock *docks;
Ship *ships[MAX_SHIP_REQUESTS];
int shipCount = 0;
int currentTimestep = 1;
MessageStruct globalMessage;

volatile bool authStringFound = false;
pthread_mutex_t authMutex = PTHREAD_MUTEX_INITIALIZER;

void initializeIPC(char *filename) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("Error opening input file");
        exit(1);
    }

    
    key_t shmKey;
    fscanf(file, "%d", &shmKey);

    
    key_t mqKey;
    fscanf(file, "%d", &mqKey);

    fscanf(file, "%d", &numSolvers);
    for (int i = 0; i < numSolvers; i++) {
        fscanf(file, "%d", &solverQueueIds[i]);
    }

    fscanf(file, "%d", &numDocks);
    docks = (Dock *)malloc(numDocks * sizeof(Dock));
    if (docks == NULL) {
        perror("Memory allocation failed for docks");
        exit(1);
    }

    for (int i = 0; i < numDocks; i++) {
        docks[i].id = i;
        fscanf(file, "%d", &docks[i].category);
        
        docks[i].craneCapacities = (int *)malloc(docks[i].category * sizeof(int));
        if (docks[i].craneCapacities == NULL) {
            perror("Memory allocation failed for crane capacities");
            exit(1);
        }
        
        // Initialize max crane capacity to lowest possible value
        docks[i].maxCraneCapacity = 0;
        
        for (int j = 0; j < docks[i].category; j++) {
            fscanf(file, "%d", &docks[i].craneCapacities[j]);
            
            // Update max crane capacities
            if (docks[i].craneCapacities[j] > docks[i].maxCraneCapacity) {
                docks[i].maxCraneCapacity = docks[i].craneCapacities[j];
            }
        }
        
        docks[i].isOccupied = false;
        docks[i].allCargoMoved = false;
    }
    fclose(file);

    
    shmId = shmget(shmKey, sizeof(MainSharedMemory), 0666);
    if (shmId == -1) {
        perror("Error connecting to shared memory");
        exit(1);
    }

    sharedMemory = (MainSharedMemory *)shmat(shmId, NULL, 0);
    if (sharedMemory == (void *)-1) {
        perror("Error attaching to shared memory");
        exit(1);
    }

    mainQueueId = msgget(mqKey, 0666);
    if (mainQueueId == -1) {
        perror("Error connecting to main message queue");
        exit(1);
    }

    for (int i = 0; i < numSolvers; i++) {
        solverQueueIds[i] = msgget(solverQueueIds[i], 0666);
        if (solverQueueIds[i] == -1) {
            perror("Error connecting to solver message queue");
            exit(1);
        }
    }
}

void calculateShipProperties() {
    // Calculate max cargo weight for each ship
    for (int i = 0; i < shipCount; i++) {
        if (!ships[i]->isServiced && ships[i]->maxCargoWeight == 0) {
            ships[i]->maxCargoWeight = 0;
            for (int j = 0; j < ships[i]->numCargo; j++) {
                if (ships[i]->cargo[j] > ships[i]->maxCargoWeight) {
                    ships[i]->maxCargoWeight = ships[i]->cargo[j];
                }
            }
        }
    }
}

void prioritizeShips() {
    for (int i = 0; i < shipCount; i++) {
        Ship *ship = ships[i];
        
        if (ship->isServiced || ship->isAssignedDock) {
            continue;
        }
        
        ship->priority = 0;
        
        // Emergency ships still get highest priority
        if (ship->direction == 1 && ship->emergency == 1) {
            ship->priority += 1000000;
            continue;
        }
        
        // Prioritize ships based on waiting time and cargo efficiency
        if (ship->direction == 1 && ship->waitingTime > 0) {
            int timeRemaining = (ship->arrivalTimestep + ship->waitingTime) - currentTimestep;
            
            if (timeRemaining <= 0) {
                ship->priority += 500000;
            } else if (timeRemaining <= 3) {
                ship->priority += 250000;
            } else if (timeRemaining <= 10) {
                ship->priority += 100000;
            } else {
                ship->priority += 50000;
            }
            
            if(ship->numCargo>20){
            // Add priority based on cargo efficiency (cargo count / waiting time)
            float cargoEfficiency = (float)ship->numCargo / ship->waitingTime;
            ship->priority += (int)(cargoEfficiency * 10000);
            }
        }
        
        // Outgoing ships - prioritize based on how long they've been waiting
        if (ship->direction == -1) {
            int waitingTime = currentTimestep - ship->arrivalTimestep;
            ship->priority += 10000 + (waitingTime * 100);
        }
        
        // Prioritize ships with higher cargo density (more cargo items relative to category)
        float cargoDensity = (float)ship->numCargo / ship->category;
        ship->priority += (int)(cargoDensity * 5000);
        
        // Prioritize ships with lower max cargo weight (easier to process)
        ship->priority += (50 - ship->maxCargoWeight) * 100;
        
        // Earlier arrival time gets higher priority
        ship->priority += (1000 - (currentTimestep - ship->arrivalTimestep)) * 10;
    }
}

void processNewShipRequests(int numNewRequests) {
    for (int i = 0; i < numNewRequests; i++) {
        ShipRequest newRequest = sharedMemory->newShipRequests[i];
        
        // Check if ship already exists (might have returned after waiting time)
        bool shipExists = false;
        for (int j = 0; j < shipCount; j++) {
            if (ships[j]->id == newRequest.shipId && ships[j]->direction == newRequest.direction) {
                // Ship already exists, update its arrival timestep
                ships[j]->arrivalTimestep = newRequest.timestep;
                ships[j]->isAssignedDock = false;
                shipExists = true;
                break;
            }
        }
        
        if (!shipExists) {
            // Create new ship
            Ship *newShip = (Ship *)malloc(sizeof(Ship));
            if (newShip == NULL) {
                perror("Memory allocation failed for new ship");
                exit(1);
            }
            
            newShip->id = newRequest.shipId;
            newShip->direction = newRequest.direction;
            newShip->category = newRequest.category;
            newShip->emergency = newRequest.emergency;
            newShip->waitingTime = newRequest.waitingTime;
            newShip->arrivalTimestep = newRequest.timestep;
            newShip->numCargo = newRequest.numCargo;
            newShip->isServiced = false;
            newShip->isAssignedDock = false;
            newShip->cargosMovedCount = 0;
            newShip->maxCargoWeight = 0;  // Will be calculated later
            newShip->priority = 0;        // Will be calculated later
        
            newShip->cargo = (int *)malloc(newShip->numCargo * sizeof(int));
            if (newShip->cargo == NULL) {
                perror("Memory allocation failed for ship cargo");
                free(newShip);
                exit(1);
            }
            
            for (int j = 0; j < newShip->numCargo; j++) {
                newShip->cargo[j] = newRequest.cargo[j];
                newShip->cargoMoved[j] = false;
                
                // Update max cargo weight while we're at it
                if (newRequest.cargo[j] > newShip->maxCargoWeight) {
                    newShip->maxCargoWeight = newRequest.cargo[j];
                }
            }
            
            ships[shipCount++] = newShip;
        }
    }
}


bool checkIfAllShipsServiced() {
    for (int i = 0; i < shipCount; i++) {
        if (!ships[i]->isServiced) {
            return false;
        }
    }
    return true;
}

void* authStringGuesser(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    int dockId = data->dockId;
    int solverIdx = data->solverIdx;
    int threadId = data->threadId;
    int numThreads = data->numThreads;
    int stringLength = data->stringLength;
    
    
    char possibleFirstLastChars[] = "56789"; 
    char possibleMiddleChars[] = "56789.";  
    
    int firstLastCombinations = 5; 

    long long totalCombinations = firstLastCombinations * firstLastCombinations;
    for (int i = 0; i < stringLength - 2; i++) {
        totalCombinations *= 6;
    }
    
    long long combinationsPerThread = totalCombinations / numThreads;
    long long startCombo = threadId * combinationsPerThread;
    long long endCombo = (threadId == numThreads - 1) ? 
                         totalCombinations : (threadId + 1) * combinationsPerThread;
    
    //printf("Thread %d (solver %d) will try combinations %lld to %lld (of %lld total)\n", 
           //threadId, solverIdx, startCombo, endCombo, totalCombinations);
    
    // Set dock ID for this solver
    SolverRequest setDockRequest;
    setDockRequest.mtype = 1;
    setDockRequest.dockId = dockId;
    
    if (msgsnd(solverQueueIds[solverIdx], &setDockRequest, sizeof(SolverRequest) - sizeof(long), 0) == -1) {
        perror("Error sending solver dock message");
        data->success = false;
        return NULL;
    }
    
    
    char authString[100];
    memset(authString, 0, 100);
    
    // Set first and last characters (not '.')
    authString[0] = possibleFirstLastChars[0];
    if (stringLength > 1) {
        authString[stringLength - 1] = possibleFirstLastChars[0];
    }
    
    // Set middle characters
    for (int i = 1; i < stringLength - 1; i++) {
        authString[i] = possibleMiddleChars[0];
    }
    
    //skip to starting combination
    long long currentCombo = 0;
    while (currentCombo < startCombo) {
        // Generate next combination
        int pos = stringLength - 1;
        while (pos >= 0) {
            if (pos == 0 || pos == stringLength - 1) {
                // first or last character
                int idx = -1;
                for (int i = 0; i < 5; i++) {
                    if (authString[pos] == possibleFirstLastChars[i]) {
                        idx = i;
                        break;
                    }
                }
                
                if (idx == 4) { //last possible character
                    authString[pos] = possibleFirstLastChars[0];
                    pos--;
                } else {
                    authString[pos] = possibleFirstLastChars[idx + 1];
                    break;
                }
            } else {
                // middle characters
                int idx = -1;
                for (int i = 0; i < 6; i++) {
                    if (authString[pos] == possibleMiddleChars[i]) {
                        idx = i;
                        break;
                    }
                }
                
                if (idx == 5) { // Last possible character
                    authString[pos] = possibleMiddleChars[0];
                    pos--;
                } else {
                    authString[pos] = possibleMiddleChars[idx + 1];
                    break;
                }
            }
        }
        
        currentCombo++;
    }
    
    // Now try combinations from startCombo to endCombo
    while (currentCombo < endCombo) {
        // Check if another thread found the solution
        if (authStringFound) {
            data->success = false;
            return NULL;
        }
        
        // Send the guess
        SolverRequest guessRequest;
        guessRequest.mtype = 2;
        strncpy(guessRequest.authStringGuess, authString, 100);
        
        if (msgsnd(solverQueueIds[solverIdx], &guessRequest, sizeof(SolverRequest) - sizeof(long), 0) == -1) {
            perror("Error sending solver guess message");
            continue;
        }
        
        // Wait for the response
        SolverResponse response;
        if (msgrcv(solverQueueIds[solverIdx], &response, sizeof(SolverResponse) - sizeof(long), 3, 0) == -1) {
            perror("Error receiving solver response");
            continue;
        }
        
        // Check if the guess is correct
        if (response.guessIsCorrect == 1) {
            // Correct guess, set the result and notify other threads
            pthread_mutex_lock(&authMutex);
            authStringFound = true;
            strncpy(data->authString, authString, 100);
            data->success = true;
            pthread_mutex_unlock(&authMutex);
            return NULL;
        }
        
        // Generate next combination
        int pos = stringLength - 1;
        while (pos >= 0) {
            if (pos == 0 || pos == stringLength - 1) {
                // First or last character
                int idx = -1;
                for (int i = 0; i < 5; i++) {
                    if (authString[pos] == possibleFirstLastChars[i]) {
                        idx = i;
                        break;
                    }
                }
                
                if (idx == 4) { // Last possible character
                    authString[pos] = possibleFirstLastChars[0];
                    pos--;
                } else {
                    authString[pos] = possibleFirstLastChars[idx + 1];
                    break;
                }
            } else {
                // Middle character
                int idx = -1;
                for (int i = 0; i < 6; i++) {
                    if (authString[pos] == possibleMiddleChars[i]) {
                        idx = i;
                        break;
                    }
                }
                
                if (idx == 5) { // Last possible character
                    authString[pos] = possibleMiddleChars[0];
                    pos--;
                } else {
                    authString[pos] = possibleMiddleChars[idx + 1];
                    break;
                }
            }
        }
        
        currentCombo++;
    }
    
    data->success = false;
    return NULL;
}

void loadAuthString(int dockId, char *authString) {
    // Copy the auth string to the shared memory for the specified dock
    strncpy(sharedMemory->authStrings[dockId], authString, 100);
    
    // Ensure null termination
    sharedMemory->authStrings[dockId][100 - 1] = '\0';
}

bool guessAuthString(int dockId) {
    // Determine string length (last cargo moved timestep - docking timestep)
    Dock *dock = &docks[dockId];
    int stringLength = dock->lastCargoMovedTimestep - dock->dockingTimestep;
    
    // Ensure a minimum length of 1
    if (stringLength <= 0) stringLength = 1;
    
    // Initialize mutex for thread synchronization
    pthread_mutex_init(&authMutex, NULL);
    authStringFound = false;
    
    // Create threads, one for each solver
    pthread_t threads[8];
    ThreadData threadData[8];
    
    //printf("Creating %d threads to guess auth string of length %d for dock %d\n", 
           //numSolvers, stringLength, dockId);
    
    // Launch threads
    for (int i = 0; i < numSolvers; i++) {
        threadData[i].dockId = dockId;
        threadData[i].solverIdx = i;
        threadData[i].threadId = i;
        threadData[i].numThreads = numSolvers;
        threadData[i].stringLength = stringLength;
        threadData[i].success = false;
        
        if (pthread_create(&threads[i], NULL, authStringGuesser, &threadData[i]) != 0) {
            perror("Failed to create thread");
            // Continue with fewer threads if creation fails
        }
    }
    
    // Wait for all threads to complete
    bool success = false;
    char foundAuthString[100];
    
    for (int i = 0; i < numSolvers; i++) {
        pthread_join(threads[i], NULL);
        
        // Check if this thread found the solution
        if (threadData[i].success) {
            success = true;
            strncpy(foundAuthString, threadData[i].authString, 100);
        }
    }
    
    // Clean up mutex
    pthread_mutex_destroy(&authMutex);
    
    // If a solution was found, load it into shared memory
    if (success) {
        //printf("Auth string found: %s\n", foundAuthString);
        loadAuthString(dockId, foundAuthString);
        return true;
    }
    
    //printf("Failed to find auth string for dock %d\n", dockId);
    return false;
}

bool undockShip(Dock *dock) {
    // Guess auth string
    if (!guessAuthString(dock->id)) {
        return false;
    }
    
    // Send undock message
    MessageStruct message;
    message.mtype = 3;
    message.shipId = dock->occupiedByShipId;
    message.direction = dock->occupiedByDirection;
    message.dockId = dock->id;
    
    if (msgsnd(mainQueueId, &message, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
        perror("Error sending undock message");
        exit(1);
    }
    
    // Reset dock status
    dock->isOccupied = false;
    dock->allCargoMoved = false;
    
    return true;
}

bool attemptUndocking() {
    printf("Trying to undock ships at timestep %d\n", currentTimestep);
    
    // Track if any undocking was attempted but failed
    bool undockingFailed = false;
    
    // Try to undock ships that have finished cargo operations
    for (int i = 0; i < numDocks; i++) {
        if (!docks[i].isOccupied) {
            continue;
        }
        
        // Print dock status
        //printf("Dock %d status: occupied by ship %d, allCargoMoved=%d, lastCargoMoved=%d\n", 
               //i, docks[i].occupiedByShipId, docks[i].allCargoMoved, docks[i].lastCargoMovedTimestep);
        
        // Try to undock if all cargo moved and it's not the same timestep as the last cargo movement
        if (docks[i].allCargoMoved && docks[i].lastCargoMovedTimestep < currentTimestep) {
            // Find the ship at this dock
            Ship *ship = NULL;
            for (int j = 0; j < shipCount; j++) {
                if (ships[j]->id == docks[i].occupiedByShipId && 
                    ships[j]->direction == docks[i].occupiedByDirection && 
                    ships[j]->isAssignedDock && 
                    ships[j]->assignedDockId == i) {
                    ship = ships[j];
                    break;
                }
            }
            
            if (ship == NULL) {
                printf("Warning: No ship found at dock %d for undocking\n", i);
                continue;
            }
            
            //printf("Attempting to undock ship %d from dock %d\n", ship->id, i);
            
            // Double-check that all cargo has been moved
            bool allMoved = true;
            for (int j = 0; j < ship->numCargo; j++) {
                if (!ship->cargoMoved[j]) {
                    allMoved = false;
                    printf("Warning - ship %d still has cargo which was not moved - index %d\n", ship->id, j);
                    break;
                }
            }
            
            if (!allMoved) {
                // Fix inconsistency - update the dock status
                docks[i].allCargoMoved = false;
                continue;
            }
            
            // Try to undock the ship with multiple attempts
            bool undockSuccess = false;
            for (int attempt = 0; attempt < 5; attempt++) {  // Try up to 5 times
                if (undockShip(&docks[i])) {
                    printf("Successfully undocked ship %d from dock %d on attempt %d\n", 
                           ship->id, i, attempt + 1);
                    ship->isServiced = true;
                    ship->isAssignedDock = false;
                    undockSuccess = true;
                    break;
                }
                
                // Short delay between attempts
                usleep(1000);  // 1ms delay
            }
            
            if (!undockSuccess) {
                printf("Failed to undock ship %d from dock %d after multiple attempts\n", ship->id, i);
                undockingFailed = true;
            }
        }
    }
    
    // Return the status of undocking operations
    return !undockingFailed;
}

bool updateTimestep() {
    // Keep trying undocking until all eligible ships are processed
    bool undockingCompleted;
    do {
        undockingCompleted = true;
        
        // Check for ships that need undocking
        for (int i = 0; i < numDocks; i++) {
            if (docks[i].isOccupied && docks[i].allCargoMoved && 
                docks[i].lastCargoMovedTimestep < currentTimestep) {
                undockingCompleted = false;
               // printf("Dock %d has ship %d needing undocking\n", i, docks[i].occupiedByShipId);
                break;
            }
        }
        
        if (!undockingCompleted) {
            //printf("Persistent undocking attempts at timestep %d\n", currentTimestep);
            attemptUndocking();
            
            // Add delay between attempts to prevent tight looping
           // usleep(10000); // 10ms delay
        }
    } while (!undockingCompleted);

    // Proceed with timestep update after successful undocking
    printf("All ships undocked successfully, proceeding to timestep %d\n", currentTimestep + 1);
    
    // Prepare message to update timestep
    MessageStruct message;
    message.mtype = 5;
    
    if (msgsnd(mainQueueId, &message, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
        perror("Error sending timestep update message");
        exit(1);
    }
    
    currentTimestep++;
    return true;
}

bool moveCargoItem(Ship *ship, Dock *dock, int cargoId, int craneId) {
    // Send cargo movement message
    MessageStruct message;
    message.mtype = 4;
    message.shipId = ship->id;
    message.direction = ship->direction;
    message.dockId = dock->id;
    message.cargoId = cargoId;
    message.craneId = craneId;
    
    if (msgsnd(mainQueueId, &message, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
        perror("Error sending cargo movement message");
        exit(1);
    }
    
    return true;
}

void moveCargoItems() {
    // Create an array to track which cranes have been used in this timestep
    bool usedCranes[MAX_DOCKS][MAX_CARGO_COUNT] = {false};
    
    // Process docks in order
    for (int i = 0; i < numDocks; i++) {
        // Skip if dock is not occupied or was just assigned this timestep
        if (!docks[i].isOccupied || docks[i].dockingTimestep >= currentTimestep) {
            continue;
        }
        
        // Find the ship at this dock
        Ship *ship = NULL;
        for (int j = 0; j < shipCount; j++) {
            if (ships[j]->id == docks[i].occupiedByShipId && 
                ships[j]->direction == docks[i].occupiedByDirection && 
                ships[j]->isAssignedDock && 
                ships[j]->assignedDockId == i) {
                ship = ships[j];
                break;
            }
        }
        
        if (ship == NULL) {
            printf("Warning: No ship found at dock %d\n", i);
            continue;
        }
        
        // Check if all cargo already moved
        if (ship->cargosMovedCount >= ship->numCargo) {
            docks[i].allCargoMoved = true;
            continue;
        }
        
        // Get unmoved cargo indices and sort by weight (descending)
        int cargoIndices[MAX_CARGO_COUNT];
        int cargoCount = 0;
        
        for (int j = 0; j < ship->numCargo; j++) {
            if (!ship->cargoMoved[j]) {
                cargoIndices[cargoCount++] = j;
            }
        }
        
        // Sort by cargo weight (descending)
        for (int j = 0; j < cargoCount - 1; j++) {
            for (int k = 0; k < cargoCount - j - 1; k++) {
                if (ship->cargo[cargoIndices[k]] < ship->cargo[cargoIndices[k + 1]]) {
                    int temp = cargoIndices[k];
                    cargoIndices[k] = cargoIndices[k + 1];
                    cargoIndices[k + 1] = temp;
                }
            }
        }
        
        // Try to match cargo with cranes optimally
        int cargoProcessed = 0;
        
        // First pass: Try optimal assignments (cargo to smallest sufficient crane)
        for (int j = 0; j < cargoCount; j++) {
            int cargoIdx = cargoIndices[j];
            int cargoWeight = ship->cargo[cargoIdx];
            
            // Try to find the smallest crane that can handle this cargo
            for (int k = 0; k < docks[i].category; k++) {
                if (!usedCranes[i][k] && docks[i].craneCapacities[k] >= cargoWeight) {
                    // We found a crane that can handle this cargo
                    if (moveCargoItem(ship, &docks[i], cargoIdx, k)) {
                        ship->cargoMoved[cargoIdx] = true;
                        ship->cargosMovedCount++;
                        docks[i].lastCargoMovedTimestep = currentTimestep;
                        usedCranes[i][k] = true;
                        cargoProcessed++;
                        break;
                    }
                }
            }
        }
        
        // Check if all cargo has been moved now
        if (ship->cargosMovedCount == ship->numCargo) {
            docks[i].allCargoMoved = true;
            //printf("All cargo moved for ship %d at dock %d\n", ship->id, i);
        } else {
            // printf("Processed %d cargo items this timestep for ship %d at dock %d (%d/%d total)\n", 
            //        cargoProcessed, ship->id, i, ship->cargosMovedCount, ship->numCargo);
        }
    }
}

bool canDockShip(Ship *ship, Dock *dock) {
    // Check if dock is free
    if (dock->isOccupied) {
        return false;
    }
    
    // Check category constraint
    if (dock->category < ship->category) {
        return false;
    }
    
    // Check if any crane can handle the max cargo weight
    if (dock->maxCraneCapacity < ship->maxCargoWeight) {
        return false;
    }
    
    // All checks passed
    return true;
}

void dockShip(Ship *ship, Dock *dock) {
    // Send dock assignment message
    MessageStruct message;
    message.mtype = 2;
    message.shipId = ship->id;
    message.direction = ship->direction;
    message.dockId = dock->id;
    
    if (msgsnd(mainQueueId, &message, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
        perror("Error sending dock assignment message");
        exit(1);
    }
    
    // Update ship and dock status
    ship->isAssignedDock = true;
    ship->assignedDockId = dock->id;
    
    dock->isOccupied = true;
    dock->occupiedByShipId = ship->id;
    dock->occupiedByDirection = ship->direction;
    dock->dockingTimestep = currentTimestep;
    dock->lastCargoMovedTimestep = currentTimestep;
    dock->allCargoMoved = false;
}

void performDockAssignment() {
    // Sort ships by priority (descending)
    Ship *sortedShips[MAX_SHIP_REQUESTS];
    int sortedShipCount = 0;
    
    for (int i = 0; i < shipCount; i++) {
        if (!ships[i]->isAssignedDock && !ships[i]->isServiced) {
            // Skip emergency ships as they are handled separately
            if (ships[i]->direction == 1 && ships[i]->emergency == 1) {
                continue;
            }
            
            // For regular ships, check if they are within waiting time
            if (ships[i]->direction == 1 && ships[i]->waitingTime >= 0) {
                if (currentTimestep > ships[i]->arrivalTimestep + ships[i]->waitingTime) {
                    continue;  // Skip this ship as its waiting time has expired
                }
            }
            
            sortedShips[sortedShipCount++] = ships[i];
        }
    }
    
    // Sort by priority (descending)
    for (int i = 0; i < sortedShipCount - 1; i++) {
        for (int j = 0; j < sortedShipCount - i - 1; j++) {
            if (sortedShips[j]->priority < sortedShips[j + 1]->priority) {
                Ship *temp = sortedShips[j];
                sortedShips[j] = sortedShips[j + 1];
                sortedShips[j + 1] = temp;
            }
        }
    }
    
    // Find and sort free docks
    Dock *freeDocks[MAX_DOCKS];
    int freeDockCount = 0;
    
    for (int i = 0; i < numDocks; i++) {
        if (!docks[i].isOccupied) {
            freeDocks[freeDockCount++] = &docks[i];
        }
    }
    
    // Sort docks by category (ascending) to use smaller docks first
    for (int i = 0; i < freeDockCount - 1; i++) {
        for (int j = 0; j < freeDockCount - i - 1; j++) {
            if (freeDocks[j]->category > freeDocks[j + 1]->category) {
                Dock *temp = freeDocks[j];
                freeDocks[j] = freeDocks[j + 1];
                freeDocks[j + 1] = temp;
            }
        }
    }
    
    // Assign docks to ships
    for (int i = 0; i < sortedShipCount; i++) {
        Ship *ship = sortedShips[i];
        
        // Find the smallest adequate dock for this ship
        for (int j = 0; j < freeDockCount; j++) {
            if (canDockShip(ship, freeDocks[j])) {
                dockShip(ship, freeDocks[j]);
                
                // Mark dock as used
                freeDocks[j] = freeDocks[--freeDockCount];
                break;
            }
        }
    }
}

void sortEmergencyShips(Ship **emergencyShips, int count) {  
    // First sort by arrival timestep (FIFO)
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (emergencyShips[j]->arrivalTimestep > emergencyShips[j + 1]->arrivalTimestep) {
                Ship *temp = emergencyShips[j];
                emergencyShips[j] = emergencyShips[j + 1];
                emergencyShips[j + 1] = temp;
            }
        }
    }
    
    // Then sort by smallest ship category (to maximize dock assignments)
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (emergencyShips[j]->arrivalTimestep == emergencyShips[j + 1]->arrivalTimestep && 
                emergencyShips[j]->category > emergencyShips[j + 1]->category) {
                Ship *temp = emergencyShips[j];
                emergencyShips[j] = emergencyShips[j + 1];
                emergencyShips[j + 1] = temp;
            }
        }
    }
}

void assignDocksToEmergencyShips() {
    // Count emergency ships that haven't been assigned a dock
    int emergencyShipCount = 0;
    Ship *emergencyShips[MAX_SHIP_REQUESTS];
    
    for (int i = 0; i < shipCount; i++) {
        if (ships[i]->direction == 1 && ships[i]->emergency == 1 && !ships[i]->isAssignedDock && !ships[i]->isServiced) {
            emergencyShips[emergencyShipCount++] = ships[i];
        }
    }
    
    if (emergencyShipCount == 0) {
        return;  // No emergency ships to handle
    }
    
    // Sort emergency ships by arrival time and then by category (ascending)
    sortEmergencyShips(emergencyShips, emergencyShipCount);
    
    // Count free docks
    int freeDockCount = 0;
    Dock *freeDocks[MAX_DOCKS];
    
    for (int i = 0; i < numDocks; i++) {
        if (!docks[i].isOccupied) {
            freeDocks[freeDockCount++] = &docks[i];
        }
    }
    
    if (freeDockCount == 0) {
        return;  // No free docks available
    }
    
    // Sort free docks by category (descending)
    for (int i = 0; i < freeDockCount - 1; i++) {
        for (int j = 0; j < freeDockCount - i - 1; j++) {
            if (freeDocks[j]->category < freeDocks[j + 1]->category) {
                Dock *temp = freeDocks[j];
                freeDocks[j] = freeDocks[j + 1];
                freeDocks[j + 1] = temp;
            }
        }
    }
    
    // Try to assign as many emergency ships as possible to free docks
    bool *dockAssigned = (bool *)calloc(freeDockCount, sizeof(bool));
    
    for (int i = 0; i < emergencyShipCount; i++) {
        Ship *ship = emergencyShips[i];
        
        // Find the smallest category dock that can accommodate this ship
        // and has a crane that can handle the max cargo weight
        for (int j = freeDockCount - 1; j >= 0; j--) {
            if (!dockAssigned[j] && 
                freeDocks[j]->category >= ship->category && 
                freeDocks[j]->maxCraneCapacity >= ship->maxCargoWeight) {
                
                dockAssigned[j] = true;
                
                // Send dock assignment message
                dockShip(ship, freeDocks[j]);
                break;
            }
        }
    }
    
    free(dockAssigned);
}

void processAllRequests() {
    bool finished = false;
    int completion = 0;
    
    while (!finished) {
        // Read message from validation module
        MessageStruct message;
        if (msgrcv(mainQueueId, &message, sizeof(MessageStruct) - sizeof(long), 1, 0) == -1) {
            perror("Error receiving message from validation");
            exit(1);
        }
        
        // Save message info to global message 
        globalMessage = message;
        
        //printf("\n=== Processing timestep %d, %d ship requests ===\n", 
               //message.timestep, message.numShipRequests);
        
        // Check if all ship requests have been serviced (validation says we're done)
        if (message.isFinished == 1) {
            printf("Received completion message from validation module.\n");
            finished = true;
            
            while (!checkIfAllShipsServiced()) {
                //printf("Not all ships are serviced yet. Continuing processing.\n");
                
                // Process remaining ships
                assignDocksToEmergencyShips();
                performDockAssignment();
                moveCargoItems();
                
                // Ensure undocking is attempted and completed before moving to next timestep
                bool undockingCompleted = true;
                do {
                    undockingCompleted = true;
                    
                    // Check for ships that need undocking
                    for (int i = 0; i < numDocks; i++) {
                        if (docks[i].isOccupied && docks[i].allCargoMoved && 
                            docks[i].lastCargoMovedTimestep < currentTimestep) {
                            undockingCompleted = false;
                            
                            // Find the ship at this dock
                            Ship *ship = NULL;
                            for (int j = 0; j < shipCount; j++) {
                                if (ships[j]->id == docks[i].occupiedByShipId && 
                                    ships[j]->direction == docks[i].occupiedByDirection &&
                                    ships[j]->isAssignedDock && 
                                    ships[j]->assignedDockId == i) {
                                    ship = ships[j];
                                    break;
                                }
                            }
                            
                            if (ship != NULL) {
                                printf("Attempting to undock ship %d from dock %d (completion phase)\n", 
                                       ship->id, i);
                                
                                if (undockShip(&docks[i])) {
                                    printf("Successfully undocked ship %d from dock %d\n", ship->id, i);
                                    ship->isServiced = true;
                                    ship->isAssignedDock = false;
                                } else {
                                    printf("Failed to undock ship %d from dock %d\n", ship->id, i);
                                }
                            }
                        }
                    }
                    
                    // If undocking is not complete, retry guessing auth strings
                    if (!undockingCompleted) {
                        attemptUndocking();
                    }
                    
                } while (!undockingCompleted);
                
                updateTimestep();
                
            }
            
            // Send final completion message
            MessageStruct completionMsg;
            completionMsg.mtype = 6;
            completionMsg.isFinished = 1;
            
            printf("All ships serviced. Sending completion message.\n");
            if (msgsnd(mainQueueId, &completionMsg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                perror("Error sending completion message");
                exit(1);
            }
            
            completion = 1;
            break;
        }
        
        if (completion == 1) {
            break;
        }
        
        // Process new ship requests
        processNewShipRequests(message.numShipRequests);
        
        // Calculate ship properties and priorities
        calculateShipProperties();
        prioritizeShips();

        // Check if all ships are already serviced
        if (checkIfAllShipsServiced()) {
            // Send completion message
            MessageStruct completionMsg;
            completionMsg.mtype = 6;
            completionMsg.isFinished = 1;
            
            printf("All ships already serviced. Sending completion message.\n");
            if (msgsnd(mainQueueId, &completionMsg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                perror("Error sending completion message");
                exit(1);
            }
        }
        
        // Process ships in priority order
        assignDocksToEmergencyShips();
        performDockAssignment();
        moveCargoItems();
        
        // Ensure all occupied docks performed an action this timestep
        bool allDocksActive = true;
        for (int i = 0; i < numDocks; i++) {
            if (docks[i].isOccupied) {
                // Check if this dock performed cargo movement this timestep
                if (docks[i].lastCargoMovedTimestep < currentTimestep && !docks[i].allCargoMoved) {
                    printf("Warning: Dock %d with ship %d did not perform any cargo movement this timestep\n",
                           i, docks[i].occupiedByShipId);
                    
                    // Check if this dock has any cargo left to move
                    Ship *ship = NULL;
                    for (int j = 0; j < shipCount; j++) {
                        if (ships[j]->id == docks[i].occupiedByShipId && 
                            ships[j]->direction == docks[i].occupiedByDirection) {
                            ship = ships[j];
                            break;
                        }
                    }
                    
                    if (ship != NULL && ship->cargosMovedCount < ship->numCargo) {
                        allDocksActive = false;
                    }
                }
            }
        }
        
        if (!allDocksActive) {
            // Try to move cargo again for inactive docks
            //printf("Some docks were inactive, attempting to move cargo again\n");
            moveCargoItems();
        }
        
        // Ensure undocking is attempted and completed for ships that are ready
        bool undockingAttempted = false;
        for (int i = 0; i < numDocks; i++) {
            if (docks[i].isOccupied && docks[i].allCargoMoved &&
                docks[i].lastCargoMovedTimestep < currentTimestep) {
                //printf("Ship at dock %d has completed cargo operations and should be undocked\n", i);
                undockingAttempted = true;
                break;
            }
        }
        
        if (undockingAttempted) {
            // Attempt undocking for all eligible ships
            attemptUndocking();
            
            // Check if any ships still need undocking
            bool stillNeedUndocking = false;
            for (int i = 0; i < numDocks; i++) {
                if (docks[i].isOccupied && docks[i].allCargoMoved &&
                    docks[i].lastCargoMovedTimestep < currentTimestep) {
                    //printf("Ship at dock %d still needs undocking after attempt\n", i);
                    stillNeedUndocking = true;
                    
                    // try one more aggressive attempt to  undock
                    Ship *ship = NULL;
                    for (int j = 0; j < shipCount; j++) {
                        if (ships[j]->id == docks[i].occupiedByShipId && 
                            ships[j]->direction == docks[i].occupiedByDirection) {
                            ship = ships[j];
                            break;
                        }
                    }
                    
                    if (ship != NULL) {
                        //printf("Making extra attempt to undock ship %d from dock %d\n", ship->id, i);
                        
                        // Retry undocking with more attempts for guessing auth string
                        if (undockShip(&docks[i])) {
                           // printf("Successfully undocked ship %d on extra attempt\n", ship->id);
                            ship->isServiced = true;
                            ship->isAssignedDock = false;
                        }
                    }
                }
            }
            
            if (stillNeedUndocking) {
                printf("Warning: Some ships still need undocking, but continuing to next timestep\n");
            }
        }
        
        updateTimestep();
        
    }
}
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <test_case_number>\n", argv[0]);
        return 1;
    }
    char filename[256];
    snprintf(filename, sizeof(filename), "testcase%s/input.txt", argv[1]);

    initializeIPC(filename);

    processAllRequests();

    return 0;
}

