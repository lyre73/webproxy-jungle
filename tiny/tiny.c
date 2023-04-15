/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *          GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);

// Iterative server, port is passed in the command line
int main(int argc, char **argv) { // port is passed in command line. (argc, argv: for command line. argc is argument count, argv is argument vector)
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr; // Enough space for any address

  /* Check command line args */
  if (argc != 2) { // main() needs only 1 argument(port). Check argc
    fprintf(stderr, "usage: %s <port>\n", argv[0]); // "usage: tiny <port>"
    exit(1);
  }

  // Open listening socket
  listenfd = Open_listenfd(argv[1]); // Open_listnfd(): return listeing descriptor. (argument: port)
  // Infinite server loop, waiting for connection request
  while (1) {
    clientlen = sizeof(clientaddr);

    // perform a transaction
    connfd = Accept(listenfd, (SA *)&clientaddr, // Accept(): wait for connection request. fill in clientaddr(+length), return connection descriptor
                    &clientlen);  // line:netp:tiny:accept
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0); // Getnameinfo(): from socket address structure to host and service name strings
    printf("Accepted connection from (%s, %s)\n", hostname, port); // prints domain name and port
    doit(connfd);   // line:netp:tiny:doit // service
    Close(connfd);  // line:netp:tiny:close // closes open descriptor
  }
}

/*
 * doit - handle one HTTP request/response transaction
 */
void doit(int fd) 
{
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio;

  /* Read request line and headers */
  // 1) Read and parse the request line
  Rio_readinitb(&rio, fd); // Rio_readinitb(): associates descriptor with read buffer(type rio_t)(address)
  if (!Rio_readlineb(&rio, buf, MAXLINE)) // Rio_readlineb(): (wrapper function) copy text line from read buffer, refill it whenever it becomes empty
    return;
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version);
  /* If client requests another method, send error message and return to main */
  if (strcasecmp(method, "GET")) {
    clienterror(fd, method, "501", "Not Implemented",
                "Tiny does not implement this method");
    return;
  }
  /* Read and ignore header */
  read_requesthdrs(&rio); //

  /* 2) Parse URI from GET request */
  is_static = parse_uri(uri, filename, cgiargs);  // flag for static or dynamic content
  /* if the file does not exist, send error message */
  if (stat(filename, &sbuf) < 0) {
    clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
    return;
  }

  if (is_static) { /* Serve static content */          
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) { // verify regular file and read permission
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
      return;
    }
    serve_static(fd, filename, sbuf.st_size); // serve static content
  }
  else { /* Serve dynamic content */
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) { // verify file executable
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
      return;
    }
    serve_dynamic(fd, filename, cgiargs); // serve dynamic content
  }
}

/*
 * clienterror - returns error code and error message to the client
 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];

  /* Build the HTTP response body*/
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcollor=""ffffff"">\r\n", body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  /* Print the HTTP response */
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf)); // Rio_writen(): transfers n bytes from buffer to descriptor
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body));
}

/* 
 * read_requesthdrs - read HTTP request headers
 */
void read_requesthdrs(rio_t *rp)
{
  char buf[MAXLINE];

  Rio_readlineb(rp, buf, MAXLINE);
  while(strcmp(buf, "\r\n")) { // terminates request headers: "\r\n"
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
    // initialize cgiargs and filename to ""
    strcpy(cgiargs, "");
    strcpy(filename, ".");
    // to file(filename)(=="."), append uri. convert to relative filename  ".${uri}" -(example)-> "./index.html"
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
void serve_static(int fd, char *filename, int filesize)
{
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF];

  /* Send response headers to client */
  get_filetype(filename, filetype); // determine file type by filename suffix
  // and send response line, response headers
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  sprintf(buf, "%sConnection: close\r\n", buf);
  sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype); // \r\n terminates header
  Rio_writen(fd, buf, strlen(buf));
  printf("Response headers:\n");
  printf("%s", buf);

  /* Send response body to client */ // copy content to connected descriptor
  srcfd = Open(filename, O_RDONLY, 0); // open filename(for reading) and get its descriptor
  srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0); // maps file to virtual memory area
  Close(srcfd); // file descriptor no longer need, !CLOSE THE FILE!
  Rio_writen(fd, srcp, filesize); // actual transfer to client. copy filesize bytes starting at srcp(requested file)
  Munmap(srcp, filesize); // !FREE MAPPED VIRTUAL MEMORY!
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
  else {
    strcpy(filetype, "text/plain");
  }
}

/*
 * serve_dynamic - run a CGI program on behalf of the client
 */
void serve_dynamic(int fd, char *filename, char *cgiargs) // serves any type of dynamic content
{
  char buf[MAXLINE], *emptylist[] = { NULL };

  /* Return first part of HTTP response */ // sending response line indicating success, informational Server header
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "server: Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));
  // (CGI program will sending the rest: not so robust)

  if (Fork() == 0) { /* Child */ // fork new child process
    /* Real server would set all CGI vars here */
    setenv("QUERY_STRING", cgiargs, 1); // initializes QUERY_STRING environment variable(Program argument) with cgi arguments(from URI)
    Dup2(fd, STDOUT_FILENO);  /* Redirect stdout // before null char(previously '&')t to client */
    Execve(filename, emptylist, environ); /* Run CGI program */
  }
  Wait(NULL); /* Parent waits for and reaps child */
}