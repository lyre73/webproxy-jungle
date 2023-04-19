#include "csapp.h"

// for homework
#define HW7  // Extend Tiny to serve MP4
#define HW9  // Modify Tiny to use Malloc, Rio_readn, and Rio_writen(not mmap and Rio_writen) for 
// #define HW10 // Write HTML form for CGI adder. 2 text boxed, form request content using GET method
#define HW11 // Extend Tiny to support HTTP HEAD method. Check using TELNET

void *doit(int *fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void get_filetype(char *filename, char *filetype);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
#ifdef HW11
void serve_static(int fd, char *filename, int filesize, char *method);
void serve_dynamic(int fd, char *filename, char *cgiargs, char *method);
#else
void serve_static(int fd, char *filename, int filesize);
void serve_dynamic(int fd, char *filename, char *cgiargs);
#endif

// Iterative server, port is passed in the command line
int main(int argc, char **argv) { // port is passed in command line. (argc, argv: for command line. argc is argument count, argv is argument vector)
  int listenfd, *connfdp;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr; // Enough space for any address
  pthread_t tid;

  /* Check command line args */
  if (argc != 2) { // main() needs only 1 argument(port). Check argc and if there's less or more argument(s), exit
    fprintf(stderr, "usage: %s <port>\n", argv[0]); // "usage: tiny <port>"
    exit(1);
  }

  // Open listening socket
  listenfd = Open_listenfd(argv[1]); // Open_listnfd(): return listeing descriptor. (argument: port( number))

  // Infinite server loop, waiting for connection request
  while (1) {
    clientlen = sizeof(clientaddr);

    // perform a transaction
    // Accept(): wait for connection request to arrive to listenfd, fill in clientaddr(+length), and return connection descriptor
    connfdp = Malloc(sizeof(int));
    *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);   // line:netp:tiny:Accept

    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0); // Getnameinfo(): from socket address structure, to host and service name strings
    printf("Accepted connection from (%s, %s)\n", hostname, port); // prints domain name and port
    Pthread_create(&tid, NULL, doit, connfdp);
  }
}

/*
 * doit - handle one HTTP request/response transaction
 */
void *doit(int *fdp) 
{
  int is_static, connfd = *fdp;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio;

  // Immediately detach the thread
  Pthread_detach(pthread_self());
  // Free connfdp(only used to send connfd to serve_proxy())
  Free(fdp);

  /* Read request line and headers */
  // 1) Read and parse the request line
  Rio_readinitb(&rio, connfd); // Rio_readinitb(): associates descriptor with read buffer(type rio_t) (receive buffer's address)
  if (!Rio_readlineb(&rio, buf, MAXLINE)) // if no line to read(met EOF) // Rio_readlineb(): (wrapper function) copy text line from read buffer, refill it whenever it becomes empty
    return;
  printf("%s", buf); // print the text line that just read
  sscanf(buf, "%s %s %s", method, uri, version); // parse the text line into method, uri, version data

#ifdef HW11
  /* If client requests method other than GET and HEAD, send error message and return to main */
  if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD")) {
    clienterror(connfd, method, "501", "Not Implemented",
                "Tiny does not implement this method");
    return;
  }
#else
  /* If client requests another method, send error message and return to main */
  if (strcasecmp(method, "GET")) {
    clienterror(fd, method, "501", "Not Implemented",
                "Tiny does not implement this method");
    return;
  }
#endif

  /* Read and ignore header */
  read_requesthdrs(&rio); // read and print HTTP request headers

  /* 2) Parse URI from GET request */
  is_static = parse_uri(uri, filename, cgiargs);  // flag for static or dynamic content
  /* if the file does not exist, send error message */
  if (stat(filename, &sbuf) < 0) {
    clienterror(connfd, filename, "404", "Not found", "Tiny couldn't find this file");
    return;
  }

  if (is_static) { /* Serve static content */          
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) { // verify regular file and read permission
      clienterror(connfd, filename, "403", "Forbidden", "Tiny couldn't read the file");
      return;
    }
    serve_static(connfd, filename, sbuf.st_size, method); // serve static content
  }
  else { /* Serve dynamic content */
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) { // verify file executable
      clienterror(connfd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
      return;
    }
    serve_dynamic(connfd, filename, cgiargs, method); // serve dynamic content
  }
  printf("\n");
  Close(connfd);
}

/*
 * clienterror - returns error code and error message to the client
 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];

  /* Build the HTTP response body*/ // HTML content. build as a single string to dasily determine size
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcollor=""ffffff"">\r\n", body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  /* Print the HTTP response */
  // print HTTP response line
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf)); // Rio_writen(): transfers n bytes from buffer to descriptor
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));
  // print the HTTP response body(HTML content) made before at the last
  Rio_writen(fd, body, strlen(body));
}

/* 
 * read_requesthdrs - read HTTP request headers
 */
void read_requesthdrs(rio_t *rp)
{
  char buf[MAXLINE];

  Rio_readlineb(rp, buf, MAXLINE);
  printf("%s", buf);
  while(strcmp(buf, "\r\n")) { // when buf == "\r\n", loop ends. ("\r\n" terminates request headers)
    // read and print HTTP request headers by lines
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  }
  return;
}

/*
 * parse_uri - parse URI into filename and CGI args
 *             return 0 if dynamic content, 1 if static
 */
int parse_uri(char *uri, char *filename, char *cgiargs)
{
  char *ptr;

  /* Static content */
  if (!strstr(uri, "cgi-bin")) { // if uri not contains "cgi-bin", assumes static
    // initialize cgiargs to "", filename to "."
    strcpy(cgiargs, "");
    strcpy(filename, ".");
    // to filename(".", the file), append uri (convert to relative filename  ".${uri}" -(example)-> "./index.html")
    strcat(filename, uri);
    if (uri[strlen(uri)-1] == '/') { // if uri omitted filename, append default filename
      strcat(filename, "home.html");
    }
    return 1;
  }

  /* Dynamic content*/
  else { // uri contains "cgi-bin", assumes dynamic
    // extract CGI arguments
    ptr = index(uri, '?'); // ptr points to '?' in uri
    if (ptr) { // if there's arguments ('?' is in uri )
      strcpy(cgiargs, ptr+1); // to cgiargs(==""), append only arguments string from uri
      *ptr = '\0'; // change '&' into null char, (to ignore arguments from uri later)
    }
    else { // if there's no arguments ('?' is not in uri)
      strcpy(cgiargs, ""); // empty cgiargs
    }

    // convert remaining uri to relative filename
    strcpy(filename, ".");
    strcat(filename, uri); // from beginning to null char(previously '?')
    return 0;
  }
}

/*
 * serve_static - copy a file back to the client
 */
#ifdef HW11
void serve_static(int fd, char *filename, int filesize, char* method)
#else
void serve_static(int fd, char *filename, int filesize)
#endif
{
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF];

  /* Send response headers to client */
  get_filetype(filename, filetype); // determine file type by filename suffix
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  sprintf(buf, "%sConnection: close\r\n", buf);
  sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype); // "\r\n" terminates header
  Rio_writen(fd, buf, strlen(buf));
  // print for server
  printf("Response headers:\n");
  printf("%s", buf);

#ifdef HW11
  if (!strcasecmp(method, "HEAD")) { // if method is HEAD, no need to send response body
    return;
  }
#endif

#ifdef HW9
  /* Send response body to client */ // copy content(file) to connected descriptor
  srcfd = Open(filename, O_RDONLY, 0); // Open file filename(for reading) and get its descriptor
  srcp = Malloc(filesize); // would use as buffer(maps file to virtual memory area)
  Rio_readn(srcfd, srcp, filesize); // copy(read) filesize bytes from file(srcfd) to srcp
  Close(srcfd); // file descriptor no longer need, !CLOSE THE FILE!
  Rio_writen(fd, srcp, filesize); // actual transfer to client. copy(write) filesize bytes starting from srcp(data of requested file) to fd
  // printf("%s", srcp);
  Free(srcp); // !FREE MAPPED VIRTUAL MEMORY!
#else
  /* Send response body to client */ // copy content to connected descriptor
  srcfd = Open(filename, O_RDONLY, 0); // Open filename(for reading) and get its descriptor
  srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0); // maps file to virtual memory area
  Close(srcfd); // file descriptor no longer need, !CLOSE THE FILE!
  Rio_writen(fd, srcp, filesize); // actual transfer to client. copy filesize bytes starting at srcp(requested file)
  Munmap(srcp, filesize); // !FREE(unmap) MAPPED VIRTUAL MEMORY!
#endif
}

/*
 * get_filetype - derive file type from file name
 */
void get_filetype(char *filename, char *filetype)
{
  if (strstr(filename, ".html")) {
    strcpy(filetype, "text/html");
  }
  else if (strstr(filename, ".gif")) {
    strcpy(filetype, "image/gif");
  }
  else if (strstr(filename, ".png")) {
    strcpy(filetype, "image/png");
  }
  else if (strstr(filename, ".jpg")) {
    strcpy(filetype, "image/jpg");
  }
#ifdef HW7
  else if (strstr(filename, ".mp4")) {
    strcpy(filetype, "video/mp4");
  }
#endif
  else {
    strcpy(filetype, "text/plain");
  }
}

/*
 * serve_dynamic - run a CGI program on behalf of the client
 */
#ifdef HW11
void serve_dynamic(int fd, char *filename, char *cgiargs, char *method) // serves any type of dynamic content
#else
void serve_dynamic(int fd, char *filename, char *cgiargs) // serves any type of dynamic content
#endif
{
  char buf[MAXLINE], *emptylist[] = { NULL };

  /* Return first part of HTTP response */ // sending response line indicating success, informational Server header
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "server: Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));
  // (CGI program will sending the rest: not so robust)

  // for me!
  printf("Response headers:\n");
  printf("%s", buf);
  printf("(CGI sends the rest)\r\n\r\n");

  if (Fork() == 0) { /* Child */ // fork new child process
    /* Real server would set all CGI vars here */
    setenv("QUERY_STRING", cgiargs, 1); // initializes QUERY_STRING environment variable(Program argument) with cgi arguments(from URI)
#ifdef HW11
    setenv("REQUEST_METHOD", method, 1); // initializes REQUEST_METHOD environment variable with method
#endif
    Dup2(fd, STDOUT_FILENO);  /* Redirect stdout to client */
    Execve(filename, emptylist, environ); /* Run CGI program */
  }
  Wait(NULL); /* Parent waits for and reaps child */
}