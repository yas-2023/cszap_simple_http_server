#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h> // epoll用ヘッダ
#include <fcntl.h>     // ファイルコントロール (ノンブロッキング設定用)
#include <errno.h>     // エラー番号 (EAGAINなど)
#include <ctype.h>

#define DEFAULT_PORT 8080
#define BUFFER_SIZE 1024
#define CALC_QUERY_PREFIX "GET /calc?query="
#define CALC_QUERY_PREFIX_LENGTH (sizeof(CALC_QUERY_PREFIX) - 1)
#define RESPONSE_BODY_BUFFER_SIZE 16
#define MAX_EPOLL_EVENTS 64 // 一度に取得するイベントの最大数

void exit_with_error(const char *message) {
    perror(message);
    exit(EXIT_FAILURE);
}

// ソケットをノンブロッキングモードに変更する関数
int make_socket_non_blocking(int socket_file_descriptor) {
    // 現在のフラグ設定を取得
    int current_flags = fcntl(socket_file_descriptor, F_GETFL, 0);
    if (current_flags == -1) {
        return -1;
    }
    // ノンブロッキングフラグ (O_NONBLOCK) を追加して設定
    return fcntl(socket_file_descriptor, F_SETFL, current_flags | O_NONBLOCK);
}

// 数式計算ロジック
int calculate_expression(const char *query_expression_start) {
    const char *current_position = query_expression_start;
    char *parse_end_position;
    int current_total = strtol(current_position, &parse_end_position, 10);
    current_position = parse_end_position;

    while (*current_position != '\0' && *current_position != ' ') {
        char operator = *current_position;
        current_position++;
        if (!isdigit(*current_position)) break;
        int next_number = strtol(current_position, &parse_end_position, 10);
        current_position = parse_end_position;

        switch (operator) {
            case '+': current_total += next_number; break;
            case '-': current_total -= next_number; break;
            case '*': current_total *= next_number; break;
            case '/': 
                if (next_number != 0) current_total /= next_number;
                else return 0;
                break;
            default: break;
        }
    }
    return current_total;
}

// クライアントからのリクエストを処理する関数
// 戻り値: 1 = 通信終了（ソケットを閉じるべき）、0 = 継続または待機
int handle_client_request(int client_socket_file_descriptor) {
    char request_buffer[BUFFER_SIZE];
    memset(request_buffer, 0, BUFFER_SIZE);

    // ★追加: 受信試行ログ
    printf("  [RECV] Trying to read from Socket %d...\n", client_socket_file_descriptor);

    // データ受信 (ノンブロッキング)
    ssize_t received_bytes = recv(client_socket_file_descriptor, request_buffer, BUFFER_SIZE - 1, 0);

    if (received_bytes < 0) {
        // データがまだ届いていないだけの場合
        // エラーではないので、一旦処理を戻して待機する
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 待機ログ
            printf("  [WAIT] Data not ready yet (EAGAIN) on Socket %d. Returning to event loop.\n", client_socket_file_descriptor);
            return 0; 
        }
        perror("recv failed");
        return 1; // 本当のエラーなので閉じる
    } else if (received_bytes == 0) {
        // クライアントが接続を切断した
        //  切断ログ
        printf("  [CLOSE] Client (Socket %d) closed connection.\n", client_socket_file_descriptor);
        return 1; 
    }

    // 受信内容の表示ログ
    char debug_buffer[64];
    strncpy(debug_buffer, request_buffer, 60);
    debug_buffer[60] = '\0';
    for(int i=0; i<60; i++) if(debug_buffer[i]=='\r' || debug_buffer[i]=='\n') debug_buffer[i]=' ';
    printf("  [RECV] Received %zd bytes from Socket %d: \"%s...\"\n", received_bytes, client_socket_file_descriptor, debug_buffer);


    // 計算処理
    int calculation_result = 0;
    int is_valid_request = 0;

    char *query_start = strstr(request_buffer, CALC_QUERY_PREFIX);
    if (query_start != NULL) {
        char *query_expression_start = query_start + CALC_QUERY_PREFIX_LENGTH;
        calculation_result = calculate_expression(query_expression_start);
        is_valid_request = 1;
        // 計算結果ログ
        printf("  [CALC] Expression parsed. Result: %d\n", calculation_result);
    } else {
        printf("  [CALC] Invalid query format.\n");
    }

    // レスポンス作成
    char response_buffer[BUFFER_SIZE];
    if (is_valid_request) {
        char body_content[RESPONSE_BODY_BUFFER_SIZE];
        int body_length = sprintf(body_content, "%d", calculation_result);
        sprintf(response_buffer,
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s",
                body_length, body_content);
    } else {
        const char *not_found_msg = "Not Found or Invalid Query";
        sprintf(response_buffer,
                "HTTP/1.1 404 Not Found\r\nContent-Length: %ld\r\n\r\n%s",
                strlen(not_found_msg), not_found_msg);
    }

    // 送信
    // 注: 本来は send もノンブロッキング対応が必要ですが、今回は短いので簡略化
    send(client_socket_file_descriptor, response_buffer, strlen(response_buffer), MSG_NOSIGNAL);
    
    // 送信完了ログ
    printf("  [SEND] Sent response to Socket %d. Closing connection.\n", client_socket_file_descriptor);

    return 1; // 今回はHTTP/1.0的に1回応答したら閉じる仕様
}

int main(int argc, char *argv[]) {
    int server_socket_file_descriptor;
    int epoll_instance_file_descriptor;
    struct sockaddr_in6 server_address_structure;
    
    // epoll用イベント設定
    struct epoll_event event_configuration;
    struct epoll_event pending_events[MAX_EPOLL_EVENTS];

    int port_number = DEFAULT_PORT;
    if (argc > 1) {
        port_number = atoi(argv[1]);
    }

    // 1. サーバーソケット作成
    server_socket_file_descriptor = socket(AF_INET6, SOCK_STREAM, 0);
    if (server_socket_file_descriptor < 0) {
        exit_with_error("socket creation failed");
    }

    // サーバーソケットをノンブロッキングに設定
    make_socket_non_blocking(server_socket_file_descriptor);

    // オプション設定
    int reuse_address_option = 1;
    int ipv6_v6only_off_option = 0;
    setsockopt(server_socket_file_descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse_address_option, sizeof(reuse_address_option));
    setsockopt(server_socket_file_descriptor, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_v6only_off_option, sizeof(ipv6_v6only_off_option));

    // アドレス設定とバインド
    memset(&server_address_structure, 0, sizeof(server_address_structure));
    server_address_structure.sin6_family = AF_INET6;
    server_address_structure.sin6_addr = in6addr_any;
    server_address_structure.sin6_port = htons(port_number);

    if (bind(server_socket_file_descriptor, (struct sockaddr *)&server_address_structure, sizeof(server_address_structure)) < 0) {
        exit_with_error("bind failed");
    }

    if (listen(server_socket_file_descriptor, SOMAXCONN) < 0) {
        exit_with_error("listen failed");
    }

    // 2. epoll インスタンス（イベント監視用の箱）を作成
    epoll_instance_file_descriptor = epoll_create1(0);
    if (epoll_instance_file_descriptor < 0) {
        exit_with_error("epoll_create1 failed");
    }

    // 3. サーバーソケットを epoll の監視リストに追加
    event_configuration.data.fd = server_socket_file_descriptor;
    event_configuration.events = EPOLLIN | EPOLLET; // 読み込み可能（＝新規接続あり）を監視
    if (epoll_ctl(epoll_instance_file_descriptor, EPOLL_CTL_ADD, server_socket_file_descriptor, &event_configuration) < 0) {
        exit_with_error("epoll_ctl (adding server socket) failed");
    }

    printf("Non-blocking Server is running on port %d using epoll...\n", port_number);
    printf("Waiting for events (epoll_wait)...\n\n"); 

    // 4. イベントループ
    while (1) {
        // イベントが発生するのを待つ（ここで唯一ブロックする）
        int ready_event_count = epoll_wait(epoll_instance_file_descriptor, pending_events, MAX_EPOLL_EVENTS, -1);
        
        if (ready_event_count < 0) {
            perror("epoll_wait failed");
            break;
        }

        // 発生したイベントを順に処理
        for (int i = 0; i < ready_event_count; i++) {
            int active_file_descriptor = pending_events[i].data.fd;

            if (active_file_descriptor == server_socket_file_descriptor) {
                // --- A. サーバーソケットに反応があった場合（＝新規接続） ---
                printf("[EVENT] Connection request on Server Socket.\n"); 

                struct sockaddr_storage client_address_structure;
                socklen_t client_address_length = sizeof(client_address_structure);
                
                // ループして可能な限り受け入れる
                while (1) {
                    int client_socket_file_descriptor = accept(server_socket_file_descriptor, (struct sockaddr *)&client_address_structure, &client_address_length);
                    
                    if (client_socket_file_descriptor < 0) {
                        // もう受け入れる接続がない場合ループを抜ける
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            printf("  [INFO] All pending connections accepted (EAGAIN).\n");
                            break; 
                        }
                        perror("accept failed");
                        break;
                    }

                    // IP表示用ロジック
                    char client_ip[INET6_ADDRSTRLEN];
                    if (client_address_structure.ss_family == AF_INET) {
                        struct sockaddr_in *s = (struct sockaddr_in *)&client_address_structure;
                        inet_ntop(AF_INET, &s->sin_addr, client_ip, sizeof(client_ip));
                    } else {
                        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_address_structure;
                        inet_ntop(AF_INET6, &s->sin6_addr, client_ip, sizeof(client_ip));
                    }
                    printf("  [ACCEPT] New Client: Socket %d (IP: %s)\n", client_socket_file_descriptor, client_ip);


                    // クライアントソケットもノンブロッキングに設定
                    make_socket_non_blocking(client_socket_file_descriptor);

                    // クライアントを epoll の監視リストに追加
                    event_configuration.data.fd = client_socket_file_descriptor;
                    event_configuration.events = EPOLLIN | EPOLLET; 
                    if (epoll_ctl(epoll_instance_file_descriptor, EPOLL_CTL_ADD, client_socket_file_descriptor, &event_configuration) < 0) {
                        perror("epoll_ctl (adding client socket) failed");
                        close(client_socket_file_descriptor);
                    }
                }
            } else {
                // --- B. クライアントソケットに反応があった場合（＝データ受信） ---
                printf("[EVENT] Data arrived on Client Socket %d.\n", active_file_descriptor); 

                int should_close_connection = handle_client_request(active_file_descriptor);

                if (should_close_connection) {
                    // 通信終了ならソケットを閉じる
                    // closeするとepollの監視リストからも自動的に削除
                    close(active_file_descriptor);
                }
            }
        }
    }

    close(server_socket_file_descriptor);
    close(epoll_instance_file_descriptor);
    return 0;
}
