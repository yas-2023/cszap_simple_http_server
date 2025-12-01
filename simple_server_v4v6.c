#include <stdio.h> //標準入力 perrorなど
#include <stdlib.h> //動的メモリ確保 mallocなど
#include <string.h> //文字列操作
#include <unistd.h> //システムコール
#include <arpa/inet.h> //ネットワーク通信 IPアドレス変換など
#include <sys/socket.h> //ソケット通信, socket, bindなど
#include <ctype.h> // 数字判定用

#define DEFAULT_PORT 8080
#define BUFFER_SIZE 1024
#define CALC_QUERY_PREFIX "GET /calc?query="
#define CALC_QUERY_PREFIX_LENGTH (sizeof(CALC_QUERY_PREFIX) - 1)
#define RESPONSE_BODY_BUFFER_SIZE 16


void exit_with_error(const char *message) {
    perror(message); // perror: 直前に発生したエラーメッセージを表示
    exit(EXIT_FAILURE); // EXIT_FAILURE: stdlib.h で定義されている異常終了（失敗）を示す終了ステータス
}

// 数式の文字列を受け取って計算する関数（左から順に計算）
int calculate_expression(const char *query_expression_start) {
    // 文字列を読み進めるためのポインタ
    const char *current_position = query_expression_start;
    char *parse_end_position;
    
    // 最初の数字を読み取る (strtolは数字を読み、読み終わった位置をparse_end_positionに入れる)
    int current_total = strtol(current_position, &parse_end_position, 10);
    current_position = parse_end_position;

    // 文字列の終わりまでループ
    while (*current_position != '\0' && *current_position != ' ') { // 空白か終端まで
        // 演算子を取得 (+, -, *, /)
        char operator = *current_position;
        current_position++; // ポインタを演算子の次へ進める

        // 次の数字がない場合は終了
        if (!isdigit(*current_position)) break;

        // 次の数字を読み取る
        int next_number = strtol(current_position, &parse_end_position, 10);
        current_position = parse_end_position;

        // 計算を実行
        switch (operator) {
            case '+': current_total += next_number; break;
            case '-': current_total -= next_number; break;
            case '*': current_total *= next_number; break;
            case '/': 
                if (next_number != 0) {
                    current_total /= next_number;
                } else {
                    return 0; // ゼロ除算エラー（簡易的に0を返す）
                }
                break;
            default: break; // 無視
        }
    }
    return current_total;
}

void handle_client(int client_socket) {
    // リクエスト受信用メモリ確保
    char *request_buffer = (char *)malloc(BUFFER_SIZE);
    if (request_buffer == NULL) {
        close(client_socket);
        return;
    }
    memset(request_buffer, 0, BUFFER_SIZE);

    // 受信
    ssize_t received_bytes = recv(client_socket, request_buffer, BUFFER_SIZE - 1, 0);
    if (received_bytes < 0) {
        free(request_buffer);
        close(client_socket);
        return;
    }

    printf("--- Client Request ---\n%s\n", request_buffer);

    int calculation_result = 0;
    int is_valid_request = 0;

    // "GET /calc?query=" を探す
    char *query_start = strstr(request_buffer, CALC_QUERY_PREFIX); 
    if (query_start != NULL) {
        // "query=" の後ろ（計算式の先頭）へのポインタを取得
        char *query_expression_start = query_start + CALC_QUERY_PREFIX_LENGTH;
        
        // ここから数式として計算する
        calculation_result = calculate_expression(query_expression_start);
        is_valid_request = 1;
    }

    // レスポンス用メモリ確保
    char *response_buffer = (char *)malloc(BUFFER_SIZE);
    if (response_buffer == NULL) {
        free(request_buffer);
        close(client_socket);
        return;
    }

    if (is_valid_request) {
        char body_content[RESPONSE_BODY_BUFFER_SIZE];
        int body_length = sprintf(body_content, "%d", calculation_result);

        sprintf(response_buffer,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %d\r\n"
                "\r\n"
                "%s",
                body_length, body_content);
    } else {
        const char *not_found_msg = "Not Found or Invalid Query";
        sprintf(response_buffer,
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: %ld\r\n"
                "\r\n"
                "%s",
                strlen(not_found_msg), not_found_msg);
    }

    // 送信
    send(client_socket, response_buffer, strlen(response_buffer), MSG_NOSIGNAL);

    // 解放と終了
    free(request_buffer);
    free(response_buffer);
    close(client_socket);
}

int main(int argc, char *argv[]) {
    int server_socket;
    int client_socket;
    struct sockaddr_in6 server_address_structure;
    
    struct sockaddr_storage client_address;
    socklen_t client_address_length = sizeof(client_address);
    int reuse_on = 1;
    int ipv6_v6only_off = 0;

    // デフォルトポートを設定
    int port_number = DEFAULT_PORT;

    // 引数がある場合はポート番号を上書き
    if (argc > 1) {
        port_number = atoi(argv[1]);
    }

    // 1. ソケットを作る
    // AF_INET6 を直接指定
    server_socket = socket(AF_INET6, SOCK_STREAM, 0);

    // 2. アドレス構造体を「手書き」で埋める
    memset(&server_address_structure, 0, sizeof(server_address_structure));
    server_address_structure.sin6_family = AF_INET6;
    server_address_structure.sin6_addr = in6addr_any; 
    server_address_structure.sin6_port = htons(port_number);

    // 3. オプション設定
    // ポート再利用
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_on, sizeof(reuse_on));
    
    // デュアルスタック設定 (IPv6ソケットでIPv4も扱う)
    if (setsockopt(server_socket, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_v6only_off, sizeof(ipv6_v6only_off)) < 0) {
        perror("Note: Could not set IPV6_V6ONLY=0 (Dual stack might not work on some OS)");
    }

    // 4. バインド
    if (bind(server_socket, (struct sockaddr *)&server_address_structure, sizeof(server_address_structure)) < 0) {
        exit_with_error("bind failed");
    }

    if (listen(server_socket, 3) < 0) {
        exit_with_error("listen failed");
    }

    printf("Server is running on port %d (supports +, -, *, /)...\n", port_number);

    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *)&client_address, &client_address_length);
        if (client_socket < 0) {
            perror("accept failed");
            continue;
        }
        handle_client(client_socket);
    }

    close(server_socket);
    return 0;
}
