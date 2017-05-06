#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <unistd.h>

#include "cqueue.h"
#include "http.h"

#define STREAM_URI    "stream.mjpeg"
#define SNAPSHOT_URI  "snapshot.jpeg"
#define BUFF_MAX      1024
#define READ_TIMEOUT  30

extern int stop;

struct http_header {
  char *method;
  char *uri;
};

static void http_header_free(struct http_header *header);

static int print_picture(int fd, unsigned char *buf, int size)
{
  int jpg_hdr = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];

  if(jpg_hdr != 0xFFD8FFE0 && jpg_hdr != 0xFFD8FFC0) {
      printf("%s: invalid JPEG header 0x%X\n", __func__, jpg_hdr);
  }

  if( write(fd, buf, size) <= 0) return -1;
  return 0;
}

static int data_available(int socket, int timeout)
{
  struct timeval to;
  fd_set fds;
  to.tv_sec = timeout, to.tv_usec = 0;
  FD_ZERO(&fds);
  FD_SET(socket, &fds);
  return select(socket+1, &fds, NULL, NULL, &to);
}

static int http_header_readline(int fd, char *buf, int len)
{
  char *ptr = buf;
  char *ptr_end = ptr + len - 1;

printf("before enter data_available... ...\n");
  if(data_available(fd, READ_TIMEOUT) <= 0) {
    return -1;
  }

printf("after enter data_available... ...\n");
  while (ptr < ptr_end) {
    switch (read(fd, ptr, 1)) {
    case 1:
	if(*ptr == '\n'){
		*ptr = '\0';
		return ptr - buf;
	}else{
       		ptr++;
		continue;
	}
       break;
    case 0:
      *ptr = '\0';
      return ptr - buf;
    default:
      printf("%s() failed: %s\n", __func__, strerror(errno));
      return -1;
    }
  }

printf("return after enter data_available... ...\n");
  return len;
}

static int http_parse_header(struct clientArgs *client, struct http_header *header)
{
  char header_line[BUFF_MAX];
  char *token = NULL;
  int res;

  header->method= NULL;
  header->uri = NULL;

  client->request_type = UNKNOWN;

printf("before enter http_header_readline ... ...\n");
  /* fcntl(socketfd, F_SETFL, fcntl(socketfd, F_GETFL, 0) | O_NONBLOCK);*/
  if((res = http_header_readline( client->socket, header_line, sizeof(header_line))) > 0) {
      token = strtok(header_line, "/");
      if(token) {
        header->method = strdup(token);
      }
      token = strtok(NULL, "/");
      if(token) {
        header->uri = strdup(token);
      }
  }

printf("after enter http_header_readline ... ...\n");
  if (header->uri) {
    if (!strcmp(header->uri, SNAPSHOT_URI)) {
      client->request_type = SNAPSHOT;
    }
    if (!strcmp(header->uri, STREAM_URI)) {
      client->request_type = STREAM;
    }
  } else {
    client->request_type= INVALID;
  }
  return res;
}

static void http_header_free(struct http_header *header)
{
  if(header->method){
    free(header->method);
  }

  if(header->uri){
    free(header->uri);
  }
}
/* thread for clients that connected to this server */
static void *http_client_thread( void *arg )
{
  struct clientArgs *ca = (struct clientArgs *)arg;
  struct thread_buff *tbuff = ca->server->ptbuff;
  int ok = 1, should_close_connection = 0;
  char buffer[BUFF_MAX] = {0};
  struct buff *b = NULL;
  struct http_header header;

  pthread_detach(pthread_self());

printf("before enter http_parse_header ... ...\n");
  if(http_parse_header(ca, &header) < 0){
    close(ca->socket);
    http_header_free(&header);
    free(arg);
    return NULL;
  }

printf("after enter http_parse_header ... ...\n");
  printf("thread_id: %ld request %s\n", pthread_self(), header.uri);

  switch (ca->request_type) {
    case SNAPSHOT:
      snprintf(buffer, sizeof(buffer),"snapshot");
    break;
    case STREAM:
      snprintf(buffer, sizeof(buffer),"stream");
    break;
    default:
      snprintf(buffer, sizeof(buffer),"The requested URL %s was not found on this server\n", header.uri);
      should_close_connection = 1;
    break;
  }

  printf("%s\n",buffer);

  if (should_close_connection) {
    close(ca->socket);
    http_header_free(&header);
    free(arg);
    return NULL;
  }

  /* mjpeg server push */
  while (ok >= 0 && !stop) {

    pthread_mutex_lock(&(tbuff)->lock);
    pthread_cond_wait(&(tbuff)->cond, &(tbuff)->lock);

    b = queue_front(&(tbuff)->qbuff);

    if (ca->request_type == STREAM) {
      snprintf(buffer,sizeof(buffer),"Content-Length:%d\r\n",b->size);
      if(write(ca->socket, buffer, strlen(buffer)) < 0) {
        pthread_mutex_unlock( &(tbuff)->lock );
        break;
      }
    }

    ok = print_picture(ca->socket, b->buff, b->size);
    pthread_mutex_unlock( &(tbuff)->lock );

    if(ca->request_type != STREAM) {
      break;
    }
  }

  close(ca->socket);
  http_header_free(&header);
  free(arg);
  return NULL;
}


int http_listener(struct http_server *srv)
{
  struct sockaddr_in addr;
  int on=1;
  int c = sizeof(struct sockaddr_in);
  struct clientArgs *ca;
  /* open socket for server */
  srv->sd = socket(PF_INET, SOCK_STREAM, 0);
  if ( srv->sd < 0 ) {
    fprintf(stderr, "socket failed\n");
    exit(1);
  }

  /* ignore "socket already in use" errors */
  if (setsockopt(srv->sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    perror("setsockopt(SO_REUSEADDR) failed");
    exit(1);
  }

  /* configure server address to listen to all local IPs */
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = srv->port;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if ( bind(srv->sd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ) {
    perror("Bind");
    exit(1);
  }

  /* start listening on socket */
  if (listen(srv->sd, 10) != 0 ) {
    fprintf(stderr, "listen failed\n");
    exit(1);
  }

  srv->client_thread = http_client_thread;
  while( 1 ) {
    /* alloc new client */
    ca = malloc(sizeof(struct clientArgs));
    ca->server = srv;
    ca->socket = accept(srv->sd, (struct sockaddr *)&ca->client_addr, (socklen_t*)&c);
    if (ca->socket < 0) {
      perror("accept failed");
      free(ca);
      continue;
    }

printf("i'm coming ... ... \n");
    if( pthread_create(&srv->client, NULL,  srv->client_thread, ca) < 0) {
      perror("could not create client thread");
      return 1;
    }
  }
}
