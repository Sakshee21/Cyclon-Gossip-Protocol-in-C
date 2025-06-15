#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <time.h>

#define MAX_BUFFER_SIZE 1024
#define MAX_USERS 100
#define MAX_CACHED_MSGS 50
#define VIEW_LENGTH 3
#define SWAP_LENGTH 2
#define FORWARD_COUNT 2

typedef struct {
    char id[50];
    char ipaddr[50];
    int port;
    time_t timestamp;
} NodeDescriptor;

typedef struct {
    NodeDescriptor descriptors[VIEW_LENGTH];
    int count;             // Current number of descriptors in view
} View;

void error(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

// Find the oldest descriptor in the view
int find_oldest_descriptor(View *view) {
    if (view->count == 0) return -1;

    int oldest_idx = 0;
    time_t oldest_time = view->descriptors[0].timestamp;

    for (int i = 1; i < view->count; i++) {
        if (view->descriptors[i].timestamp < oldest_time) {
            oldest_time = view->descriptors[i].timestamp;
            oldest_idx = i;
        }
    }

    return oldest_idx;
}

// Remove a descriptor at specified index from the view
NodeDescriptor remove_descriptor(View *view, int index) {
    if (index < 0 || index >= view->count) {
        NodeDescriptor empty;
        memset(&empty, 0, sizeof(NodeDescriptor));
        return empty;
    }

    NodeDescriptor removed = view->descriptors[index];

    // Shift remaining elements
    for (int i = index; i < view->count - 1; i++) {
        view->descriptors[i] = view->descriptors[i + 1];
    }

    view->count--;
    return removed;
}

// Add a descriptor to the view if there's space
int add_descriptor(View *view, NodeDescriptor descriptor) {
    // Don't add if view is full
    if (view->count >= VIEW_LENGTH) {
        return 0;
    }

    // Don't add descriptors with empty id
    if (strlen(descriptor.id) == 0) {
        return 0;
    }

    // Check if already exists
    for (int i = 0; i < view->count; i++) {
        if (strcmp(view->descriptors[i].id, descriptor.id) == 0) {
            // Update timestamp if it already exists
            view->descriptors[i].timestamp = descriptor.timestamp;
            return 0; // Already exists (but timestamp updated)
        }
    }

    view->descriptors[view->count++] = descriptor;
    return 1;
}

// Update or add descriptor in view
int update_descriptor(View *view, NodeDescriptor descriptor) {
    // Don't add descriptors with empty id
    if (strlen(descriptor.id) == 0) {
        return 0;
    }

    // Check if already exists
    for (int i = 0; i < view->count; i++) {
        if (strcmp(view->descriptors[i].id, descriptor.id) == 0) {
            // Update timestamp
            view->descriptors[i].timestamp = descriptor.timestamp;
            return 1; // Updated existing
        }
    }

    // Add if not exists and we have space
    if (view->count < VIEW_LENGTH) {
        view->descriptors[view->count++] = descriptor;
        return 1;
    }

    return 0;
}

// Select random descriptors from view (and remove them)
int select_random_descriptors(View *view, NodeDescriptor *selected, int count) {
    if (view->count == 0) return 0;

    int selected_count = 0;
    int indices[VIEW_LENGTH];
    int available = view->count;

    // Initialize indices array
    for (int i = 0; i < view->count; i++) {
        indices[i] = i;
    }

    // Don't try to select more than what's available
    count = (count < available) ? count : available;

    // Fisher-Yates shuffle to select random indices
    for (int i = 0; i < count && available > 0; i++) {
        int j = rand() % available;
        selected[selected_count++] = view->descriptors[indices[j]];

        // Remove the selected descriptor
        for (int k = indices[j]; k < view->count - 1; k++) {
            view->descriptors[k] = view->descriptors[k + 1];
        }
        view->count--;

        // Update indices array
        indices[j] = indices[available - 1];
        available--;
    }

    return selected_count;
}

// Check if a message has been seen before
int is_duplicate_message(char *msg, char cached_msgs[][MAX_BUFFER_SIZE], int *msg_count) {
    for (int i = 0; i < *msg_count; i++) {
        if (strcmp(msg, cached_msgs[i]) == 0) {
            return 1;
        }
    }

    // Add to cache if not found
    if (*msg_count < MAX_CACHED_MSGS) {
        strcpy(cached_msgs[*msg_count], msg);
        (*msg_count)++;
    } else {
        // Replace oldest message (simple FIFO)
        for (int i = 0; i < MAX_CACHED_MSGS - 1; i++) {
            strcpy(cached_msgs[i], cached_msgs[i+1]);
        }
        strcpy(cached_msgs[MAX_CACHED_MSGS-1], msg);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int srvsock, portno, maxfd, result;
    struct sockaddr_in serveraddr, clientaddr;
    socklen_t len = sizeof(clientaddr);
    fd_set readset, tempset;
    char buf[MAX_BUFFER_SIZE];
    char cached_msgs[MAX_CACHED_MSGS][MAX_BUFFER_SIZE];
    int msg_count = 0;

    // Remember the last gossip partner to avoid repeat exchanges
    NodeDescriptor last_partner;
    memset(&last_partner, 0, sizeof(NodeDescriptor));

    // Initialize random seed
    srand(time(NULL) ^ getpid());

    // Read users from file
    FILE *userFile = fopen("users.txt", "r");
    if (!userFile) error("Error opening users.txt");

    NodeDescriptor allUsers[MAX_USERS];
    int userCount = 0;

    while (fscanf(userFile, "%s %s %d",
                 allUsers[userCount].id,
                 allUsers[userCount].ipaddr,
                 &allUsers[userCount].port) == 3) {
        allUsers[userCount].timestamp = time(NULL);
        userCount++;
    }
    fclose(userFile);

    if (userCount < 2) {
        error("Need at least 2 users in users.txt");
    }

    // Find my own descriptor
    portno = atoi(argv[1]);
    NodeDescriptor myDescriptor;
    int myIndex = -1;

    for (int i = 0; i < userCount; i++) {
        if (allUsers[i].port == portno) {
            myDescriptor = allUsers[i];
            myIndex = i;
            break;
        }
    }

    if (myIndex == -1) error("No matching user found for the provided port");

    // Initialize socket
    srvsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (srvsock < 0) error("ERROR opening socket");

    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = INADDR_ANY;
    serveraddr.sin_port = htons(portno);

    if (bind(srvsock, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
        error("ERROR on binding");
    }

    // Initialize my view with RANDOM subset of nodes (proper bootstrapping)
    View myView;
    myView.count = 0;

    // Create array of indices excluding myself
    int otherIndices[MAX_USERS];
    int otherCount = 0;
    for (int i = 0; i < userCount; i++) {
        if (i != myIndex) {
            otherIndices[otherCount++] = i;
        }
    }

    // Shuffle the indices
    for (int i = otherCount - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int temp = otherIndices[i];
        otherIndices[i] = otherIndices[j];
        otherIndices[j] = temp;
    }

    // Add at most VIEW_LENGTH random nodes (excluding myself)
    int initialViewSize = (otherCount < VIEW_LENGTH) ? otherCount : VIEW_LENGTH;
    for (int i = 0; i < initialViewSize; i++) {
        add_descriptor(&myView, allUsers[otherIndices[i]]);
    }

    printf("Node %s initialized with %d nodes in view\n", myDescriptor.id, myView.count);

    // Display initial view
    printf("Initial view contents:\n");
    for (int i = 0; i < myView.count; i++) {
        printf("  %d. %s (%s:%d)\n",
               i+1,
               myView.descriptors[i].id,
               myView.descriptors[i].ipaddr,
               myView.descriptors[i].port);
    }

    // Set up for select()
    FD_ZERO(&readset);
    FD_SET(srvsock, &readset);
    FD_SET(STDIN_FILENO, &readset);
    maxfd = (srvsock > STDIN_FILENO) ? srvsock : STDIN_FILENO;

    // Set up cyclic timer for Cyclon protocol (every 10 seconds)
    time_t last_cycle_time = time(NULL);
    int cycle_interval = 10; // seconds

    while (1) {
        // Set timeout for select to handle cyclic behavior
        struct timeval tv;
        tv.tv_sec = 1;  // Check every second
        tv.tv_usec = 0;

        tempset = readset;
        result = select(maxfd + 1, &tempset, NULL, NULL, &tv);

        if (result < 0) error("ERROR on select");

        // Check if it's time for a Cyclon cycle
        time_t current_time = time(NULL);
        if ((current_time - last_cycle_time) >= cycle_interval) {
            last_cycle_time = current_time;

            if (myView.count > 0) {
                printf("\n[CYCLON CYCLE] Initiating gossip exchange\n");

                // Step 1: Select oldest node from view
                int oldest_idx = find_oldest_descriptor(&myView);
                if (oldest_idx >= 0) {
                    NodeDescriptor partner = remove_descriptor(&myView, oldest_idx);

                    // Avoid selecting the same partner twice in a row
                    if (strcmp(partner.id, last_partner.id) == 0 && myView.count > 0) {
                        // Put this descriptor back and get next oldest
                        add_descriptor(&myView, partner);
                        oldest_idx = find_oldest_descriptor(&myView);
                        partner = remove_descriptor(&myView, oldest_idx);
                    }

                    // Save this partner as the last one selected
                    last_partner = partner;

                    printf("→ Selected gossip partner: %s:%d\n", partner.id, partner.port);

                    // Step 2: Select descriptors to send
                    NodeDescriptor to_send[SWAP_LENGTH];
                    int sendable = SWAP_LENGTH - 1; // Reserve one slot for self
                    int random_count = 0;

                    if (sendable > 0 && myView.count > 0) {
                        // Select random descriptors from view
                        random_count = select_random_descriptors(&myView, &to_send[1], sendable);
                    }

                    // First descriptor is always a fresh descriptor of myself
                    myDescriptor.timestamp = time(NULL);  // Update timestamp
                    to_send[0] = myDescriptor;

                    // Step 3: Send descriptors to partner
                    struct sockaddr_in peeraddr;
                    memset(&peeraddr, 0, sizeof(peeraddr));
                    peeraddr.sin_family = AF_INET;
                    peeraddr.sin_port = htons(partner.port);
                    inet_pton(AF_INET, partner.ipaddr, &peeraddr.sin_addr);

                    // Create message with descriptors to send
                    char msg[MAX_BUFFER_SIZE];
                    memset(msg, 0, MAX_BUFFER_SIZE);

                    // Format: "CYCLON_PUSH:<count>:<id1>:<ip1>:<port1>:<timestamp1>:..."
                    int total_to_send = 1 + random_count; // self + random
                    char *ptr = msg;
                    ptr += sprintf(ptr, "CYCLON_PUSH:%d:", total_to_send);

                    for (int i = 0; i < total_to_send; i++) {
                        ptr += sprintf(ptr, "%s:%s:%d:%ld:",
                                    to_send[i].id,
                                    to_send[i].ipaddr,
                                    to_send[i].port,
                                    to_send[i].timestamp);
                    }

                    printf("→ Sending %d descriptors to %s\n", total_to_send, partner.id);
                    sendto(srvsock, msg, strlen(msg), 0, (struct sockaddr *)&peeraddr, sizeof(peeraddr));
                }
            }
        }

        // Handle incoming messages
        if (result > 0 && FD_ISSET(srvsock, &tempset)) {
            memset(buf, 0, MAX_BUFFER_SIZE);
            recvfrom(srvsock, buf, MAX_BUFFER_SIZE, 0, (struct sockaddr *)&clientaddr, &len);

            // Parse message
            if (strncmp(buf, "CYCLON_PUSH:", 12) == 0) {
                // Another node initiated a gossip exchange with us
                printf("\n[CYCLON RECEIVED] Exchange request\n");

                // Parse the message to extract descriptors
                char *token = strtok(buf, ":");
                token = strtok(NULL, ":");  // Skip "CYCLON_PUSH"

                if (!token) continue; // Malformed message
                int count = atoi(token);
                NodeDescriptor received[VIEW_LENGTH];
                int received_count = 0;
                NodeDescriptor sender; // Keep track of who sent this request
                memset(&sender, 0, sizeof(NodeDescriptor));

                // Extract descriptors
                for (int i = 0; i < count && received_count < VIEW_LENGTH; i++) {
                    char id[50], ipaddr[50];
                    int port;
                    time_t timestamp;

                    token = strtok(NULL, ":");  // id
                    if (!token) break;
                    strcpy(id, token);

                    token = strtok(NULL, ":");  // ipaddr
                    if (!token) break;
                    strcpy(ipaddr, token);

                    token = strtok(NULL, ":");  // port
                    if (!token) break;
                    port = atoi(token);

                    token = strtok(NULL, ":");  // timestamp
                    if (!token) break;
                    timestamp = atol(token);

                    if (strlen(id) > 0 && port > 0) {
                        NodeDescriptor desc;
                        strcpy(desc.id, id);
                        strcpy(desc.ipaddr, ipaddr);
                        desc.port = port;
                        // Always use current time for freshness
                        desc.timestamp = time(NULL);

                        received[received_count++] = desc;

                        // The first descriptor is always the sender
                        if (i == 0) {
                            sender = desc;
                        }
                    }
                }

                // Step 4: Select descriptors to reply with
                NodeDescriptor to_reply[SWAP_LENGTH];
                int reply_count = 0;

                // Select random descriptors from my view
                if (myView.count > 0) {
                    reply_count = select_random_descriptors(&myView, to_reply, SWAP_LENGTH);
                }

                // Step 5: Add received descriptors to my view (excluding myself)
                int added = 0;
                for (int i = 0; i < received_count; i++) {
                    if (strcmp(received[i].id, myDescriptor.id) != 0) {
                        if (add_descriptor(&myView, received[i])) {
                            added++;
                        }
                    }
                }

                printf("→ Added %d descriptors to my view\n", added);

                // Step 6: Send reply back
                char reply[MAX_BUFFER_SIZE];
                memset(reply, 0, MAX_BUFFER_SIZE);

                // Format: "CYCLON_REPLY:<count>:<id1>:<ip1>:<port1>:<timestamp1>:..."
                char *ptr = reply;
                ptr += sprintf(ptr, "CYCLON_REPLY:%d:", reply_count);

                for (int i = 0; i < reply_count; i++) {
                    ptr += sprintf(ptr, "%s:%s:%d:%ld:",
                                to_reply[i].id,
                                to_reply[i].ipaddr,
                                to_reply[i].port,
                                to_reply[i].timestamp);
                }

                printf("→ Replying with %d descriptors\n", reply_count);
                sendto(srvsock, reply, strlen(reply), 0, (struct sockaddr *)&clientaddr, sizeof(clientaddr));

                // Add the exchange partner back to view with updated timestamp
                update_descriptor(&myView, sender);

            } else if (strncmp(buf, "CYCLON_REPLY:", 13) == 0) {
                // Received reply to our gossip request
                printf("\n[CYCLON RECEIVED] Exchange reply\n");

                // Parse the message to extract descriptors
                char *token = strtok(buf, ":");
                token = strtok(NULL, ":");  // Skip "CYCLON_REPLY"

                if (!token) continue; // Malformed message
                int count = atoi(token);
                NodeDescriptor received[VIEW_LENGTH];
                int received_count = 0;

                // Extract descriptors
                for (int i = 0; i < count && received_count < VIEW_LENGTH; i++) {
                    char id[50], ipaddr[50];
                    int port;
                    time_t timestamp;

                    token = strtok(NULL, ":");  // id
                    if (!token) break;
                    strcpy(id, token);

                    token = strtok(NULL, ":");  // ipaddr
                    if (!token) break;
                    strcpy(ipaddr, token);

                    token = strtok(NULL, ":");  // port
                    if (!token) break;
                    port = atoi(token);

                    token = strtok(NULL, ":");  // timestamp
                    if (!token) break;
                    timestamp = atol(token);

                    if (strlen(id) > 0 && port > 0) {
                        NodeDescriptor desc;
                        strcpy(desc.id, id);
                        strcpy(desc.ipaddr, ipaddr);
                        desc.port = port;
                        // Always use current time for freshness
                        desc.timestamp = time(NULL);
                        received[received_count++] = desc;
                    }
                }

                // Add received descriptors to my view (excluding myself)
                int added = 0;
                for (int i = 0; i < received_count; i++) {
                    if (strcmp(received[i].id, myDescriptor.id) != 0) {
                        if (add_descriptor(&myView, received[i])) {
                            added++;
                        }
                    }
                }

                printf("→ Added %d descriptors to my view\n", added);

                // Add the last partner back with a fresh timestamp
                last_partner.timestamp = time(NULL);
                update_descriptor(&myView, last_partner);
            } else {
                // Regular gossip message
                printf("\n[GOSSIP RECEIVED] %s\n", buf);

                // Check if we've seen this message before
                if (!is_duplicate_message(buf, cached_msgs, &msg_count)) {
                    // Forward to random peers
                    if (myView.count > 0) {
                        // Shuffle view indices for random selection
                        int indices[VIEW_LENGTH];
                        for (int i = 0; i < myView.count; i++) {
                            indices[i] = i;
                        }

                        // Fisher-Yates shuffle
                        for (int i = myView.count - 1; i > 0; i--) {
                            int j = rand() % (i + 1);
                            int temp = indices[i];
                            indices[i] = indices[j];
                            indices[j] = temp;
                        }

                        // Select up to FORWARD_COUNT peers
                        int forward_to = (myView.count < FORWARD_COUNT) ? myView.count : FORWARD_COUNT;

                        struct sockaddr_in peeraddr;
                        printf("→ Forwarding to peers:\n");

                        for (int i = 0; i < forward_to; i++) {
                            memset(&peeraddr, 0, sizeof(peeraddr));
                            peeraddr.sin_family = AF_INET;
                            peeraddr.sin_port = htons(myView.descriptors[indices[i]].port);
                            inet_pton(AF_INET, myView.descriptors[indices[i]].ipaddr, &peeraddr.sin_addr);

                            printf("   → Peer: %s (%s:%d)\n",
                                myView.descriptors[indices[i]].id,
                                myView.descriptors[indices[i]].ipaddr,
                                myView.descriptors[indices[i]].port);

                            sendto(srvsock, buf, strlen(buf), 0,
                                (struct sockaddr *)&peeraddr, sizeof(peeraddr));
                        }
                    } else {
                        printf("→ No peers in view to forward message to\n");
                    }
                } else {
                    printf("→ Duplicate message, not forwarding\n");
                }
            }
        }

        // Handle user input
        if (result > 0 && FD_ISSET(STDIN_FILENO, &tempset)) {
            memset(buf, 0, MAX_BUFFER_SIZE);
            read(STDIN_FILENO, buf, MAX_BUFFER_SIZE);
            buf[strcspn(buf, "\n")] = 0;

            if (strcmp(buf, "BYE") == 0) {
                printf("Exiting...\n");
                break;
            } else if (strcmp(buf, "VIEW") == 0) {
                // Print current view
                printf("\n[VIEW] Current view (%d nodes):\n", myView.count);
                for (int i = 0; i < myView.count; i++) {
                    printf("  %d. %s (%s:%d) [age: %lds]\n",
                           i+1,
                           myView.descriptors[i].id,
                           myView.descriptors[i].ipaddr,
                           myView.descriptors[i].port,
                           time(NULL) - myView.descriptors[i].timestamp);
                }
            } else if (strcmp(buf, "CYCLE") == 0) {
                // Force a Cyclon cycle
                last_cycle_time = 0;  // This will trigger a cycle on next iteration
            } else {
                // Regular gossip message
                char formattedMessage[MAX_BUFFER_SIZE];
                snprintf(formattedMessage, MAX_BUFFER_SIZE, "%s: %.900s", myDescriptor.id, buf);

                printf("\n[GOSSIP SENT] %s\n", formattedMessage);

                // Add to cached messages to avoid receiving our own message back
                is_duplicate_message(formattedMessage, cached_msgs, &msg_count);

                // Select random nodes from view to send to
                if (myView.count > 0) {
                    // Shuffle view indices for random selection
                    int indices[VIEW_LENGTH];
                    for (int i = 0; i < myView.count; i++) {
                        indices[i] = i;
                    }

                    // Fisher-Yates shuffle
                    for (int i = myView.count - 1; i > 0; i--) {
                        int j = rand() % (i + 1);
                        int temp = indices[i];
                        indices[i] = indices[j];
                        indices[j] = temp;
                    }

                    // Select up to FORWARD_COUNT peers
                    int send_to = (myView.count < FORWARD_COUNT) ? myView.count : FORWARD_COUNT;

                    struct sockaddr_in peeraddr;
                    printf("→ Sending to peers:\n");

                    for (int i = 0; i < send_to; i++) {
                        memset(&peeraddr, 0, sizeof(peeraddr));
                        peeraddr.sin_family = AF_INET;
                        peeraddr.sin_port = htons(myView.descriptors[indices[i]].port);
                        inet_pton(AF_INET, myView.descriptors[indices[i]].ipaddr, &peeraddr.sin_addr);

                        printf("   → Peer: %s (%s:%d)\n",
                            myView.descriptors[indices[i]].id,
                            myView.descriptors[indices[i]].ipaddr,
                            myView.descriptors[indices[i]].port);

                        sendto(srvsock, formattedMessage, strlen(formattedMessage), 0,
                            (struct sockaddr *)&peeraddr, sizeof(peeraddr));
                    }
                } else {
                    printf("→ No peers in view to send message to\n");
                }
            }
        }
    }

    close(srvsock);
    return 0;
}