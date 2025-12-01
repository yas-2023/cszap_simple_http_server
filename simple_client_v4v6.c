#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024

void exit_with_error(const char *message) {
    perror(message);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    // 引数の指定方法
    if (argc != 4) {
        printf("Usage: %s <IP_ADDRESS> <PORT> <QUERY>\n", argv[0]);
        printf("Example: %s 127.0.0.1 8080 \"10+20\"\n", argv[0]);
        printf("Example: %s ::1 8080 \"10+20\"\n", argv[0]);
        return 1;
    }

    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    char *query_string = argv[3];

    int network_socket;
    // IPv4とIPv6両方のアドレス構造体を保持できる汎用的な構造体
    struct sockaddr_storage server_address; 
    socklen_t addr_len;

    // メモリをクリア
    memset(&server_address, 0, sizeof(server_address));

    // --- 自動判別ロジック ---
    
    // 1. まず IPv4 かどうか試す
    struct sockaddr_in *addr4 = (struct sockaddr_in *)&server_address;
    if (inet_pton(AF_INET, server_ip, &addr4->sin_addr) == 1) {
        // IPv4 である
        printf("[Info] Detected IPv4 address.\n");
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(server_port);
        addr_len = sizeof(struct sockaddr_in);
        
        // IPv4ソケット作成
        network_socket = socket(AF_INET, SOCK_STREAM, 0);
    } 
    // 2. 駄目なら IPv6 かどうか試す
    else {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&server_address;
        if (inet_pton(AF_INET6, server_ip, &addr6->sin6_addr) == 1) {
            // IPv6 である
            printf("[Info] Detected IPv6 address.\n");
            addr6->sin6_family = AF_INET6;
            addr6->sin6_port = htons(server_port);
            addr_len = sizeof(struct sockaddr_in6);
            
            // IPv6ソケット作成
            network_socket = socket(AF_INET6, SOCK_STREAM, 0);
        } else {
            // どちらでもない（無効なIP、またはホスト名）
            fprintf(stderr, "Error: Invalid IP address format (IPv4 or IPv6 required).\n");
            return 1;
        }
    }

    if (network_socket < 0)
        exit_with_error("socket failed");

    // --- 接続 ---
    
    // connectは共通で (struct sockaddr *) にキャストして渡す
    if (connect(network_socket, (struct sockaddr *)&server_address, addr_len) < 0) {
        close(network_socket);
        exit_with_error("Connection failed");
    }

    // --- リクエスト送信処理 ---

    char *request_message = (char *)malloc(BUFFER_SIZE);
    if (request_message == NULL)
        exit_with_error("Memory allocation failed");

    const char *query = query_string;
    printf("Sending query: %s\n", query);
    
    sprintf(request_message, 
            "GET /calc?query=%s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "\r\n", query, server_ip);

    if (send(network_socket, request_message, strlen(request_message), 0) < 0) {
        free(request_message);
        close(network_socket);
        exit_with_error("Send failed");
    }
    free(request_message);

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
