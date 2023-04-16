/*
 * adder.c - a minimal CGI program that adds two numbers together
 */
#include "csapp.h"

#define HW10 // Write HTML form for CGI adder. 2 text boxed, form request content using GET method

int main(void) { // assumes 2 arguments are given in 1 string, separeted by '&'
  char *buf, *p;
  char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE];
  int n1=0, n2=0;

  /* Extract the two arguments */
  if ((buf = getenv("QUERY_STRING")) != NULL) { // CGI environment variable QUERY_STRING: Program arguments <- initialized by setenv()
    p = strchr(buf, '&'); // find '&' in string buf
    *p = '\0'; // change '&' into null char
#ifdef HW10
    sscanf(buf, "num1=%d", &n1); // (int)n1. from beginning, before null char(previously '&')
    sscanf(p+1, "num2=%d", &n2); // (int)n2. after null char(previously '&'), to original null char
#else
    strcpy(arg1, buf); // from beginning, before null char(previously '&')
    strcpy(arg2, p+1); // after null char(previously '&'), to original null char
    // change argument strings to integer
    n1 = atoi(arg1);
    n2 = atoi(arg2);
#endif
  }

  /* Make the response body */
  sprintf(content, "QUERY_STRING=%s", buf);
  sprintf(content, "<head><title>add.com | Internet addition portal</title></head>\r\n");
  sprintf(content, "%s<h1>Welcome to add.com: The Internet addition portal.\r\n", content);
  sprintf(content, "%s<h2>The answer is: %d + %d = %d</h2>\r\n", content, n1, n2, n1 + n2);
  sprintf(content, "%s<p>Thanks for visiting!\r\n", content);

  /* Generate the HTTP response */ // Make and print the remaining response header that serve_dynamic didn't returned
  printf("Connection: close\r\n");
  printf("Content-length = %d\r\n", (int)strlen(content));
  printf("Content-type: text/html\r\n\r\n");
  printf("%s", content); // print response body at the last
  fflush(stdout); // fflush(): empty buffer associated with stream(for now, stdout)(if possible)
  exit(0);
}
