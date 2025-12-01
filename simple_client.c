#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define SERVER_IP "127.0.0.1"

void exit_with_error(const char *message) {
    perror(message);
    exit(EXIT_FAILURE);
}

int main() {
    int network_socket;
    struct sockaddr_in server_address;

    network_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (network_socket < 0) exit_with_error("socket failed");

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    // 文字列を IPv4用バイナリ形式に変換して server_address.sin_addr に格納
    if (inet_pton(AF_INET, SERVER_IP, &server_address.sin_addr) <= 0) {
        exit_with_error("Invalid address");
    }

    // 接続要求
    if (connect(network_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        exit_with_error("Connection failed");
    }

    // 1. リクエストメッセージの作成
    char *request_message = (char *)malloc(BUFFER_SIZE);
    if (request_message == NULL) exit_with_error("Memory allocation failed");

    // 計算式を指定
    // 例: 100 + 50 * 2 - 10 
    // (左から計算するので 150 * 2 = 300, 300 - 10 = 290 になる)
    const char *query = "100+50*2-10";
    
    printf("Sending query: %s\n", query);
    
    sprintf(request_message, 
            "GET /calc?query=%s HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n", query);

    // 2. 送信
    if (send(network_socket, request_message, strlen(request_message), 0) < 0) {
        free(request_message);
        close(network_socket);
        exit_with_error("Send failed");
    }
    free(request_message);

    // 3. 受信
    char *response_buffer = (char *)malloc(BUFFER_SIZE);
    if (response_buffer == NULL) {
        close(network_socket);
        exit_with_error("Memory allocation failed");
    }
    memset(response_buffer, 0, BUFFER_SIZE);

    if (recv(network_socket, response_buffer, BUFFER_SIZE - 1, 0) < 0) {
        perror("recv failed");
    } else {
        printf("--- Server Response ---\n%s\n", response_buffer);
    }

    free(response_buffer);
    close(network_socket);

    return 0;
}
