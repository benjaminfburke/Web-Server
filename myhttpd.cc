const char * usage =
"                                                             \n"
"myhttpd server:                                              \n"
"                                                             \n"
"To use the server type:                                      \n"
"                                                             \n"
"   myhttpd [-f|-t|-p] <port>                                 \n"
"                                                             \n"
"Where 1024 < port < 65536                                    \n"
"                                                             \n"
"If port is not specified, the default is port 2222           \n"
"                                                             \n"
"The default option is to handle HTTP requests iteratively    \n"
" -f handles HTTP requests with processes                     \n"
" -t handles HTTP requests with threads                       \n"
" -p handles HTTP requests with a thread pool                 \n"
"                                                             \n";

#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <signal.h>
#include <string>
#include <dlfcn.h>
#include <link.h>
#include <errno.h>

void processRequest(int socket);
void processRequestThread(int socket);
void poolSlave(int masterSocket);
bool endsWith(char * str1, char * str2);
void writeHeader(int socket, char * content);
void writeFail(int socket, char * content);
void writeLink(int socket, char * path, char * name, char * fileType, char * parent);
void writeCGIHeader (int socket);
int sortNameAscending(const void * name1, const void * name2);
int sortNameDescending(const void * name1, const void * name2);
int sortSizeAscending(const void * name1, const void * name2);
int sortSizeDescending(const void * name1, const void * name2);
int sortModifiedTimeAscending(const void * name1, const void * name2);
int sortModifiedTimeDescending(const void * name1, const void * name2);


typedef void (*httprun)(int ssock, char * query_string);

char const * icon_unknown = "/icons/unknown.gif";
char const * icon_menu = "/icons/menu.gif";
char const * icon_back = "/icons/back.gif";
char const * icon_image = "/icons/image2.gif";


extern "C" void sigIntHandler(int sig) {
  if (sig == SIGCHLD) {
    pid_t pid = waitpid(-1, NULL, WNOHANG);
  }
}

int QueueLength = 5;
pthread_mutex_t mutex;
int port;
int countRequests = 0;
char password[256];
char * startTime;

int main(int argc, char ** argv)
{
  // Add your HTTP implementation here
  signal(SIGCHLD, sigIntHandler);

  //wrong number of arguments
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "%s", usage);
    exit(-1);
  }

  char flag;

  //get port and flag from arguments
  port = 2222;
  flag = 0;
  if (argc == 2) {
    port = atoi(argv[1]);
    flag = 0;
  }

  if (argc == 3) {
    port = atoi(argv[2]);
    flag = argv[1][1];
  }

  if (port < 1024 || port > 65536) {
    perror("port");
    exit(-1);
  }

  //set IP address and port for the server
  struct sockaddr_in serverIPAddress;
  memset(&serverIPAddress, 0, sizeof(serverIPAddress));
  serverIPAddress.sin_family = AF_INET;
  serverIPAddress.sin_addr.s_addr = INADDR_ANY;
  serverIPAddress.sin_port = htons((u_short) port);

  //allocate socket
  int masterSocket = socket(PF_INET, SOCK_STREAM, 0);
  if (masterSocket < 0) {
    perror("socket");
    exit(-1);
  }

  //set socket options to reuse port
  int optval = 1;
  int err = setsockopt(masterSocket, SOL_SOCKET, SO_REUSEADDR, (char *) &optval, sizeof(int));

  //bind the socket to the IP address and port
  int error = bind(masterSocket, (struct sockaddr *) &serverIPAddress, sizeof(serverIPAddress));
  if (error) {
    perror("bind");
    exit(-1);
  }

  //put the socket in listening mode
  error = listen(masterSocket, QueueLength);
  if (error) {
    perror("listen");
    exit(-1);
  }

  //pool concurrency mode
  if (flag == 'p') {

    pthread_t tid[QueueLength];
    pthread_mutex_init(&mutex, NULL);

    for (int i = 0; i < QueueLength; i++) {
      pthread_create(&tid[i], NULL, (void *(*)(void *)) poolSlave, (void *) masterSocket);
    }

    pthread_join(tid[0], NULL);

  }
  else {  //not -p mode

    while (1) {  //accept incoming connections

      struct sockaddr_in clientIPAddress;
      int alen = sizeof(clientIPAddress);
      int slaveSocket = accept(masterSocket, (struct sockaddr *)&clientIPAddress, (socklen_t *)&alen);

      if (slaveSocket < 0) {
        perror("accept");
        exit(-1);
      }

      if (flag == 0) {  //default - handles iteratively
        processRequest(slaveSocket);
        close(slaveSocket);

      }

      if (flag == 'f') {  // -f mode - handles with processes
        struct sigaction sa;
        sa.sa_handler = sigIntHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        pid_t pid = fork();

        if (pid == 0) {
          processRequest(slaveSocket);
          close(slaveSocket);
          exit(EXIT_SUCCESS);
        }
        else {

          if (sigaction(SIGCHLD, &sa, NULL)) {
            perror("sigaction");
            exit(2);
          }

          close(slaveSocket);

        }
      }

      if (flag == 't') {  // -t mode - handles with threads
        pthread_t thr1;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        pthread_create(&thr1, &attr, (void *(*)(void *)) processRequestThread, (void *) slaveSocket);

      }
    }
  }
  return 0;
}

void processRequestThread (int socket) {

  processRequest(socket);
  close(socket);

}

void poolSlave(int masterSocket) {

  while(1) {

    struct sockaddr_in clientIPAddress;
    int alen = sizeof(clientIPAddress);

    pthread_mutex_lock(&mutex);
    int slaveSocket = accept(masterSocket, (struct sockaddr *)&clientIPAddress, (socklen_t*)&alen);
    pthread_mutex_unlock(&mutex);

    if (slaveSocket < 0) {
      perror("accept");
      exit(-1);
    }

    processRequest(slaveSocket);
    close(slaveSocket);
  }
}

void processRequest(int socket) {

  countRequests++;  //increase request counter

  //time info
  clock_t start_t, end_t, current_t;
  start_t = clock();

  const int max = 256;  //maximum length
  char currentString[max + 1];  //buffer to store name recieved from the client
  char path[max + 1];
  int len = 0;
  int temp;

  unsigned char ch;  //variable chars will be read into
  unsigned char prevCh = 0;  //last character read

  //flags to see what has been read (0 = unread, 1 = read)
  int getFlag = 0;
  int crlfFlag = 0;
  int authFlag = 0;
  int basicFlag = 0;
  int passwordFlag = 0;
  int passwordCompare = 0;

  char currentPassword[256];
  currentPassword[0] = '\0';

  DIR * dir;

  while ((temp = read(socket, &ch, sizeof(ch))) > 0) {  //read in chars

    printf("%c", ch);

    if (ch == ' ') {  //whitespace
      if (strncmp("GET", currentString, 3) == 0) {  //GET
        getFlag = 1;
        bzero(currentString, max);
        len = 0;
      }
      else if (strncmp("Authorization", currentString, 13) == 0) {  //AUTH
        authFlag = 1;
        bzero(currentString, max);
        len = 0;
      }
      else if (strncmp("Basic", currentString, 5) == 0 && authFlag == 1) {
        basicFlag = 1;
        bzero(currentString, max);
        len = 0;
        authFlag = 0;
      }
      else if (getFlag == 1) {
        currentString[len] = '\0';
        strcpy(path, currentString);
        getFlag = 0;
      }
      else {
        authFlag = 0;
        basicFlag = 0;
      }
    }
    else if (ch == '\n' && prevCh == '\r') {
      if (basicFlag == 1) {
        if (strlen(password) == 0) {
          strcpy(password, currentString);  //update password
          passwordFlag = 1;
        }
        else if (strlen(currentPassword) == 0) {
          strcpy(currentPassword, currentString);  //update current password
          passwordFlag = 1;
          passwordCompare = strcmp(password, currentPassword);

        }
        else {
          passwordCompare = strcmp(password, currentPassword);
        }

        basicFlag = 0;
      }

      bzero(currentString, max);
      len = 0;

      if (crlfFlag == 1) {
        break;
      }
      crlfFlag = 1;

    }
    else {

      prevCh = ch;

      if (ch != '\r') {
        crlfFlag = 0;
      }

      currentString[len] = ch;
      len++;


    }
  }

  printf("password is %s\n", password);
  printf("current password is %s\n", currentPassword);
  printf("password compare result is %d\n", passwordCompare);
  printf("got password? %d\n", passwordFlag);
  printf("document path is %s\n", path);

  //mapping the document path to the file
  char filePath[max];
  char c[256];

  int parent = 0;

  getcwd(c, 256);

  if (strncmp("/icons", path, 6) == 0) {
    sprintf(filePath, "%s/http-root-dir%s", c, path);
  }
  else if (strncmp("/htdocs", path, 7) == 0) {
    sprintf(filePath, "%s/http-root-dir%s", c, path);
  }
  else if (strcmp("/", path) == 0 || strlen(path) == 0) {
    sprintf(filePath, "%s/http-root-dir/htdocs/index.html", c);
  }
  else if (strncmp("/cgi", path, 4) == 0) {
    sprintf(filePath, "%s/http-root-dir%s", c, path);
  }
  else if (strstr(filePath, "..") != NULL) {
    parent = 1;
  }
  else {
    sprintf(filePath, "%s/http-root-dir/htdocs%s", c, path);
  }

  printf("realpath is %s\n", filePath);

  char * s;

  //check validity of password
  if (passwordFlag == 0 || passwordCompare != 0) {

    printf("Authentication failed\n");
    const char * p = "\"myhttpd-cs252\"";

    write(socket, "HTTP/1.1 401 Unauthorized\r\n", 27);
    write(socket, "WWW-Authenticate: Basic realm=", 30);
    write(socket, p, strlen(p));
    write(socket, "\r\n\r\n", 4);
    printf("pass test");

  }
  else if (strstr(filePath, "cgi-bin")) {
    char exe[128];
    s = strchr(filePath, '?');
    if (s) {
      *s = '\0';
    }
    strcpy(exe, filePath);
    if (s) {
      *s = '?';
    }

    if (!open(exe, O_RDONLY)) {
      writeFail(socket, "text/plain");
      perror("open(cgi)");
      exit(1);
    }
    else {
      char ** arguements = (char **)malloc(sizeof(char *) * 2);
      arguements[0] = (char *)malloc(sizeof(char) * strlen(filePath));

      for (int i = 0; i < strlen(filePath); i++) {
        arguements[0][i] = '\0';
      }

      arguements[1] = NULL;

      s = strchr(filePath, '?');

      if (s) {
        s++;
        strcpy(arguements[0], s);
      }

      writeCGIHeader(socket);

      int tempout = dup(1);
      dup2(socket, 1);
      close(socket);

      pid_t pid = fork();
      if (pid == 0) {
        setenv("REQUEST_METHOD", "GET", 1);
        setenv("QUERY_STRING", arguements[0], 1);
        execvp(exe, arguements);
        exit(2);
      }

      dup2(tempout, 1);
      close(tempout);

      free(arguements[0]);
      free(arguements[1]);
      free(arguements);

    }

  }
  else if ((dir=opendir(filePath)) != NULL || endsWith(filePath, "?C=M;O=A") || endsWith(filePath, "?C=M;O=D") || endsWith(filePath, "?C=N;O=A") || endsWith(filePath, "?C=N;O=D") || endsWith(filePath, "?C=S;O=A") || endsWith(filePath, "?C=S;O=D")) {
    char sortMode = 'N';
    char sortOrder = 'A';

    if (dir == NULL) {
      sortMode = filePath[strlen(filePath) - 5];
      sortOrder = filePath[strlen(filePath) - 1];
      filePath[strlen(filePath) - 8] = '\0';
      path[strlen(path) - 8] = '\0';
      dir = opendir(filePath);
    }

    writeHeader(socket, "text/html");
    struct dirent * ent;
    char ** content = (char **)malloc(sizeof(char *) * 128);
    int numFiles = 0;
    
    while((ent = readdir(dir)) != NULL) {
      if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) {
        content[numFiles] = (char *)malloc(sizeof(char) * strlen(ent->d_name + strlen(filePath) + 5));
        content[numFiles][0] = '\0';
        strcat(content[numFiles], filePath);

        if (!endsWith(filePath, "/")) {
          strcat(content[numFiles], "/");
        }
        strcat(content[numFiles], ent->d_name);
        printf("content[%d] is %s\n", numFiles, content[numFiles]);
        numFiles++;
      }
    }

    //sorting by size
    if (sortMode == 'S') {
      if (sortOrder == 'A') {  //Ascending
        qsort(content, numFiles, sizeof(char *), sortSizeAscending);

      }
      if (sortOrder == 'D') {  //Ascending
        qsort(content, numFiles, sizeof(char *), sortSizeDescending);
      }
    }
    else if (sortMode == 'M') {  //sort by modified time
      if (sortOrder == 'A') {  //Ascending
        qsort(content, numFiles, sizeof(char *), sortModifiedTimeAscending);

      }
      if (sortOrder == 'D') {  //Ascending
        qsort(content, numFiles, sizeof(char *), sortModifiedTimeDescending);
      }
    }
    else if (sortMode == 'N') {  //sort by name
      if (sortOrder == 'A') {  //Ascending
        qsort(content, numFiles, sizeof(char *), sortNameAscending);

      }
      if (sortOrder == 'D') {  //Ascending
        qsort(content, numFiles, sizeof(char *), sortNameDescending);
      }

    }

    //write to socket
    printf("passes\n");

  char const * t = "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 3.2 FINAL//EN\">\n<html>\n<head>\n<title>";
  write(socket, t, strlen(t));
  write(socket, "Index of ", 9);
  write(socket, filePath, strlen(filePath));

  t = "</title>\n</head>/n<body>";
  write(socket, t, strlen(t));
  write(socket, "Index of ", 9);
  write(socket, filePath, strlen(filePath));

  t = "</h1>\n<table>\n<tr><th valign=\"top\"><img src=\"/icons/blank.gif\" alt=\"[ICO]\"></th>";
  write(socket, t, strlen(t));

  t = "<th><a href=\"?C=N;O=D\">Name</a></th>";
  write(socket, t, strlen(t));

  t = "<th><a href=\"?C=M;O=A\">Last modified</a></th>";
  write(socket, t, strlen(t));

  t = "<th><a href=\"?C=S;O=A\">Size</a></th>";
  write(socket, t, strlen(t));

  t = "<th><a href=\"?C=M;O=A\">Last modified</a></th>";
  write(socket, t, strlen(t));

  t = "</tr><tr><th colspan=\"5\"><hr></th></tr>\n";
  write(socket, t, strlen(t));


  //path of parent
  char pPath[256];
  strcpy(pPath, path);
  if (!endsWith(filePath, "/")) {
    pPath[strlen(path)] = '/';
    pPath[strlen(path+2)] = '.';
    pPath[strlen(path+3)] = '\0';
  }
  else {
    pPath[strlen(path)] = '.';
    pPath[strlen(path)+2] = '\0';
  }

  pPath[strlen(path) + 1] = '.';

  writeLink(socket, pPath, "Parent Directory", "DIR", path);

  for (int i = 0; i < numFiles; i++) {
    char name[128];
    int begins = 0;
    for (int j = strlen(content[i]) - 1; j >= 0; j--) {
      if (content[i][j] == '/') {
        begins = j;
        break;
      }
    }

    for (int j = begins + 1; j < strlen(content[i]); j++) {
      name[j-begins-1] = content[i][j];
      if (j == strlen(content[i]) -1) {
        name[j-begins] = '\0';
      }
    }

    char * ft;
    //get file type
    if (opendir(content[i]) != NULL) {  //dir
      ft = "DIR";
    }
    else {
      ft = "   ";
    }

    //write content to socket
    writeLink(socket, content[i], name, ft, path);
  }

  t = "<tr><th colspan=\5\"<hr></th></tr>\n</table><address>Apache/2.4.18 (Ubuntu) Server at www.cs.purdue.edu Port 2222</address>\n</body><html>";

  write(socket, t, strlen(t));

  //free content and close directory
  for (int i = 0; i < numFiles; i++) {
    free(content[i]);
  }
  free(content);
  closedir(dir);
  }
  else {
    char cType[16];

    //get content type
    if (endsWith(filePath, ".html") || endsWith(filePath, ".html/")) {
      strcpy(cType, "text/html");
    }
    else if (endsWith(filePath, ".gif") || endsWith(filePath, ".gif/")) {
      strcpy(cType, "text/gif");
    }
    else if (endsWith(filePath, ".svg")) {
      strcpy(cType, "image/svg+xml");
    }
    else {
      strcpy(cType, "text/plain");
    }

    int info = open(filePath, O_RDWR);

    if (info < 0 || parent == 1) {
      perror("open");
      writeFail(socket, cType);
    }
    else {
      writeHeader(socket, cType);
      char data[1000000];
      int count;
      while (count = read(info, data, 1000000)) {
        if (write(socket, data, count) != count) {
          perror("write");
          break;
          bzero(data, 1000000);
        }
      }
    }
    close(info);

  }

  //update times
  end_t = clock();
  current_t = (double)(end_t - start_t);

  char host[1023];
  gethostname(host, 1023);
  strcat(host, ":");

  char convert[8];
  sprintf(convert, "%d", port);
  strcat(host, convert);
  strcat(host, filePath);
  strcat(host, "\n");

  printf("test");

  FILE * fp = fopen("/homes/burke174/Desktop/cs252/lab5-src/http-root-dir", "a+");
  fwrite(host, sizeof(char), strlen(host), fp);
  fclose(fp);




}

bool endsWith(char * str1, char * str2) {
  int temp = strlen(str2);
  int start = strlen(str1) - temp;

  if (start < 0) {
    return false;
  }

  for (int i = 0; i < temp; i++) {
    if (str1[start + i] != str2[i]) {
      return false;
    }
  }

  return true;
}

void writeHeader(int socket, char * content) {

  write(socket, "HTTP/1.1 200 Document follows\r\n", 31);
  write(socket, "Server: CS 252 lab5\r\n", 21);
  write(socket, "Content type: ", 14);
  write(socket, content, strlen(content));
  write(socket, "\r\n\r\n", 4);

}

void writeCGIHeader(int socket) {
  write(socket, "HTTP/1.1 200 Document follows\r\n", 31);
  write(socket, "Server: CS 252 lab5\r\n", 21);
}

void writeFail(int socket, char * content) {
  const char * notFound = "File not Found";
  write(socket, "HTTP/1.0 404FileNotFound\r\n", 26);
  write(socket, "Server: CS 252 lab5\r\n", 21);
  write(socket, "Content type: ", 14);
  write(socket, content, strlen(content));
  write(socket, "\r\n\r\n", 4);
  write(socket, notFound, strlen(notFound));

}

int sortNameAscending(const void * name1, const void * name2) {
  const char * nameA = *(const char **)name1;
  const char * nameB = *(const char **)name2;
  return strcmp(nameA, nameB);
}

int sortNameDescending(const void * name2, const void * name1) {
  const char * nameA = *(const char **)name1;
  const char * nameB = *(const char **)name2;
  return strcmp(nameA, nameB);
}

int sortModifiedTimeAscending(const void * name1, const void * name2) {
  const char * nameA = *(const char **)name1;
  const char * nameB = *(const char **)name2;
  struct stat nameA_stat, nameB_stat;
  stat(nameA, &nameA_stat);
  stat(nameB, &nameB_stat);
  return difftime(nameA_stat.st_mtime, nameB_stat.st_mtime);
}

int sortModifiedTimeDescending(const void * name2, const void * name1) {
  const char * nameA = *(const char **)name1;
  const char * nameB = *(const char **)name2;
  struct stat nameA_stat, nameB_stat;
  stat(nameA, &nameA_stat);
  stat(nameB, &nameB_stat);
  return difftime(nameA_stat.st_mtime, nameB_stat.st_mtime);
}

int sortSizeAscending(const void * name1, const void * name2) {
  const char * nameA = *(const char **)name1;
  const char * nameB = *(const char **)name2;
  struct stat nameA_stat, nameB_stat;
  stat(nameA, &nameA_stat);
  stat(nameB, &nameB_stat);
  int temp1 = nameA_stat.st_size;
  int temp2 = nameB_stat.st_size;
  return temp1 - temp2;
}

int sortSizeDescending(const void * name2, const void * name1) {
  const char * nameA = *(const char **)name1;
  const char * nameB = *(const char **)name2;
  struct stat nameA_stat, nameB_stat;
  stat(nameA, &nameA_stat);
  stat(nameB, &nameB_stat);
  int temp1 = nameA_stat.st_size;
  int temp2 = nameB_stat.st_size;
  return temp1 - temp2;
}

void writeLink(int socket, char * path, char * name, char * fileType, char * parent) {

  char const * temp = "<tr><td valign=\"top\"><img src=\"";
  write(socket, temp, strlen(temp));

  char * gif = (char *)icon_unknown;
  char * ft = strdup(fileType);

  if (!strcmp(name, "Parent Directory")) {
    gif = (char *)icon_back;
  }
  else if (!strcmp(fileType, "DIR")) {
    gif = (char *)icon_menu;
  }

  write(socket, gif, strlen(gif));

  temp = "\" alt=\"[";
  write(socket, temp, strlen(temp));
  write(socket, ft, strlen(ft));

  temp = "]\"></td><td><a href=\"";
  write(socket, temp, strlen(temp));

  if (strcmp(name, "Parent Directory")) {
    if (!endsWith(parent, "/")) {
      char ch[128];
      ch[0] = '\0';
      strcat(ch, parent);
      strcat(ch, "/");
      strcat(ch, name);
      write(socket, ch, strlen(ch));
    }
    else {
      write(socket, name, strlen(name));
    }
  }
  else {
    write(socket, path, strlen(path));
  }

  temp = "\">";
  write(socket, temp, strlen(temp));
  write(socket, name, strlen(name));

  temp = "</a>";
  write(socket, temp, strlen(temp));

  char mtime[20];
  mtime[0] = '\0';
  int x = 0;

  if (!strcmp(name, "Parent Directory")) {
    char const * nbsp = "</td><td>&nbsp;";
    write(socket, nbsp, strlen(nbsp));

  }
  else {
    struct stat name_stat;
    stat(path, &name_stat);

    struct tm * timeStruct;
    timeStruct = localtime(&(name_stat.st_mtime));
    strftime(mtime, 20, "%F %H:%M", timeStruct);
    x = name_stat.st_size;
  }

  temp = " </td><td align=\"right\">";
  write(socket, temp, strlen(temp));
  write(socket, mtime, strlen(mtime));

  if (strcmp(name, "Parent Directory")) {
    write(socket, temp, strlen(temp));
    if (strcmp(fileType, "DIR")) {
      char c[16];
      snprintf(c, 16, "%d", x);
      write(socket, c, strlen(c));
    }
    else {
      write(socket, "-", 1);
    }
  }

  temp = " </td><td>$nbsp;</td><tr>\n";
  write(socket, temp, strlen(temp));



}


