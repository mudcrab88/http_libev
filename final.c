#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUFFER_SIZE 65536
#define METHOD_GET "GET"
#define STATUS_OK "HTTP/1.0 200 OK\r\n"
#define STATUS_NOT_FOUND "HTTP/1.0 404 NOT FOUND\r\n"
#define CONTENT_TYPE "Content-Type: text/html;\r\n"
#define CONTENT_LENGTH_0 "Content-length: 0\r\n"
#define HEADER_CACHE "Cache-Control: no-cache\r\n"
#define HEADER_CONNECTION "Connection: close\r\n"

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
        return NULL;
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
    bzero(buf, size);
    // читаем полностью весь файл в буфер  
    fread(buf, 1, size, file);
    fclose(file);

    return buf;
}

//получение ответа от сервера
char * get_response(int status, char * content) {
    char * response = NULL;
    if (status == 404) {
        response = (char *)malloc(sizeof(char) * 100);
        strcat(response, STATUS_NOT_FOUND);
        strcat(response, CONTENT_LENGTH_0);
        strcat(response, CONTENT_TYPE);
    } 

    if (status == 200){
        response = (char*)malloc(sizeof(char) * strlen(content) + 100);        
        bzero(response, strlen(content) + 100);
        strcat(response, STATUS_OK);
        strcat(response, HEADER_CONNECTION);
        strcat(response, CONTENT_TYPE);
        strcat(response, "\r\n");
        strcat(response, content);
    }    

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
}

void parse_http_request(const char * request, struct http_header * header) {
    char query[512];
    sscanf(request, "%s %s", header->type, query);
    split_query_string(query, header);
}

void send_response(int client_d, struct http_header * header) {
    char * content = get_content(header->path); 
    char * response;

    if (content == NULL) {
        response = get_response(404, content);
    } else {
        response = get_response(200, content);
    }
    printf("%s %s %s\n", header->type, header->path, header->params);

    int len = strlen(response);
    send(client_d, response, len, 0);
    
    free(content);
    free(response);
}

void handle_request(int client_d) {
    char request[BUFFER_SIZE];
    struct http_header header;
    int bytes_recvd = recv(client_d, request, BUFFER_SIZE - 1, 0);

    if (bytes_recvd < 0) {
        fprintf(stderr, "error recv\n");
        return;
    }
    request[bytes_recvd] = '\0';

    parse_http_request(request, &header);

    if (strcmp(header.type, METHOD_GET) == 0) {
        send_response(client_d, &header);
    }
}

void * thread_handle_request(void * arg) {
	int client_d = *((int *)arg);
    handle_request(client_d);
	return 0;
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

    printf("chroot=%d\n", chroot(dir));
    if (0 == daemon(0, 0)) {
        perror("daemon");
    }

    struct sockaddr_storage client_addr;
    int client_d;
    pthread_t thread;
    while (1) {
        //подключение клиента
        socklen_t s_size = sizeof(client_addr);
        client_d = accept(sd, (struct sockaddr *)&client_addr, &s_size);
        if(client_d == -1) {
            fprintf(stderr, "error accept\n");
            return -1;
        }
        pthread_create(&thread, 0, &thread_handle_request, (void *)(&client_d));
        pthread_join(thread, 0);
        //handle_request(client_d);
        close(client_d);
    }

    return 0;
}