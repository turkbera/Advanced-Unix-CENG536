#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/mman.h>

#define MAX_CLIENTS 1000
#define MAX_SUPPLY 10000
#define MAX_DEMAND 10000
#define MAX_WATCH 1000
#define MAX_NOTIFICATIONS 1000

typedef struct {
    int x;
    int y;
    int a_amount;
    int b_amount;
    int c_amount;
    int distance;
    int client_id;
} supply;

typedef struct {
    int x;
    int y;
    int a_amount;
    int b_amount;
    int c_amount;
    int client_id;
} demand;

typedef struct {
    int x;
    int y;
    int client_id;
    int watch_id;
} watch_t;

typedef struct {
    char message[256];
} notification;

typedef struct
{
    int client_id;
    int x;
    int y;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int client_socket;

    // Notification queue
    notification notifications[MAX_NOTIFICATIONS];
    int notif_head;
    int notif_tail;
} client;

typedef struct
{
    supply supplies[MAX_SUPPLY];
    demand demands[MAX_DEMAND];
    watch_t watches[MAX_WATCH];
    pthread_mutex_t mutex;
    client clients[MAX_CLIENTS];
} shared_mem;

typedef struct {
    int sockfd;
    int client_id;
} thread_arg;

shared_mem *shm;

void client_agent(int childfd);
int add_new_supply(int client_id, int distance, int a, int b, int c);
void add_new_demand(int client_id, int a, int b, int c);
void add_new_watch(int client_id, int new_watch_id);
void remove_watch(int client_id);
void list_supplies(int client_id);
void list_demands(int client_id);
void my_supplies(int client_id);
void my_demands(int client_id);
void move_client(int client_id, int x, int y);
int check_for_match(int client_id);
void check_for_watch_events_on_new_supply(int supply_index);
void register_client(int *client_id, int sockfd);
int manhattan_distance(int x1, int y1, int x2, int y2);
int check_case_match(int demand_id, int supply_id);
void match_demand_and_supply(int demand_id, int supply_id);
void notify_client(int client_socket, const char *message);
void *command_thread_func(void *arg);
void *notification_thread_func(void *args);
void cleanup_shared_memory();
void remove_client_resources(int client_id);
void enqueue_notification(int client_id, const char *msg);
void remove_demand(int demand_id);
void remove_supply(int supply_id);

int main(int argc, char** argv){

    int acceptfd;
    
    shm = mmap(NULL, sizeof(shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }

    memset(shm, 0, sizeof(shared_mem));

    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shm->mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);

    signal(SIGCHLD, SIG_IGN);

    for (int i=0; i<MAX_CLIENTS; i++) {
        shm->clients[i].client_id = -1;
        shm->clients[i].client_socket = -1;
        shm->clients[i].notif_head = 0;
        shm->clients[i].notif_tail = 0;

        pthread_mutexattr_t cattr;
        pthread_condattr_t condattr;
        pthread_mutexattr_init(&cattr);
        pthread_condattr_init(&condattr);
        pthread_mutexattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
        pthread_condattr_setpshared(&condattr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&shm->clients[i].mutex, &cattr);
        pthread_cond_init(&shm->clients[i].condition, &condattr);
        pthread_mutexattr_destroy(&cattr);
        pthread_condattr_destroy(&condattr);
    }

    for (int i=0; i<MAX_DEMAND; i++){
        shm->demands[i].client_id = -1;
    }
    for (int i=0; i<MAX_SUPPLY; i++) shm->supplies[i].client_id = -1;
    for (int i=0; i<MAX_WATCH; i++) shm->watches[i].client_id = -1;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <conn> <width> <height>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *conn = argv[1];
    // width and height not directly used
    int width = atoi(argv[2]);
    int height = atoi(argv[3]);

    if(conn[0] == '@'){
        struct sockaddr_un serv_addr_unix;
        memset(&serv_addr_unix, 0, sizeof(struct sockaddr_un));
        serv_addr_unix.sun_family = AF_UNIX;
        strncpy(serv_addr_unix.sun_path, conn+1, sizeof(serv_addr_unix.sun_path)-1);
        serv_addr_unix.sun_path[sizeof(serv_addr_unix.sun_path)-1] = '\0';
        acceptfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if(acceptfd < 0){
            perror("socket");
            exit(EXIT_FAILURE);
        }
        unlink(serv_addr_unix.sun_path);    

        int opt = 1;
        if (setsockopt(acceptfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("setsockopt");
            close(acceptfd);
            exit(EXIT_FAILURE);
        }
        if(bind(acceptfd, (struct sockaddr *)&serv_addr_unix, sizeof(serv_addr_unix)) < 0){
            perror("bind");
            exit(EXIT_FAILURE);
        }

        listen(acceptfd, SOMAXCONN);
        while(1){
            int childfd = accept(acceptfd, NULL, NULL);
            if(childfd < 0){
                perror("accept");
                exit(EXIT_FAILURE);
            }
            pid_t pid = fork();
            if(pid== 0){
                close(acceptfd);
                client_agent(childfd);
                close(childfd);
                exit(EXIT_SUCCESS);

            } else {
                close(childfd);
            }

        }
        
    } else {
        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(struct sockaddr_in));

        char conn_copy[256];
        strncpy(conn_copy, conn, sizeof(conn_copy)-1);
        conn_copy[sizeof(conn_copy)-1] = '\0';

        char *ip = strtok(conn_copy, ":");
        char *port_str = strtok(NULL, ":");
        if (!port_str) {
            fprintf(stderr, "Invalid conn format. Expected ip:port\n");
            exit(EXIT_FAILURE);
        }

        int port = atoi(port_str);
        serv_addr.sin_port = htons(port);
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = inet_addr(ip);

        acceptfd = socket(AF_INET, SOCK_STREAM, 0);
        if(acceptfd < 0){
            perror("socket");
            exit(EXIT_FAILURE);
        }
        int opt = 1;
        if (setsockopt(acceptfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("setsockopt");
            close(acceptfd);
            exit(EXIT_FAILURE);
        }
       
        if(bind(acceptfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0){
            perror("bind");
            exit(EXIT_FAILURE);
        }

        listen(acceptfd, SOMAXCONN);

        while(1){
            int childfd = accept(acceptfd, NULL, NULL);
            if(childfd < 0){
                perror("accept");
                exit(EXIT_FAILURE);
            }

            pid_t pid = fork();
            if(pid == 0){
                close(acceptfd);
                client_agent(childfd);
                close(childfd);
                exit(EXIT_SUCCESS);

            } else {
                close(childfd);
            }
        }
    }
    cleanup_shared_memory();
}

void remove_client_resources(int client_id) {
    pthread_mutex_lock(&shm->mutex);
    for (int i=0; i<MAX_SUPPLY; i++) {
        if(shm->supplies[i].client_id == client_id) {
            memset(&shm->supplies[i], 0, sizeof(supply));
            shm->supplies[i].client_id = -1;
        }
    }
    for (int i=0; i<MAX_DEMAND; i++) {
        if(shm->demands[i].client_id == client_id) {
            memset(&shm->demands[i], 0, sizeof(demand));
            shm->demands[i].client_id = -1;
        }
    }
    for (int i=0; i<MAX_WATCH; i++) {
        if(shm->watches[i].client_id == client_id) {
            shm->watches[i].client_id = -1;
            shm->watches[i].watch_id = 0;
        }
    }
    shm->clients[client_id].client_socket = -1;
    shm->clients[client_id].client_id = -1;
    pthread_mutex_unlock(&shm->mutex);
}

void register_client(int *client_id, int client_socket){
    pthread_mutex_lock(&shm->mutex);

    for(int i = 0; i < MAX_CLIENTS; i++){
        if(shm->clients[i].client_socket == -1){
            *client_id = i;
            shm->clients[i].client_socket = client_socket;
            shm->clients[i].client_id = *client_id;
            shm->clients[i].x = 0;
            shm->clients[i].y = 0;
            shm->clients[i].notif_head = 0;
            shm->clients[i].notif_tail = 0;
            break;
        }
    }
    pthread_mutex_unlock(&shm->mutex);
}

void client_agent(int sockfd){

    pthread_t command_thread, notification_thread;

    int client_id;
    register_client(&client_id, sockfd);

    thread_arg *arg = (thread_arg *)malloc(sizeof(thread_arg));
    arg->sockfd = sockfd;
    arg->client_id = client_id;

    if(pthread_create(&command_thread, NULL, command_thread_func, arg) != 0){
        perror("pthread_create");
        free(arg);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    if(pthread_create(&notification_thread, NULL, notification_thread_func, arg) != 0){
        perror("pthread_create");
        pthread_cancel(command_thread);
        pthread_join(command_thread, NULL);
        free(arg);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    pthread_join(command_thread, NULL);
    pthread_cancel(notification_thread); 
    pthread_join(notification_thread, NULL);

    remove_client_resources(client_id);

    free(arg);
    close(sockfd);
}

void enqueue_notification(int client_id, const char *msg) {
    pthread_mutex_lock(&shm->clients[client_id].mutex);
    int next_head = (shm->clients[client_id].notif_head + 1) % MAX_NOTIFICATIONS;
    if (next_head == shm->clients[client_id].notif_tail) {
        // queue full, drop
    } else {
        strncpy(shm->clients[client_id].notifications[shm->clients[client_id].notif_head].message, msg, 255);
        shm->clients[client_id].notifications[shm->clients[client_id].notif_head].message[255] = '\0';
        shm->clients[client_id].notif_head = next_head;
        pthread_cond_signal(&shm->clients[client_id].condition);
    }
    pthread_mutex_unlock(&shm->clients[client_id].mutex);
}

void *command_thread_func(void *arg){
    thread_arg *targ = (thread_arg *)arg;
    int client_socket = targ->sockfd;
    int client_id = targ->client_id;

    char buffer[1024];
    size_t buffer_len = 0;
    while(1){
        ssize_t bytes_read = read(client_socket, buffer + buffer_len, sizeof(buffer) - buffer_len - 1);
        if(bytes_read <= 0){
    
            // Treat as quit
            return NULL;
        }
        buffer_len += bytes_read;
        buffer[buffer_len] = '\0';

        char *line_start = buffer;
        char *newline_pos;

        while ((newline_pos = strchr(line_start, '\n')) != NULL) {
            *newline_pos = '\0';
    

            char *command = line_start;
            int x,y,a,b,c,distance,watch_id;

            pthread_mutex_lock(&shm->mutex);
            if (sscanf(command, "move %d %d", &x, &y) == 2) {
                move_client(client_id, x, y);
                write(client_socket, "OK\n", 3);
            }
            else if (sscanf(command, "demand %d %d %d", &a, &b, &c) == 3) {
                add_new_demand(client_id, a, b, c);
                write(client_socket, "OK\n", 3);
                check_for_match(client_id);
            }
            else if (sscanf(command, "supply %d %d %d %d", &distance, &a, &b, &c) == 4) {
                int new_supply_index = -1;
                new_supply_index = add_new_supply(client_id, distance, a, b, c);
                write(client_socket, "OK\n", 3);
                check_for_match(client_id);

                if (new_supply_index != -1) {
                    check_for_watch_events_on_new_supply(new_supply_index);
                }
            }
            else if (sscanf(command, "watch %d", &watch_id) == 1) {
                add_new_watch(client_id, watch_id);
                write(client_socket, "OK\n", 3);
            }
            else if (strncmp(command, "unwatch", 7) == 0) {
                remove_watch(client_id);
                write(client_socket, "OK\n", 3);
            }
            else if (strncmp(command, "listsupplies", 12) == 0) {
                list_supplies(client_id);
            }
            else if (strncmp(command, "listdemands", 11) == 0) {
                list_demands(client_id);
            }
            else if (strncmp(command, "mysupplies", 10) == 0) {
                my_supplies(client_id);
            }
            else if (strncmp(command, "mydemands", 9) == 0) {
                my_demands(client_id);
            }
            else if (strncmp(command, "quit", 4) == 0) {
                write(client_socket, "OK\n", 3);
                pthread_mutex_unlock(&shm->mutex);
                return NULL;
            }
            else {
                write(client_socket, "Error: Invalid command\n", 24);
            }
            pthread_mutex_unlock(&shm->mutex);

            line_start = newline_pos + 1;
        }
        buffer_len = strlen(line_start);
        memmove(buffer, line_start, buffer_len);
        buffer[buffer_len] = '\0';
    }
    return NULL;
}

void *notification_thread_func(void *args){
    thread_arg *targ = (thread_arg *) args;
    int client_id = targ->client_id;

    while(1){

        pthread_mutex_lock(&shm->clients[client_id].mutex);
        while (shm->clients[client_id].notif_head == shm->clients[client_id].notif_tail) {
            pthread_cond_wait(&shm->clients[client_id].condition, &shm->clients[client_id].mutex);
        }

        while (shm->clients[client_id].notif_tail != shm->clients[client_id].notif_head) {
            char msg[256];
            strncpy(msg, shm->clients[client_id].notifications[shm->clients[client_id].notif_tail].message, 255);
            msg[255] = '\0';
            shm->clients[client_id].notif_tail = (shm->clients[client_id].notif_tail + 1) % MAX_NOTIFICATIONS;

            pthread_mutex_unlock(&shm->clients[client_id].mutex);
            notify_client(shm->clients[client_id].client_socket, msg);
            pthread_mutex_lock(&shm->clients[client_id].mutex);
        }

        pthread_mutex_unlock(&shm->clients[client_id].mutex);
    }
    return NULL;
}

void add_new_demand(int client_id, int a, int b, int c){
    for(int i = 0; i < MAX_DEMAND; i++){
        if(shm->demands[i].client_id == -1){
            shm->demands[i].x = shm->clients[client_id].x;
            shm->demands[i].y = shm->clients[client_id].y;
            shm->demands[i].client_id = client_id;
            shm->demands[i].a_amount = a;
            shm->demands[i].b_amount = b;
            shm->demands[i].c_amount = c;
            break;
        }
    }
}

int add_new_supply(int client_id, int distance, int a, int b, int c){
    for(int i = 0; i < MAX_SUPPLY; i++){
        if(shm->supplies[i].client_id == -1){
            shm->supplies[i].client_id = client_id;
            shm->supplies[i].x = shm->clients[client_id].x;
            shm->supplies[i].y = shm->clients[client_id].y;
            shm->supplies[i].a_amount = a;
            shm->supplies[i].b_amount = b;
            shm->supplies[i].c_amount = c;
            shm->supplies[i].distance = distance;
            return i;
        }
    }
    return -1;
}

void add_new_watch(int client_id, int new_watch_id){
    for(int i = 0; i < MAX_WATCH; i++){
        if(shm->watches[i].client_id == client_id){
            shm->watches[i].client_id = -1;
            shm->watches[i].watch_id = 0;
        }
    }
    // Add new watch
    for(int i = 0; i < MAX_WATCH; i++){
        if(shm->watches[i].client_id == -1){
            shm->watches[i].client_id = client_id;
            shm->watches[i].x = shm->clients[client_id].x;
            shm->watches[i].y = shm->clients[client_id].y;
            shm->watches[i].watch_id = new_watch_id;
            break;
        }
    }
}

void remove_watch(int client_id){
    for(int i = 0; i < MAX_WATCH; i++){
        if(shm->watches[i].client_id == client_id){
            shm->watches[i].client_id = -1;
            shm->watches[i].watch_id = 0;
        }
    }
}

int check_for_match(int client_id){
    for (int j=0; j<MAX_DEMAND; j++){
        if(shm->demands[j].client_id != -1) {
            for (int i=0; i<MAX_SUPPLY; i++){
                if (shm->supplies[i].client_id != -1 && check_case_match(j, i)) {
                    match_demand_and_supply(j, i);
                }
            }
        }
    }
    return 0;
}

void check_for_watch_events_on_new_supply(int supply_index) {
    // Only check watchers for this newly inserted supply
    if(shm->supplies[supply_index].client_id == -1) return; // invalid supply
    supply *s = &shm->supplies[supply_index];
    for (int i=0; i<MAX_WATCH; i++){
        if (shm->watches[i].client_id != -1 && shm->watches[i].watch_id > 0) {
            int distance = manhattan_distance(shm->watches[i].x, shm->watches[i].y, s->x, s->y);
            if (distance <= shm->watches[i].watch_id) {
                int watch_client = shm->watches[i].client_id;
                char msg[256];
                snprintf(msg, sizeof(msg), "A supply [%d,%d,%d] is inserted at (%d,%d).\n",
                         s->a_amount, s->b_amount, s->c_amount, s->x, s->y);
                enqueue_notification(watch_client, msg);
            }
        }
    }
}

int check_case_match(int demand_id, int supply_id){
    supply *s = &shm->supplies[supply_id];
    demand *d = &shm->demands[demand_id];

    if (d->client_id == -1 || s->client_id == -1) return 0;

    int distance = manhattan_distance(d->x, d->y, s->x, s->y);

    if(distance < s->distance && s->a_amount >= d->a_amount && s->b_amount >= d->b_amount && s->c_amount >= d->c_amount){
        return 1;
    }
    return 0;
}

void match_demand_and_supply(int demand_id, int supply_id) {
    supply *s = &shm->supplies[supply_id];
    demand *d = &shm->demands[demand_id];

    // Demand notification
    if (d->client_id != -1) {
        char demand_message[256];
        snprintf(demand_message, sizeof(demand_message),
                 "Your demand at (%d,%d), [%d,%d,%d] is fulfilled by a supply at (%d,%d).\n",
                 d->x, d->y, d->a_amount, d->b_amount, d->c_amount, s->x, s->y);
        enqueue_notification(d->client_id, demand_message);
    }

    // Supply notification
    if (s->client_id != -1) {
        char supply_message[256];
        snprintf(supply_message, sizeof(supply_message),
                 "Your supply at (%d,%d), [%d,%d,%d] with distance %d is delivered to a demand at (%d,%d) [%d,%d,%d].\n",
                 s->x, s->y, s->a_amount, s->b_amount, s->c_amount, s->distance,
                 d->x, d->y, d->a_amount, d->b_amount, d->c_amount);
        enqueue_notification(s->client_id, supply_message);
    }

    // Deduct from supply
    s->a_amount -= d->a_amount;
    s->b_amount -= d->b_amount;
    s->c_amount -= d->c_amount;

    // Remove demand
    remove_demand(demand_id);

    // If supply exhausted
    if (s->a_amount == 0 && s->b_amount == 0 && s->c_amount == 0) {
        if (s->client_id != -1) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Your supply is removed from map.\n");
            enqueue_notification(s->client_id, msg);
        }
        remove_supply(supply_id);
    }
}

void notify_client(int client_socket, const char *message) {
    if (client_socket == -1) {

        return;
    }

    ssize_t bytes_sent = send(client_socket, message, strlen(message), 0);
    if (bytes_sent < 0) {
        perror("send");
    } else {

    }
}

void remove_demand(int demand_id) {
    memset(&shm->demands[demand_id], 0, sizeof(demand));
    shm->demands[demand_id].client_id = -1;
}

void remove_supply(int supply_id) {
    memset(&shm->supplies[supply_id], 0, sizeof(supply));
    shm->supplies[supply_id].client_id = -1;
}

void list_supplies(int client_id) {

    int count = 0;
    for (int i = 0; i < MAX_SUPPLY; i++) {
        if (shm->supplies[i].client_id != -1) {
            count++;
        }
    }

    char header[512];
    snprintf(header, sizeof(header),
             "There are %d supplies in total.\n"
             "X | Y | A | B | C | D |\n"
             "-------+-------+-----+-----+-----+-------+\n", count);
    write(shm->clients[client_id].client_socket, header, strlen(header));

    for (int i = 0; i < MAX_SUPPLY; i++) {
        if (shm->supplies[i].client_id != -1) {
            char line[128];
            snprintf(line, sizeof(line), "%7d|%7d|%5d|%5d|%5d|%7d|\n",
                     shm->supplies[i].x,
                     shm->supplies[i].y,
                     shm->supplies[i].a_amount,
                     shm->supplies[i].b_amount,
                     shm->supplies[i].c_amount,
                     shm->supplies[i].distance);
            write(shm->clients[client_id].client_socket, line, strlen(line));
        }
    }
}

void list_demands(int client_id) {

    int count = 0;
    for (int i = 0; i < MAX_DEMAND; i++) {
        if (shm->demands[i].client_id != -1) {
            count++;
        }
    }

    char header[512];
    snprintf(header, sizeof(header),
             "There are %d demands in total.\n"
             "X | Y | A | B | C |\n"
             "-------+-------+-----+-----+-----+\n", count);
    write(shm->clients[client_id].client_socket, header, strlen(header));

    for (int i = 0; i < MAX_DEMAND; i++) {
        if (shm->demands[i].client_id != -1) {
            char line[128];
            snprintf(line, sizeof(line), "%7d|%7d|%5d|%5d|%5d|\n",
                     shm->demands[i].x,
                     shm->demands[i].y,
                     shm->demands[i].a_amount,
                     shm->demands[i].b_amount,
                     shm->demands[i].c_amount);
            write(shm->clients[client_id].client_socket, line, strlen(line));
        }
    }
}

void my_supplies(int client_id) {

    int count = 0;
    for (int i = 0; i < MAX_SUPPLY; i++) {
        if (shm->supplies[i].client_id == client_id) {
            count++;
        }
    }

    char header[512];
    snprintf(header, sizeof(header),
             "There are %d supplies in total.\n"
             "X | Y | A | B | C | D |\n"
             "-------+-------+-----+-----+-----+-------+\n", count);
    write(shm->clients[client_id].client_socket, header, strlen(header));

    for (int i = 0; i < MAX_SUPPLY; i++) {
        if (shm->supplies[i].client_id == client_id) {
            char line[128];
            snprintf(line, sizeof(line), "%7d|%7d|%5d|%5d|%5d|%7d|\n",
                     shm->supplies[i].x,
                     shm->supplies[i].y,
                     shm->supplies[i].a_amount,
                     shm->supplies[i].b_amount,
                     shm->supplies[i].c_amount,
                     shm->supplies[i].distance);
            write(shm->clients[client_id].client_socket, line, strlen(line));
        }
    }
}

void my_demands(int client_id) {

    int count = 0;
    for (int i = 0; i < MAX_DEMAND; i++) {
        if (shm->demands[i].client_id == client_id) {
            count++;
        }
    }

    char header[512];
    snprintf(header, sizeof(header),
             "There are %d demands in total.\n"
             "X | Y | A | B | C |\n"
             "-------+-------+-----+-----+-----+\n", count);
    write(shm->clients[client_id].client_socket, header, strlen(header));

    for (int i = 0; i < MAX_DEMAND; i++) {
        if (shm->demands[i].client_id == client_id) {
            char line[128];
            snprintf(line, sizeof(line), "%7d|%7d|%5d|%5d|%5d|\n",
                     shm->demands[i].x,
                     shm->demands[i].y,
                     shm->demands[i].a_amount,
                     shm->demands[i].b_amount,
                     shm->demands[i].c_amount);
            write(shm->clients[client_id].client_socket, line, strlen(line));
        }
    }
}

void move_client(int client_id, int x, int y) {
    shm->clients[client_id].x = x;
    shm->clients[client_id].y = y;
}

int manhattan_distance(int x1, int y1, int x2, int y2) {
    return abs(x1 - x2) + abs(y1 - y2);
}

void cleanup_shared_memory() {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        pthread_mutex_destroy(&shm->clients[i].mutex);
        pthread_cond_destroy(&shm->clients[i].condition);
    }
    pthread_mutex_destroy(&shm->mutex);
    munmap(shm, sizeof(shared_mem));
}
