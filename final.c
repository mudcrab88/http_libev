#include <arpa/inet.h>
#include <errno.h>
#include <ev.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUFFER_SIZE 65536
#define METHOD_GET "GET"
#define STATUS_OK "HTTP/1.1 200 OK\r\n"
#define STATUS_NOT_FOUND "HTTP/1.1 404 Not Found\r\n"
#define HEADER_CONTENT "Content-Type: text/html;\r\n"
#define HEADER_CACHE "Cache-Control: no-cache\r\n"
#define HEADER_CONNECTION "Connection: keep-alive\r\n"

struct http_header {
    char type[255];
    char path[255];
    char params[255];
};

void send_404(int sock) {
    const char *buffer = "HTTP/1.1 404 \n\n";
    int len = strlen(buffer);
    send(sock, buffer, len, 0);
}

//получение файла, если он есть
char * get_content(char * path) {
    char * filename;
    if (strcmp(path, "/") == 0) {
        filename = "index.html";
    } else {
        filename = &path[1];
    }

    FILE* file;
    // открываем файл
    if((file = fopen(filename, "rb")) == NULL){
        return NULL;
    }
    
    // узнаем размер файла для создания буфера нужного размера
    fseek(file, 0L, SEEK_END);
    long size = ftell(file); 
    fseek(file, 0L, SEEK_SET); 
    
    // выделяем память под буфер
    char* buf = (char*)malloc(sizeof(char) * size);
    // читаем полностью весь файл в буфер  
    fread(buf, 1, size, file);
    fclose(file);

    return buf;
}

//получение ответа от сервера
char * get_response(char * status, char * content) {
    char * response = (char*)malloc(sizeof(char) * strlen(content) + 100);
    strcat(response, status);
    strcat(response, HEADER_CONTENT);
    strcat(response, HEADER_CACHE);
    strcat(response, HEADER_CONNECTION);
    strcat(response, "\n");
    strcat(response, content);

    return response;
}

void split_query_string(const char * query, struct http_header * header) {
    size_t i = 0, j = 0, checked = 0;
    bzero(header->path, strlen(header->path));
    bzero(header->params, strlen(header->params));

    //ищем path(имя файла)
    for (i = 0; i < strlen(query); i++) {
        if (query[i] == '?') {
            header->path[i] = '\0';
            j = i;
            break;
        } else {
            header->path[i] = query[i];
        }
    }
    //ищем params(параметры запроса)
    for (i = 0; j < strlen(query); i++, j++) {
        if (j == 0) {
            header->params[i] = '\0';
            break;
        } else {
            header->params[i] = query[j];
        }
    }

    printf("%s %s %s\n", header->type, header->path, header->params);
}

void parse_http_request(const char * request, struct http_header * header) {
    char query[512];
    sscanf(request, "%s %s", header->type, query);
    split_query_string(query, header);
    //printf("%s\n", request);    
}

//коллбэк(чтение с клиента)
void read_cb(struct ev_loop * loop, struct ev_io * watcher, int revents) {
    char request[BUFFER_SIZE];
    ssize_t r = recv(watcher->fd, request, BUFFER_SIZE, MSG_NOSIGNAL);

    if (r < 0) {
        return;
    } else if (r == 0) {//если соединение закрыто, остановка и освобождение памяти
        printf("Connection closed %d\n", watcher->fd);
        ev_io_stop(loop, watcher);        
        free(watcher);
        return;
    } else {
        struct http_header header;
        parse_http_request(request, &header);
        
        //принимаем только GET-запросы, иначе 404
        char * response, * content;
        if (strcmp(header.type, METHOD_GET) == 0) {
            content = get_content(header.path);
            if (content != NULL) {
                response = get_response(STATUS_OK, content);
            } else {
                content = get_content("/404.html");
                response = get_response(STATUS_NOT_FOUND, content);
            }
        } else {
            content = get_content("/404.html");
            response = get_response(STATUS_NOT_FOUND, content);
        }
        send(watcher->fd, response, strlen(response), MSG_NOSIGNAL);
        free(content);
        free(response);
        ev_io_stop(loop, watcher);
        close(watcher->fd);        
        free(watcher);
    }
}

//коллбэк при подключении клиента
void accept_cb(struct ev_loop * loop, struct ev_io * watcher, int revents) {
    //подключение клиента
    int client_sd = accept(watcher->fd, 0, 0);
    printf("Client connected %d\n", client_sd);
    //watcher для клиента
    struct ev_io * w_client = (struct ev_io *)malloc(sizeof(struct ev_io));
    ev_io_init(w_client, read_cb, client_sd, EV_READ);
    ev_io_start(loop, w_client);
}

int main(int argc, char** argv) 
{
    int opt, port;
    char * host, * dir;
    while ((opt = getopt (argc, argv, "h:p:d:")) != -1) {
        switch (opt) {
            case 'h':
                host = optarg;
                printf ("%s\n", optarg);
                break;
            case 'p':
                port = atoi(optarg);
                printf ("%d\n", port);
                break;
            case 'd':                
                dir = optarg;
                printf ("%s\n", dir);
                break;
            default:
                fprintf (stderr, "Unknown error\n");
                return 1;
        }
    }    

    struct ev_loop * loop = ev_default_loop(0);

    //создание сокета и установка свойств
    int sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int yes;
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1){
        fprintf(stderr, "error setsockopt SO_REUSEADDR\n");
        close(sd);
        return 2;
    }
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(int)) == -1){
        fprintf(stderr, "error setsockopt SO_REUSEPORT\n");
        close(sd);
        return 2;
    }
    
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    bind(sd, (struct sockaddr *)&addr, sizeof(addr));
    listen(sd, SOMAXCONN);

    //создание watcher'a
    struct ev_io w_accept;
    ev_io_init(&w_accept, accept_cb, sd, EV_READ);
    ev_io_start(loop, &w_accept);

    printf("chroot=%d\n", chroot(dir));
    fprintf(stderr, "Error message : %s\n", strerror(errno));
    if (0 == daemon(0, 0)) {
        perror("daemon");
    } else {
        printf("daemon start!\n");
    }

    while (1) {
        ev_loop(loop, 0);
    }

    return 0;
}