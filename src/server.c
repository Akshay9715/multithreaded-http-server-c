#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>

#define PORT 8080
#define MAX_QUEUE 100
#define THREAD_COUNT 4

// ---------------- QUEUE ----------------
int queue[MAX_QUEUE];
int front = 0, rear = 0;

pthread_mutex_t queue_lock;
pthread_cond_t cond;

// ---------------- LOGGING ----------------
FILE *log_file;
pthread_mutex_t log_lock;

void log_message(const char *message) {
    pthread_mutex_lock(&log_lock);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);

    fprintf(log_file, "[%s] %s\n", time_str, message);
    fflush(log_file);

    pthread_mutex_unlock(&log_lock);
}

// ---------------- QUEUE OPS ----------------
int is_queue_full() {
    return (rear - front) >= MAX_QUEUE;
}

int is_queue_empty() {
    return front == rear;
}

void enqueue(int client_fd) {
    queue[rear % MAX_QUEUE] = client_fd;
    rear++;
}

int dequeue() {
    return queue[front++ % MAX_QUEUE];
}

// ---------------- CONTENT TYPE ----------------
const char* get_content_type(const char *path) {
    if (strstr(path, ".html")) return "text/html";
    if (strstr(path, ".jpg")) return "image/jpeg";
    if (strstr(path, ".png")) return "image/png";
    if (strstr(path, ".css")) return "text/css";
    if (strstr(path, ".js")) return "application/javascript";
    return "text/plain";
}

// ---------------- SAFE SEND ----------------
int send_all(int sock, const void *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(sock, (char*)buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

// ---------------- WORKER ----------------
void *worker(void *arg) {
    while (1) {
        pthread_mutex_lock(&queue_lock);

        while (is_queue_empty()) {
            pthread_cond_wait(&cond, &queue_lock);
        }

        int client_fd = dequeue();

        pthread_mutex_unlock(&queue_lock);

        char buffer[2048] = {0};
        int bytes = recv(client_fd, buffer, sizeof(buffer)-1, 0);

        if (bytes <= 0) {
            log_message("recv failed or client disconnected");
            close(client_fd);
            continue;
        }

        // Parse request safely
        char method[10], path[256];
        sscanf(buffer, "%9s %255s", method, path);

        char log_buf[512];
        snprintf(log_buf, sizeof(log_buf), "Request: %s %s", method, path);
        log_message(log_buf);

        // Prevent directory traversal
        if (strstr(path, "..")) {
            char *response =
                "HTTP/1.1 403 Forbidden\r\n"
                "Content-Type: text/plain\r\n\r\n"
                "Forbidden";

            send_all(client_fd, response, strlen(response));
            close(client_fd);
            continue;
        }

        char file_path[256];
        if (strcmp(path, "/") == 0)
            snprintf(file_path, sizeof(file_path), "../public/index.html");
        else
            snprintf(file_path, sizeof(file_path), "../public/%s", path + 1);

        FILE *file = fopen(file_path, "rb");


        if (!file) {
            log_message("File not found");

            char *response =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain\r\n\r\n"
                "File Not Found";

            send_all(client_fd, response, strlen(response));
            close(client_fd);
            continue;
        }

        // Get file size
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        rewind(file);

        const char *content_type = get_content_type(file_path);

        // Send header
        char header[512];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %ld\r\n\r\n",
                 content_type, file_size);

        send_all(client_fd, header, strlen(header));

        // Send file
        char file_buffer[1024];
        int read_bytes;

        while ((read_bytes = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0) {
            if (send_all(client_fd, file_buffer, read_bytes) < 0) break;
        }

        fclose(file);
        close(client_fd);

        snprintf(log_buf, sizeof(log_buf), "Served: %s", file_path);
        log_message(log_buf);
    }
    return NULL;
}

// ---------------- MAIN ----------------
int main() {
    int server_fd;
    struct sockaddr_in address;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        exit(1);
    }

    printf("Server running on port %d...\n", PORT);

    pthread_mutex_init(&queue_lock, NULL);
    pthread_cond_init(&cond, NULL);
    pthread_mutex_init(&log_lock, NULL);

    log_file = fopen("../server.log", "a");
    if (!log_file) {
        perror("log file");
        exit(1);
    }

    // Thread pool
    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_create(&threads[i], NULL, worker, NULL);
        pthread_detach(threads[i]);
    }

    // Accept loop
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf), "Client connected: fd=%d", client_fd);
        log_message(log_buf);

        pthread_mutex_lock(&queue_lock);

        if (is_queue_full()) {
            pthread_mutex_unlock(&queue_lock);

            char *response =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: text/plain\r\n\r\n"
                "Server Busy";

            send_all(client_fd, response, strlen(response));
            close(client_fd);

            log_message("Dropped request (queue full)");
        } else {
            enqueue(client_fd);
            pthread_cond_signal(&cond);
            pthread_mutex_unlock(&queue_lock);
        }
    }

    return 0;
}