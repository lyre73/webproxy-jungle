/* simple HTTP proxy that caches web objects */
/* 
 * First part: set up the proxy Done!
 *   - Accept incoming connections
 *   - read and parse requests
 *   - forward requests to web servers,
 *   - read the servers’ responses
 *   - forward those responses to the corresponding clients
 * Second part: upgrade your proxy Done!
 *   - deal with multiple concurrent connections
 * Third part:
 *   - add caching using a simple main memory cache of recently accessed web content
 */
#include "csapp.h"
#include <stdio.h>

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000 // = 1MiB = 1.049 megabytes
#define MAX_OBJECT_SIZE 102400 // = 1KiB = 102.4 kilobytes

// Cache structure: Simple memory inefficient cache
#define MAX_OBJECT_NUMS 10 // b/c if all caches has MAX_OBJECT_SIZE, only 10 can be. I know it's inefficient...
#define LRU_PRIORITY 9999  // default priority num for newly inserted cache block

typedef struct Cacheblock {
  // metadata
  char data[MAX_OBJECT_SIZE], url[MAXLINE];
  _Bool is_empty;         // used boolean(only 1 byte)
  int delete_priority;    // lower num -> higher priority to delete
} Cacheblock;

typedef struct Cachelist {
  Cacheblock caches[MAX_OBJECT_NUMS];

  // for synchronization
  int readcnt;            // num of currently accessing readers
  sem_t resource;         // controls access, binary semaphore(semaphore used to protect shared vars)
  sem_t rmutex;           // sync changes(readcount) (mutex: for mutual exclusion)
  sem_t service_queue;    // ordering of requests(FIFO)
} Cachelist;

// a global variable for the cache list
Cachelist cachelist;

// header lines, formats, keys for making new header for end server
/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";
char *request_hdr_format = "GET %s HTTP/1.0\r\n";
char *host_hdr_format = "Host: %s\r\n";
char *connection_hdr = "Connection: close\r\n";
char *proxy_connection_hdr = "Proxy-Connection: close\r\n";
char *end_of_hdr = "\r\n";

char *host_key = "Host";
char *user_agent_key = "User-Agent";
char *connection_key = "Connection";
char *proxy_connection_key = "Proxy-Connection";

// prototypes
void *serve_proxy(int *fd);
void make_hdrs(rio_t *rp, char* resulthdrs, char *hostname, char *uri);
void parse_url(char *url, char *uri, char *hostname, char *port);

void init_cachelist(void);
int search_cache(char *url);
int search_empty(void);
void insert_cache(char *buf, char *url);
void update_priority(int index);
int find_LRU();
int delete_cache();
void reader_entry();
void reader_exit();
void writer_entry();
void writer_exit();

// Iterative server, port is passed in the command line
int main(int argc, char **argv) { // port is passed in command line. (argc, argv: for command line. argc is argument count, argv is argument vector)
  int listenfd, *connfdp;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr; // Enough space for any address
  pthread_t tid;

  /* Check command line args */
  if (argc != 2) { // main() needs only 1 argument(port). Check argc and if there's less or more argument(s), exit
    fprintf(stderr, "usage: %s <port>\n", argv[0]); // "usage: proxy <port>"
    exit(1);
  }

  // prepare(initialize) cachelist for caching
  init_cachelist();

  // Open listening socket
  listenfd = Open_listenfd(argv[1]); // Open_listnfd(): return listeing descriptor. (argument: port( number))

  // Infinite server loop, waiting for connection request
  while (1) {
    clientlen = sizeof(clientaddr); // = sizeof(struct sockaddr_storage);
    connfdp = Malloc(sizeof(int));
    *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen); // Accept(): wait for connection request to arrive to listenfd, fill in clientaddr(+length), and return connection descriptor

    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0); // Getnameinfo(): from socket address structure, to host and service name strings
    printf("Accepted connection from (%s, %s)\n", hostname, port); // prints domain name and port

    // create new thread
    Pthread_create(&tid, NULL, serve_proxy, connfdp);
  }
}

// serve_proxy - handle HTTP request/response transaction
void *serve_proxy(int *fdp) 
{
  int index, endfd, connfd = *fdp;
  char buf[MAXLINE], method[MAXLINE], url[MAXLINE], end_hostname[MAXLINE], uri[MAXLINE], version[MAXLINE], requesthdrs[MAXLINE], end_port[10];
  char cachebuf[MAX_OBJECT_SIZE];
  rio_t client_rio, end_rio;

  // Immediately detach the thread
  Pthread_detach(pthread_self());
  // Free connfdp(only used to send connfd to serve_proxy())
  Free(fdp);

  // Read and parse the client's request header: method, end_hostname, end_port, uri(file path), version
  Rio_readinitb(&client_rio, connfd); // Rio_readinitb(): associates descriptor with read buffer(type rio_t) (receive buffer's address)
  if (!Rio_readlineb(&client_rio, buf, MAXLINE)) // Rio_readlineb(): (wrapper function) copy text line from read buffer, refill it whenever it becomes empty
    return; // if no line to read(met EOF), return
  printf("%s", buf); // print the text line that just read to buf
  // parse the text line into method, url, version data
  sscanf(buf, "%s %s %s", method, url, version);
  // parse url into end_hostname, end_port, uri(file path)
  parse_url(url, uri, end_hostname, end_port);
  sprintf(url, "%s:%s%s", end_hostname, end_port, uri); // unify url format for cache metadata

  // printf("\nfinding for %s in cache.\n", url);
  if ((index = search_cache(url)) != -1) {
    printf("found in cache!\n\n");

    // read and ignore remaining headers
    strcpy(buf, "");
    while(strcmp(buf, "\r\n")) { // when buf == "\r\n", loop ends. ("\r\n" terminates request headers)
      Rio_readlineb(&client_rio, buf, MAXLINE);
    }
    // give requested response in cache
    Rio_writen(connfd, cachelist.caches[index].data,
               strlen(cachelist.caches[index].data));
  }
  else {
    printf("not found in cache\n\n");
    // Read the given header and make new header to send
    make_hdrs(&client_rio, requesthdrs, end_hostname, uri); // read headers and make new HTTP request headers

    // Open connection to end server(end_hostname:end_port)
    endfd = Open_clientfd(end_hostname, end_port);
    Rio_readinitb(&end_rio, endfd);
    // Send HTTP request
    Rio_writen(endfd, requesthdrs, strlen(requesthdrs));

    // Get result from end server
    strcpy(buf, ""); // empty buffer

    // empty cache buffer
    strcpy(cachebuf, "");

    // read and send the data given by chunks from end server
    ssize_t n;
    while ((n = Rio_readnb(&end_rio, buf, MAXLINE)) > 0) {
      Rio_writen(connfd, buf, n);
      // 캐시버퍼에 버퍼 내용 더해주는데, 만약 크기가 maxobj를 넘으면 캐시버퍼는 안쓸거임
      if (strlen(cachebuf) <= MAX_OBJECT_SIZE) {
        strcat(cachebuf, buf);
      }
      fwrite(buf, 1, n, stdout); // Use fwrite to correctly handle binary data in stdout
    }
    printf("\n");
    
    // 캐시버퍼가 유효하면 리스트에 넣는다
    if (strlen(cachebuf) <= MAX_OBJECT_SIZE) {
      insert_cache(cachebuf, url);
    }

    Close(endfd); // closes open client descriptor(proxy->end server)
  }

  Close(connfd); // closes open connection descriptor(proxy->client)
  return NULL;
}

// make_hdrs - read client's HTTP request headers and make new HTTP request header for end server
void make_hdrs(rio_t *rp, char *resulthdrs, char *hostname, char *uri)
{
  char buf[MAXLINE], request_hdr[MAXLINE], host_hdr[MAXLINE], other_hdr[MAXLINE];

  // make request_hdr and host_hdr lines(have certain form)
  sprintf(request_hdr, request_hdr_format, uri);
  sprintf(host_hdr, host_hdr_format, hostname);
  // initialize other_hdr(empty string)
  strcpy(other_hdr, "");

  // start reading and making header lines
  Rio_readlineb(rp, buf, MAXLINE);
  while(strcmp(buf, "\r\n")) { // when buf == "\r\n", loop ends. ("\r\n" terminates request headers)
    Rio_readlineb(rp, buf, MAXLINE);
    if (strcmp(buf, "\r\n") != 0) {
      if (!strncmp(buf, host_key, strlen(host_key))) { // if there's original Host header, use it
        strcpy(host_hdr, buf);
      } else if (strncmp(buf, user_agent_key, strlen(user_agent_key)) && // the lines premade(no need to save client's)
                 strncmp(buf, connection_key, strlen(connection_key)) &&
                 strncmp(buf, proxy_connection_key, strlen(proxy_connection_key)))
      { // other headers should be appended not changed
        strcat(other_hdr, buf);
      }
    }
  }

  // combine all header lines
  strcpy(resulthdrs, "");
  sprintf(resulthdrs, "%s%s%s%s%s%s%s",
          request_hdr,
          host_hdr,
          user_agent_hdr,
          connection_hdr,
          proxy_connection_hdr,
          other_hdr,
          end_of_hdr);

  // print the header
  printf("Request headers:\n%s", resulthdrs);
  return;
}

// parse url string into uri, (end server)hostname and its port number(string)
void parse_url(char *url, char *uri, char *hostname, char *port)
{
  char *ptr;

  // initialize port number for if client have not specified
  strcpy(port, "8001");

  // if url contains "http://" or not, the server should keep going on
  if (!strncmp(url, "http://", 7)) {
    ptr = url + 7;
    strcpy(url, ptr);
  }

  // parsing "hostname:port/url"
  // "hostname:port", url
  ptr = index(url, '/');
  if (ptr != NULL) { // if have not gave /uri
    *ptr = '\0';
    strcpy(uri, "/");
    strcat(uri, ptr + 1);
  } else {
    strcpy(uri, "/");
  }
  // hostname, port, url
  ptr = index(url, ':');
  if (ptr != NULL) {
    *ptr = '\0';
    strcpy(port, ptr + 1);
  }
  strcpy(hostname, url);
  
  return;
}

// initialize cache list(all cache blocks)
// no need to check for synchronization: only called once before any peer threads
void init_cachelist(void)
{
  // initialize cache blocks
  Cacheblock *current;
  for (int index = 0; index < MAX_OBJECT_NUMS; index++) {
    // to readability
    current = &cachelist.caches[index];

    // initialize metadata
    strcpy(current->data, "");
    strcpy(current->url, "");
    current->is_empty = 1; // true
    current->delete_priority = 0;
  }

  // used for synchronization(list, not block)
  cachelist.readcnt = 0; // no one reading now
  // initialize semaphores for resource/readcount/ordering to 1
  sem_init(&cachelist.resource, 0, 1);
  sem_init(&cachelist.rmutex, 0, 1);
  sem_init(&cachelist.service_queue, 0, 1);
}

// search through cache list and return the index of the correct block. 0~9 if exists, -1 if not
int search_cache(char *url)
{
  int index = -1;
  Cacheblock *current;

  reader_entry();

  for (int i = 0; i < MAX_OBJECT_NUMS; i++) {
    current = &cachelist.caches[i];
    if (current->is_empty == 0) {
      if (strcmp(current->url, url) == 0) { // found same url metadata
        index = i;
        break;
      }
    }
  }

  reader_exit();
  return index;
}

// search through cache list and return the index of the empty block. 0~9 if exists, -1 if not
int search_empty(void)
{
  int index = -1;
  Cacheblock *current;

  for (int i = 0; i < MAX_OBJECT_NUMS; i++) {
    current = &cachelist.caches[i];
    
    if (current->is_empty == 1) {
      index = i;
      break;
    }
  }
  return index;
}

// insert data to cache
// 빈 자리 있으면 리스트에 그냥 넣음, 빈 자리 없으면 제일 쓸모없는 거 먼저 지우고 넣음
void insert_cache(char *buf, char *url)
{
  Cacheblock *current;
  int index;

  writer_entry();

  if ((index = search_empty()) == -1) {
    delete_cache();
    index = search_empty();
  }

  current = &cachelist.caches[index];
  strcpy(current->data, buf);
  current->delete_priority = LRU_PRIORITY; // default num for newly inserted cache block
  current->is_empty = 0;
  strcpy(current->url, url);

  // and other blocks' priority numbers!
  update_priority(index);

  writer_exit();

  return;
}

// raise delete priority(lower priority number) except current index
void update_priority(int index)
{
  Cacheblock *current;
  for (int i = 0; i < MAX_OBJECT_NUMS; i++) {
    current = &cachelist.caches[i];
    if (current->is_empty == 0) {
      if (i != index) {
        current->delete_priority--;
      }
    }
  }
  return;
}

// find least recently used data in cache
int find_LRU()
{
  int target_index = -1; // 안 그렇겠지만 혹시 리스트가 비어 있으면
  int min = LRU_PRIORITY;
  Cacheblock *current;
  for (int i = 0; i < MAX_OBJECT_NUMS; i++) {
    current = &cachelist.caches[i];

    if (current->is_empty == 0) {
      if (current->delete_priority < min) {
        target_index = i;
        min = current->delete_priority;
      }
    }
  }
  return target_index;
}

// delete least recently used data in cache
int delete_cache()
{
  int index = find_LRU();
  Cacheblock *current = &cachelist.caches[index];

  if (index == -1) { // find_LRU failed
    return -1;
  }

  // initialize metadata
  strcpy(current->data, "");
  strcpy(current->url, "");
  current->is_empty = 1; // true
  current->delete_priority = 0;

  return 0;
}

// ENTRY section for readers, block cache for writers
void reader_entry()
{
  sem_wait(&cachelist.service_queue); // wait in line to be serviced
  sem_wait(&cachelist.rmutex);        // request exclusive access to readcount
  cachelist.readcnt++;                // update count of active readers
  if (cachelist.readcnt == 1) {       // if I am the first reader(only 1 readcnt)
    sem_wait(&cachelist.resource);    // request resource access for readers(writers blocked)
  }
  sem_post(&cachelist.service_queue); // let next in line be serviced
  sem_post(&cachelist.rmutex);        // release access to readcount
}

// EXIT section for readers, unblock cache
void reader_exit()
{
  sem_wait(&cachelist.rmutex);        // request exclusive access to readcount
  cachelist.readcnt--;                // update count of active readers
  if (cachelist.readcnt == 0) {       // if there are no readers left
    sem_post(&cachelist.resource);    // release resource access for all
  }
  sem_post(&cachelist.rmutex); // release access to readcount
}

// ENTRY section for writers, block cache
void writer_entry()
{
  sem_wait(&cachelist.service_queue); // wait in line to be serviced
  sem_wait(&cachelist.resource);      // request exclusive access to resource
  sem_post(&cachelist.service_queue); // let next in line be serviced
}

// EXIT section for writer, unblock cache
void writer_exit()
{
  sem_post(&cachelist.resource);      // release resource access for next reader/writer
}