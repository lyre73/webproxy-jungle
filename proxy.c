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
  // data and metadata

  char data[MAX_OBJECT_SIZE], url[MAXLINE];
  _Bool is_empty;         // used boolean(only 1 byte) to check, if empty: 1, else: 0
  int LRU_priority;       // lower num -> higher priority to delete

  // for synchronization

  int readcnt;            // num of currently accessing readers
  sem_t wrtmutex;         // if 0(there is a writer), blocks any access to the cache. binary semaphore
  sem_t rdcntmutex;       // when updating rdcnt, blocks access to rdcnt. (sync readercnt between threads)
  sem_t service_queue;    // (not strictly) ordering of requests(FIFO) -> no thread would starve. don't prefer reader nor writer
} Cacheblock;

typedef struct Cachelist {
  Cacheblock caches[MAX_OBJECT_NUMS];
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

// proxy server function prototypes

void *serve_proxy(int *fd);
void make_hdrs(rio_t *rp, char* resulthdrs, char *hostname, char *uri);
void parse_url(char *url, char *uri, char *hostname, char *port);

// cache function prototypes

void init_cachelist(void);
int search_cache(char *url);
int search_empty(void);
void insert_cache(char *buf, char *url);
void update_priority(int index);
int find_insert_index(void);
int delete_cache(void);

// synchronization function prototypes

void reader_entry(int index);
void reader_exit(int index);
void writer_entry(int index);
void writer_exit(int index);

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

  // let kernel IGNore broken PIPE SIGnal(premature socket ends)
  Signal(SIGPIPE, SIG_IGN);

  // prepare(initialize) cachelist for caching
  init_cachelist();

  // Open listening socket
  listenfd = Open_listenfd(argv[1]); // Open_listnfd(): return listeing descriptor. (argument: port( number))

  // Infinite server loop, waiting for connection request
  while (1) {
    clientlen = sizeof(clientaddr); // = sizeof(struct sockaddr_storage);
    connfdp = Malloc(sizeof(int));  // to send arguments to thread function for Pthread_create()
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
  ssize_t n;
  int is_valid = 1;

  // Immediately detach the thread
  Pthread_detach(pthread_self());
  // Free connfdp(only used to send connfd to serve_proxy()).
  // if there's many arguments to use, this malloc way is better than making structure for arguments
  Free(fdp);

  // Read and parse the client's request header: method, end_hostname, end_port, uri(file path), version
  Rio_readinitb(&client_rio, connfd); // Rio_readinitb(): associates descriptor with read buffer(type rio_t) (receive buffer's address)
  if (!Rio_readlineb(&client_rio, buf, MAXLINE)) // Rio_readlineb(): (wrapper function) copy text line from read buffer, refill it whenever it becomes empty
    return; // if no line left to read(met EOF), return
  printf("%s", buf); // print the text line that just read to buf
  sscanf(buf, "%s %s %s", method, url, version); // parse the text line into method, url, version data
  parse_url(url, uri, end_hostname, end_port); // parse url into end_hostname, end_port, uri(file path)

  // for url format consistency(to be used for cache metadata)
  sprintf(url, "%s:%s%s", end_hostname, end_port, uri);

  // if method is not GET, return error msg and end connection with client
  if (strcmp(method, "GET")) {
    strcpy(buf,
           "501 Not Implemented\tProxy does not implement this method.\n");
    Rio_writen(connfd, buf, strlen(buf));

    // don't forget to close open connection descriptor(proxy->client)
    Close(connfd);

    // terminal print for me
    printf("501 Not Implemented\tProxy does not implement this method.\n\n");
    return NULL;
  }

  if ((index = search_cache(url)) != -1) { // Cache hit: no need to connect to the end server
    printf("----Cache hit----\n\n");

    reader_entry(index);

    // read and ignore remaining headers
    strcpy(buf, "");
    while(strcmp(buf, "\r\n")) { // when buf == "\r\n", loop ends. ("\r\n" terminates request headers)
      Rio_readlineb(&client_rio, buf, MAXLINE);
    }
    
    // send requested data in cache to client
    Rio_writen(connfd, cachelist.caches[index].data, strlen(cachelist.caches[index].data));

    reader_exit(index);

  } else { // Cache miss: connect to the end server and get response (and remember to close the file descriptor!)
    printf("----Cache miss----\n\n");

    // Read the given header and make new header to send
    make_hdrs(&client_rio, requesthdrs, end_hostname, uri); // read headers and make new HTTP request headers

    // Open connection to end server(end_hostname:end_port)
    endfd = Open_clientfd(end_hostname, end_port);
    Rio_readinitb(&end_rio, endfd);

    // Send HTTP request
    Rio_writen(endfd, requesthdrs, strlen(requesthdrs));

    // Get response from end server
    // empty buffer and cache buffer to use
    strcpy(buf, "");
    strcpy(cachebuf, "");

    // read and send the data given by chunks from end server
    while ((n = Rio_readnb(&end_rio, buf, MAXLINE)) > 0) {
      Rio_writen(connfd, buf, n);
      // concatnate buffer to cachebuffer, but if the size exceeds MAX_OBJECT_SIZE, we won't use it(stop )
      if (strlen(cachebuf) + strlen(buf) <= MAX_OBJECT_SIZE && is_valid) { // PREVENT BUFFER OVERRUN!
        strcat(cachebuf, buf);
      } else { // cachebuffer is not valid(over maximum size), update flag
        is_valid = 0;
      }
      fwrite(buf, 1, n, stdout); // Use fwrite to correctly handle binary data in stdout
    }
    printf("\n"); // for readability of terminal output(for programmer(me), not client)

    // closes open client descriptor(proxy->end server)
    Close(endfd);

    // if cache buffer is valid, insert it to the cache list
    if (is_valid) {
      insert_cache(cachebuf, url);
    }
  }

  // closes open connection descriptor(proxy->client)
  Close(connfd);

  return NULL;
}

// make_hdrs - read client's HTTP request headers and make new HTTP request header for end server
void make_hdrs(rio_t *rp, char *resulthdrs, char *hostname, char *uri)
{
  char buf[MAXLINE], request_hdr[MAXLINE], host_hdr[MAXLINE], other_hdr[MAXLINE];

  // make request_hdr and host_hdr lines(have certain form)
  sprintf(request_hdr, request_hdr_format, uri);
  sprintf(host_hdr, host_hdr_format, hostname);

  // initialize(empty) other_hdr line
  strcpy(other_hdr, "");

  // start reading and making header lines
  strcpy(buf, "");
  while(strcmp(buf, "\r\n")) { // when buf == "\r\n", loop ends. ("\r\n" terminates request headers)
    Rio_readlineb(rp, buf, MAXLINE);
    if (strcmp(buf, "\r\n") != 0) {
      if (!strncmp(buf, host_key, strlen(host_key))) { // if there's original Host header, use it
        strcpy(host_hdr, buf);
      } else if (strncmp(buf, user_agent_key, strlen(user_agent_key)) && // the lines premade(no need to save client's)
                 strncmp(buf, connection_key, strlen(connection_key)) &&
                 strncmp(buf, proxy_connection_key, strlen(proxy_connection_key)))
      { // other headers should be appended, without being changed
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

  // print the header(for me, not client)
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
  // -> "hostname:port", url
  ptr = index(url, '/');
  if (ptr != NULL) { // if have not gave /uri
    *ptr = '\0';
    strcpy(uri, "/");
    strcat(uri, ptr + 1);
  } else {
    strcpy(uri, "/");
  }
  // -> hostname, port, url
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
    current = &cachelist.caches[index]; // for readability

    // initialize metadata
    strcpy(current->data, "");
    strcpy(current->url, "");
    current->is_empty = 1; // true
    current->LRU_priority = 0;

    // used for synchronization(list, not block)
    current->readcnt = 0; // no one reading now
    // initialize semaphores for resource/readcount/ordering to 1
    sem_init(&current->wrtmutex, 0, 1);
    sem_init(&current->rdcntmutex, 0, 1);
    sem_init(&current->service_queue, 0, 1);
  }
}

// search through cache list and return the index of the correct block. 0~9 if exists, -1 if not
int search_cache(char *url)
{
  Cacheblock *current;

  // search through cache list
  for (int index = 0; index < MAX_OBJECT_NUMS; index++) {
    current = &cachelist.caches[index]; // for readability
    reader_entry(index);
    if (current->is_empty == 0 && strcmp(current->url, url) == 0) { // cache hit! return index
      reader_exit(index); // don't forget
      return index;
    }
    reader_exit(index);
  }
  return -1; // indicates cache miss
}

// Not needed: just overwrite other cache!
// // search through cache list and return the index of the empty block. 0~9 if exists, -1 if not
// int search_empty(void)
// {
//   int index = -1;
//   Cacheblock *current;

//   for (int i = 0; i < MAX_OBJECT_NUMS; i++) {
//     current = &cachelist.caches[i];
    
//     if (current->is_empty == 1) {
//       index = i;
//       break;
//     }
//   }
//   return index;
// }

// insert data to cache
void insert_cache(char *buf, char *url)
{
  int index = find_insert_index();
  Cacheblock *current = &cachelist.caches[index];

  writer_entry(index);

  // fill the cache in(only data and metadata)
  strcpy(current->data, buf);
  current->LRU_priority = LRU_PRIORITY; // default num for newly inserted cache block
  current->is_empty = 0;
  strcpy(current->url, url);

  // and lower other blocks' priority numbers(increase delete priority)!
  update_priority(index); // inside writer_entry and exit, and has own writer_entry and exit. but the block doesn't overlap so it's okay

  writer_exit(index);

  return;
}

// raise delete priority(lower priority number) except current index block
// the writing blocks do not overlap with it's caller function
void update_priority(int index)
{
  Cacheblock *current;

  // search through cache list 
  for (int i = 0; i < MAX_OBJECT_NUMS; i++) {
    current = &cachelist.caches[i]; // for readability
    if (i != index) { // skip given index
      writer_entry(i);
      if (current->is_empty == 0) {
        current->LRU_priority--;
      }
      writer_exit(i);
    }
  }
  return;
}

// find empty or least recently used data block to overwrite
int find_insert_index()
{
  int target = 0; // if the list is empty(or have other errors, anyway), just remove(overwrite) cache[0]
  int min = LRU_PRIORITY;
  Cacheblock *current;

  for (int index = 0; index < MAX_OBJECT_NUMS; index++) {
    current = &cachelist.caches[index]; // for readability
    reader_entry(index);
    if (current->is_empty == 1) { // if there was empty block, just use it
      reader_exit(index); // don't forget!
      return index;
    }
    if (current->LRU_priority < min) { // finding for highest priority block
      target = index;
      min = current->LRU_priority;
    }
    reader_exit(index);
  }
  return target;
}

// Not needed: just overwrite other cache!
// // delete least recently used data in cache
// int delete_cache()
// {
//   int index = find_insert_index();
//   Cacheblock *current = &cachelist.caches[index];

//   if (index == -1) { // find_insert_index failed
//     return -1;
//   }

//   // initialize metadata
//   strcpy(current->data, "");
//   strcpy(current->url, "");
//   current->is_empty = 1; // true
//   current->LRU_priority = 0;

//   return 0;
// }

// ENTRY section for readers, block cache for writers
void reader_entry(int index)
{
  Cacheblock *current = &cachelist.caches[index]; // for readability
  P(&current->service_queue);   // wait in line to be serviced
  P(&current->rdcntmutex);      // request exclusive access to readcount
  current->readcnt++;           // update count of active readers
  if (current->readcnt == 1) {  // if I am the first reader(only 1 readcnt)
    P(&current->wrtmutex);      // writers blocked(someone is reading!)
  }
  V(&current->service_queue);   // let next in line be serviced
  V(&current->rdcntmutex);      // release access to readcount
}

// EXIT section for readers, unblock cache
void reader_exit(int index)
{
  Cacheblock *current = &cachelist.caches[index]; // for readability
  P(&current->rdcntmutex);      // request exclusive access to readcount
  current->readcnt--;           // update count of active readers
  if (current->readcnt == 0) {  // if there are no readers left
    V(&current->wrtmutex);      // release(unblock) access for all
  }
  V(&current->rdcntmutex);      // release access to readcount
}

// ENTRY section for writers, block cache
void writer_entry(int index)
{
  Cacheblock *current = &cachelist.caches[index]; // for readability
  P(&current->service_queue);   // wait in line to be serviced
  P(&current->wrtmutex);        // request exclusive access(block) to the cache
  V(&current->service_queue);   // let next in line be serviced
}

// EXIT section for writer, unblock cache
void writer_exit(int index)
{
  Cacheblock *current = &cachelist.caches[index]; // just one variable, so not needed, but for consistency with other functions
  V(&current->wrtmutex);        
  // unblock access for next reader/writer
}