#define PSK_BYTES 16

#include <stdlib.h>

int server_bind(const int port);

int server_create(char * const pskhex);

int server_accept();

int server_close() ;

int client_close(const int fd) ;

int client_update(const int fd, void * const buffer, const size_t size);

