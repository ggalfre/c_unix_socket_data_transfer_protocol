/* 
	This version of the server is concurrent, exploiting a different process for the service of each connected client
	It simply has a main process that iterates looking for new connection requests; once a connection is established, it create a new process 
	managing the interaction through that connection.
	Further characteristics of the server are just like the one of the sequential server, same model and functions are used.
	
*/

#include	<stdlib.h>
#include	<string.h>
#include	<inttypes.h>
#include	<unistd.h>
#include	<sys/time.h>
#include  <sys/wait.h>
#include	<sys/types.h>
#include	<sys/select.h>
#include	<sys/stat.h>
#include 	<signal.h>
#include	<fcntl.h>
#include	<errno.h>
#include	"../../lib/errlib.h"
#include	"../../lib/sockwrap.h"

// defining constant parameters
#define BUF_LEN 4096
#define BCKLOG 10
#define MAX_PATH_LEN 500
#define MAX_WORK_DIR_LEN 200
#define TIMEOUT 10

//types definitions
struct sock_data {
	int socket;
	size_t size;
	char buff[BUF_LEN];		//communication buffer
	char path[MAX_PATH_LEN];	//stores file path
	struct stat file_info;	//stores file (path) information, if file exists
	int infile;			//file descriptor used to read the file
	int w_stage;			//to manage what is going to be sent
	int get_stage;			//to manage and check what has been received
	fd_set recfd;			//sets used form the select
	fd_set sndfd;
};
typedef struct sock_data SockData;

struct pidList{
	int n;				//number of element in pids
	pid_t pids[FD_SETSIZE];	//stores the pid of all active child processes, used to collect exit statuses
	int sockets[FD_SETSIZE];	//stores the file descriptor of all active sockets, associate to the process in tha sam position in pids
};	
typedef struct pidList * PidList;					

//global variables
PidList children;	//stores the pid of all chilren processes
char *prog_name;
char work_dir[MAX_WORK_DIR_LEN];
int connection_request_socket_ipv4;	//store file descriptor of the passive socket used to receive connection requests
int connection_request_socket_ipv6;	//store file descriptor of the passive socket used to receive connection requests
int ipv4_flag = 0;



//function prototypes
void routine(int sock);
void sendErrMsg(int sock);
int checkGet(SockData *data);
int checkQuit(SockData *data);
int service(SockData *data, int rw);
void bufferShift(SockData *data, int n);
void shut_down_server(void);
void close_connection(int socket);
int getWorkDir(char *progname);
int checkPathPermissions(char * path);

/**********************************************
These function shoudn't be declared, they are not used by this server

int getNextReadAvailSkt(fd_set *rset);
int getNextWriteAvailSkt(fd_set *wset);
int getSockIndex(int socket);
int getWriteToSockIndex(int socket);
***********************************************/

//handler for the SIGCHLD signal, received by the parent process when a child exits
void sig_chld_handler(int signo) {
	int status, val, flag = 1, ret;
	
	if(signo == SIGCHLD) {
		//if SIGCHLD is received, a child process exited
		//collecting the exit status
		while((val = waitpid(-1, &status, WNOHANG)) < 0 && flag) {
		 	if(val < 0) {
		 		if(errno != EINTR) {
		 			//not interrupted by signal
		 			flag = 0;
		 		}
		 	}
		}
		if(flag) {
			if(val != 0) {
				//waitpid returned a success value
				for(int i = 0; i < children->n; i++) {
					if(val == children->pids[i]) {
						//checking exit status
						 if(WIFEXITED(status)) {
						 	ret = WEXITSTATUS(status);
						 	if(ret != 0) {
						 		//signaling an exit status different than 0: 
						 		//it means that child process has encountered problem during close operations
						 		fprintf(stdout, "Child process with PID %d exited with status %d.\n", val, ret);
						 	}
						 }	
						//child process terminated and collected
						//erasing pid value from children struct
						for(int j = i; j < children->n-1; j++) {
							children->pids[j] = children->pids[j+1];
							children->sockets[j] = children->sockets[j+1];
						}
						children->n--;
						return;
					}
				}
				//if this section of code is executed means that a child process has received SIGCHLD and one of its own children
				//exited. This should never happen because child can't generate new children
				//anyway that process exit status would have been collected, avoiding zombies
				return;
			}
			else {
				//if this section of code is executed means that a SIGCHLD from non child process have been received: ignore it
				return;
			}
		}
		else {
			return;
		}
	}
	return;
}


//handler for main process for SIGINT signals
void sig_int_handler_main(int signo) {
	int flag = 1, val, status;
	
	if(signo == SIGINT) {
		//main process received SIGINT
		
		//untill there are active children, try to wait for them
		while(children->n > 0) {
			while((val = wait(&status)) < 0 && flag) {
			 	if(val < 0) {
			 		if(errno != EINTR) {
			 			//not interrupted by signal
			 			printf("wait fail\n");
			 			flag = 0;
			 		}
			 	}
			}
			children->n--;
		}
		
		//release resources occupied by children
		free(children);
		Close(connection_request_socket_ipv6);
		if(ipv4_flag)
			Close(connection_request_socket_ipv4);
		
		exit(0);		
	}
	return;
}


//handler for children processes for SIGINT signal
void sig_int_handler_child(int signo) {
	int i, flag = 1;
	
	if(signo == SIGINT) {
		//child process received SIGINT
		
		for(i = 0; i < children->n && flag; i++) {
			if(getpid() == children->pids[i])
				flag = 0;
		}
		
		if(flag) {
			exit(0);
		}
		else {
			//if still registered as an active process, close connection and exits
			close_connection(children->sockets[i]);
		}
	}
}
	
				 			
int main(int argc, char **argv) {
	uint16_t porth4, portn4, porth6, portn6;
	int sock;
	struct sockaddr_in sock_addr_ipv4, connected_addr_ipv4;
	struct sockaddr_in6 sock_addr_ipv6, connected_addr_ipv6;
	socklen_t sock_addr_len, connected_addr_len;
	int val, flag = 1;
	fd_set readfds;	//used by main process in the select, to receive connection request
	char string[200];
	
	
	//checking constant value for path and workdir strings length
	if(MAX_PATH_LEN <= MAX_WORK_DIR_LEN) {
		fprintf(stdout, "Error: constants invalid values, MAX_PATH_LEN must be higher than MAX_WORK_DIR_LEN.\n");
		exit(1);
	}
	
	//getting working directory path
	if(!getWorkDir(argv[0])) {
		fprintf(stdout, "Path to working directory is too long, max %d characters\n", MAX_WORK_DIR_LEN-1);
		exit(1);
	}
	
	//checking arguments number
	if(argc < 2) {
		fprintf(stdout, "Too few arguments.\nThe correct syntax is:\t%s <port_IPV4only> [ <port_IPV6> ]\n", argv[0]);
		exit(1);
	}
	prog_name = argv[0];
	
	//getting port number
	if (sscanf(argv[1], "%" SCNu16, &porth6)!=1) {
		err_sys("Invalid port number");
	}
	
	if(argc > 2) {
		ipv4_flag = 1;
		//getting port number
		if (sscanf(argv[2], "%" SCNu16, &porth4)!=1) {
			err_sys("Invalid port number");
		}
	}
	
	//creating socket
	sock = Socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	
	//preparing structures for binding
	portn6 = htons(porth6);
	bzero((void *)&sock_addr_ipv6, sizeof(sock_addr_ipv6));
	sock_addr_ipv6.sin6_family = AF_INET6;
	sock_addr_ipv6.sin6_port = portn6;
	sock_addr_ipv6.sin6_addr = in6addr_any;
	sock_addr_len = sizeof(sock_addr_ipv6);
	
	//binding
	Bind(sock, (struct sockaddr *) &sock_addr_ipv6, sock_addr_len);
    	
    	//Starting listening
	Listen(sock, BCKLOG);
	
	//saving socket number of the one receiving connection requests
	connection_request_socket_ipv6 = sock;
	
	//setup of fds sets
	FD_ZERO(&readfds);
	FD_SET(connection_request_socket_ipv6, &readfds);

	if(ipv4_flag) {
		//creating socket
		sock = Socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		
		//preparing structures for binding
		portn4 = htons(porth4);
		bzero((void *)&sock_addr_ipv4, sizeof(sock_addr_ipv4));
		sock_addr_ipv4.sin_family = AF_INET;
		sock_addr_ipv4.sin_port = portn4;
		sock_addr_ipv4.sin_addr.s_addr = INADDR_ANY;
		sock_addr_len = sizeof(sock_addr_ipv4);
		
		//binding
		Bind(sock, (struct sockaddr *) &sock_addr_ipv4, sock_addr_len);
	    	
	    	//Starting listening
		Listen(sock, BCKLOG);
		
		//saving socket number of the one receiving connection requests
		connection_request_socket_ipv4 = sock;
		
		FD_SET(connection_request_socket_ipv4, &readfds);
	}
	
	//initializing pidList struct
	if((children = malloc(sizeof(struct pidList))) == NULL) {
		err_msg ("(%s) error - malloc() failed", prog_name);
		Close(connection_request_socket_ipv6);
		if(ipv4_flag)
			Close(connection_request_socket_ipv4);
		exit(1);
	}
	for(int i = 0; i < FD_SETSIZE; i++) {
		children->sockets[i] = -1;
		children->pids[i] = -1;
	}
	children->n = 0;
	
	//instantiating signal handler for SIGCHLD and SIGINT
	if(signal(SIGCHLD, &sig_chld_handler) == SIG_ERR || signal(SIGINT, &sig_int_handler_main) == SIG_ERR) {
		err_msg("signal error");
		Close(connection_request_socket_ipv6);
		if(ipv4_flag)
			Close(connection_request_socket_ipv4);		
		exit(1);
	}
	
	//main loop
	for (;;)
	{
		//preparing fd_set used by select (readfds)
		FD_SET(connection_request_socket_ipv6, &readfds);
		if(ipv4_flag)
			FD_SET(connection_request_socket_ipv4, &readfds);
					
		//looking for a connection request
		while ( (val = select(FD_SETSIZE, &readfds, NULL, NULL, NULL)) < 0 && flag)
		{
			if (INTERRUPTED_BY_SIGNAL)
				flag = 1;
			else {
				err_msg ("(%s) error - select() failed", prog_name);
				flag = 0;
			}
		}
		
		if(flag) {			
			//if passive socket is ready, then a connection must be accepted
			if(FD_ISSET(connection_request_socket_ipv6, &readfds)) {
			
				//accepting connection
				connected_addr_len = sizeof(struct sockaddr_in6);
				while ( (sock = accept(connection_request_socket_ipv6, (struct sockaddr *) &connected_addr_ipv6, &connected_addr_len)) < 0 && flag)
				{
					if (INTERRUPTED_BY_SIGNAL ||
						errno == EPROTO || errno == ECONNABORTED ||
						errno == EMFILE || errno == ENFILE ||
						errno == ENOBUFS || errno == ENOMEM			
					    )
						flag = 1;
					else {
						err_msg ("(%s) error - accept() failed", prog_name);
						flag = 0;
					}
				}				
				if(flag) {
					//Connection accepted
					inet_ntop(AF_INET6, (const void *) &(connected_addr_ipv6.sin6_addr), string, (socklen_t)200);
					fprintf(stdout, "\nAccepted connection from %s!%" PRIu16 "\n", string, htons(connected_addr_ipv6.sin6_port));
					
					//generating child process to serve the new connected client
					val = fork();
					
					if(val < 0) {
						//fork failure
						err_msg("(%s) error - fork() failed", prog_name);
					}
					else {
						if(val == 0) {
							//child process
							routine(sock);
						}
						else {
							//parent process
							//adding pid to children
							children->pids[children->n] = val;
							children->sockets[children->n++] = sock;
							//closing connection
							while(close(sock) != 0 && flag) {
								if(errno != EINTR) {
									err_msg ("(%s) error - close() failed", prog_name);
									flag = 0;
								}
							}
						}
					}
				}
			}
			else {
				//if passive socket is ready, then a connection must be accepted
				if(ipv4_flag && FD_ISSET(connection_request_socket_ipv4, &readfds)) {
				
					//accepting connection
					connected_addr_len = sizeof(struct sockaddr_in);
					while ( (sock = accept(connection_request_socket_ipv4, (struct sockaddr *) &connected_addr_ipv4, &connected_addr_len)) < 0 && flag)
					{
						if (INTERRUPTED_BY_SIGNAL ||
							errno == EPROTO || errno == ECONNABORTED ||
							errno == EMFILE || errno == ENFILE ||
							errno == ENOBUFS || errno == ENOMEM			
						    )
							flag = 1;
						else {
							err_msg ("(%s) error - accept() failed", prog_name);
							flag = 0;
						}
					}				
					if(flag) {
						//Connection accepted
						showAddr("\nAccepted connection from", &connected_addr_ipv4);
						
						//generating child process to serve the new connected client
						val = fork();
						
						if(val < 0) {
							//fork failure
							err_msg("(%s) error - fork() failed", prog_name);
						}
						else {
							if(val == 0) {
								//child process
								routine(sock);
							}
							else {
								//parent process
								//adding pid to children
								children->pids[children->n] = val;
								children->sockets[children->n++] = sock;
								//closing connection
								while(close(sock) != 0 && flag) {
									if(errno != EINTR) {
										err_msg ("(%s) error - close() failed", prog_name);
										flag = 0;
									}
								}
							}
						}
					}
				}
			}
			
		}
		flag = 1;
	}	
}


//routine is the starting point function of each child process
void routine(int sock) {
	SockData data; //structure storing all data used for socket management
	int val, flag = 1;
	struct timeval tv;
	
	//initializing data
	data.socket = sock;
	data.w_stage = -1;
	data.get_stage = -1;
	data.infile = -1;
	data.size = 0;
	FD_ZERO(&data.recfd);
	FD_ZERO(&data.sndfd);
	
	//instantiating signal handler for SIGINT
	if(signal(SIGINT, &sig_int_handler_child) == SIG_ERR) {
		err_msg("signal error");
		exit(1);
	}
	
	for(;;) {
		//setting socket for reception 
		FD_SET(sock, &data.recfd);
		
		//setting timeout
		tv.tv_sec = TIMEOUT;
		tv.tv_usec = 0;
		
		//looking for a socket with which interact
		while ( (val = select(data.socket + 1, &data.recfd, &data.sndfd, NULL, &tv)) < 0 && flag)
		{
			if (INTERRUPTED_BY_SIGNAL)
				flag = 1;
			else {
				err_msg ("(%s) error - select() failed", prog_name);
				flag = 0;
			}
		}
		
		if(flag) {
			//select success	
			if(val == 0) {
				//time out exceeded
				fprintf(stdout, "Timeout exceeded: client unreachable.\nClosing connection...\n");
				//closing connection
				close_connection(data.socket);
			}
			else {
				if(FD_ISSET(data.socket, &data.recfd)) {
					//data ready on the reception socket
					if(service(&data, 0) == -1) {
						//closing connection
						close_connection(data.socket);
					}				
				}
				else {
					if(FD_ISSET(data.socket, &data.sndfd)) {
						//data ready to be sent
						if(service(&data, 1) == -1) {
							//closing connection
							close_connection(data.socket);
						}
					}
				}
			}
		}
		flag = 1;
	}
	
}


//function implementing the service of the server, both sending and receiving functionalities
int service(SockData *data, int rw) {
	ssize_t	nrec,	//number of bytes received from socket
			nread, 	//number of bytes read from input file
			nsend;	//number of bytes sent to socket
	uint32_t tmp;		//temporaty uint32_t for size and timestamp conversion
	int 	 	socket = data->socket,
			flag = 1,	//flag used for error management
			tot_check = 0, //number of bytes, from the beginning of the buffer considered, already checked during current service call
			n;
	char *buff = data->buff;	//copy of the reference of the buffer used with the socket considered
	
	//receiving client request
	if(rw == 0) {
		 
		if(data->w_stage != -1) {
			//if data is received during sending operation, connection is closed
			//nothing should be received but TCP messages linked to connection closure (FIN or RST) in this scenario
			//even a GET request should be considered anomaly, because the protocol considers requests sent only after the previous has been satisfied	
			
			//trying to receive overwriting data of buffer (it should be data that is going to be sent)
			nrec = recv(socket, buff , (ssize_t)(BUF_LEN-1-data->size), 0);
			
			if(nrec < 0) { //if failure during receiving
				if(errno != ECONNRESET) {
					//if errno == ECONNRESET a TCP RST message has been received
					err_msg ("(%s) error - recv() failed", prog_name);
				}
				else
					fprintf(stdout, "Client closed connection.\n");
			}
			else {
				if(nrec == 0) {
					//if nrec is zero and the max number of bytes requested by recv is > 0, 
					//FIN message is being received, conection is closed
					fprintf(stdout, "Client closed connection.\n");
				}
				else {
					fprintf(stdout, "Client sent unexpected data during service execution.\n");		
					sendErrMsg(socket);
					
				}
					
			}
			return -1;				
		}
		else { 
			//if server wasn't going to send anything to the socket
			//receive data appending bytes to the content of buffer
			nrec = recv(socket, buff + data->size, (ssize_t)(BUF_LEN-1-data->size), 0);
		
			if(nrec < 0) { //if failure during receiving
				
				if(errno != ECONNRESET) {
					//if errno == ECONNRESET a TCP RST message has been received
					err_msg ("(%s) error - recv() failed", prog_name);
				}
				return -1;
			}
			else {
				if(nrec == 0 && BUF_LEN-1 > data->size) {
					//if nrec is zero and the max number of bytes requested by recv is > 0, 
					//FIN message is being received, conection is closed
					fprintf(stdout, "Client closed connection.\n");
					return -1;
				}
				else {
					//normal message reception
					
					//updating size of the content of the buffer
					data->size+=nrec;
					
					//adding string terminator
					buff[data->size] = '\0'; 
		
					if(data->get_stage < 0) {				
						//if socket's buffer is not being checked for a GET message
						
						//preventively checking the first character received to decide whether to
						//apply checks for the QUIT message or for the GET one
						switch(buff[0]) {
						
							case 'G': //GET check
								
								//setting 0 to execute GET checks from the beginning stage
								data->get_stage = 0;
								
								n = checkGet(data);
								
								if(n == -1) {
									fprintf(stdout, "-- Invalid request, closing connection...\n");
									sendErrMsg(socket);
									return -1;
								}
								else {
								
									//updating number of checked bytes
									tot_check += n;
									
									if(data->get_stage == 5) {
										//GET checking complete, start sending response
										data->w_stage = 0;
										data->get_stage = -1;
										buff[0] = '+';
										buff[1] = 'O';
										buff[2] = 'K';
										buff[3] = (char)13;
										buff[4] = (char)10;
										data->size = 5;
										tot_check = 0;
										
										//setting socket for the sending operations
										if(!FD_ISSET(data->socket, &data->sndfd));
											FD_SET(socket, &data->sndfd);
									}
									
									//shifting left buffer content, erasing checked bytes
									if(data->size == tot_check)
										data->size = 0;
									else
										bufferShift(data, tot_check); 
								}
								break;
							
							case 'Q': //QUITcheck
								if(data->size >= 6) {
									if(checkQuit(data) == 6) {
									
										tot_check += 6;
										
										//quit message received
										fprintf(stdout, "\nRequest received: << QUIT >>\n");
										
										//client is closing the connection
										//removing socket from ready write-to set and also from the list.wfds array
										if(FD_ISSET(data->socket, &data->sndfd)) {
											FD_CLR(data->socket, &data->sndfd);
										}
										//setting w_stage != -1 in order to allow reception of closure message only
										data->w_stage = 0;
										
									}
									else {
										fprintf(stdout, "-- Invalid request, closing connection...\n");
										sendErrMsg(socket);
										return -1;
									}		
								}						
								break;
							
							default: //message received is surely wrong
								fprintf(stdout, "-- Invalid request, closing connection...\n");
								sendErrMsg(socket);
								return -1;							
						}
					}
					else {
					
						//if a GET message checking is in execution, continues checking
						n = checkGet(data);
						
						if(n == -1) {
							fprintf(stdout, "-- Invalid request, closing connection...\n");
							sendErrMsg(socket);
							return -1;
						}
						else {
						
							//updating number of checked bytes
							tot_check += n;
							
							if(data->get_stage == 5) {
								//GET checking complete, start sending response
								data->w_stage = 0;
								data->get_stage = -1;
								buff[0] = '+';
								buff[1] = 'O';
								buff[2] = 'K';
								buff[3] = (char)13;
								buff[4] = (char)10;
								data->size = 5;
								tot_check = 0;
								
								//setting socket for the sending operations
								if(!FD_ISSET(data->socket, &data->sndfd));
									FD_SET(socket, &data->sndfd);
							}
							
							//shifting left buffer content, erasing checked bytes
							if(data->size == tot_check)
								data->size = 0;
							else
								bufferShift(data, tot_check); 
							
						}
					}	
				}
			}
		}
	}
	
	else if(rw == 1) { 			
		//sending response
		nsend = sendn(socket, buff, data->size, 0);
		
		//checking if succefull
		if(nsend == -1) {
			err_msg ("(%s) error - sendn() failed", prog_name);
			return -1;
		}
		else {
		
			//shifting left buffer content, erasing sent bytes
			if(data->size == nsend)
				data->size = 0;
			else
				bufferShift(data, nsend); 
			
			switch(data->w_stage) {
			
				case 0:	
					//setting up second response portion
					//converting file size info in network byte order
					tmp = htonl((uint32_t)data->file_info.st_size);
					memcpy(buff + data->size, &tmp, (size_t)4);
					data->size += 4;
					data->w_stage = 1;
					break;
				
				case 1:
					//setting up third response portion
					//converting file last modification time info in network byte order
					tmp = htonl((uint32_t)data->file_info.st_mtime);
					memcpy(buff + data->size, (char *)&tmp, (size_t)4);
					data->size += 4;
					data->w_stage = 2;
					break;
				
				case 2:
					//setting up file data sending
					if(data->infile == -1) {
						//opening file selected as input
						data->infile = open(data->path, O_RDONLY, 0);
					}
					
					//try to read from file as many bytes as to completely fill the available buffer
					nread = read(data->infile, buff + data->size, BUF_LEN - data->size);
					
					if(nread == -1) {
						//error reading file
						sendErrMsg(socket);
						return -1;
					}
					else {
						if(nread == 0) {
							//if file had been completely read
							//message success notification
							fprintf(stdout, "\n============================================================================\n");
							fprintf(stdout, "-- <%s>:file requested successfully sent to client.\n", data->path + strlen(work_dir));
							fprintf(stdout, "============================================================================\n");
							
							//file is terminated, so close it
							flag = 1;
							while(close(data->infile) != 0 && flag) {
								if(errno != EINTR) {
									err_msg ("(%s) error - close() failed", prog_name);
									flag = 0;
									return -1;
								}
							}
							data->infile = -1;
							data->w_stage = -1;
							data->size = 0;
							
							//removing socket from ready write-to set and also from the list.wfds array
							FD_CLR(data->socket, &data->sndfd);
						}
						else {
							//updating buffer content size
							data->size += nread;
						}
					}
					break;
				
				default:
					//this is redundant, it should never happen
					fprintf(stdout, "BUG: Unexpected situation during file sending.\n");
					return -1;
										
			}
		}	
	}
	
	return 1;
}

									
//this function execute all checkings necessary to find out if a correct GET message is received and if the file requested is available 
//it only needs the SockData struct reference associated to the receiving socket to get access to its data:
//--> buff stores the message's bytes already received to be checked
//--> size stores the number of bytes in the buffer
//--> path is filled with the file path
//--> file_info is used to store and check file properties
//--> get_stage is used for partially checked messages that has to be completed. 
//			 it defines which checkings have been already performed and wchich are the next ones
//All checks are performed, untill the end of buffer is reached. At each step the get_stage value is updated.
//It returns the number of bytes checked, from the beginning of the buffer; -1 if message format is not respected
int checkGet(SockData * data) {
	int 	len = data->size, //buffer content size
		path_len,		//length of the current file path received
		max_len,		//max number of char that can be appended to the currently received file path
		max_available,	//number of char available in the buffer to be appended to the currently received file path 
		tot_check = 0,	//total of the bytes in the buffer already checked/consumed
		flag = 1, 	
		i;				
	
	while(1)   {
	
		switch(data->get_stage) {
	
			case 0: //"GET|CR|LF" check
				if(len >= 5) {					
					 //checking GET header
					if(memcmp(data->buff + tot_check, "GET ", 4)) {
						return -1;
					}
					tot_check += 4;
					data->get_stage = 1;
				}
				break;
			
			case 1:
				//preparing path string, retrieving the path to server working directory and appending the filename 
				memcpy(data->path, work_dir, strlen(work_dir));
				path_len = strlen(work_dir);
				data->path[path_len] = '\0'; 
				data->get_stage++;				
				break;
			
			case 2: 
				flag = 1;
				path_len = strlen(data->path);
				max_available = len - tot_check;
				max_len = MAX_PATH_LEN-1-path_len;				
				
				for(i = 0; i < max_available && flag && i < max_len; i++) {
					if(data->buff[tot_check+i] == (char)13) {
						//if detected CR
						data->get_stage = 3;
						flag = 0;
					}
					else {
						data->path[path_len+i] = data->buff[tot_check+i];
					}
				}
				
				tot_check+=i;
				
				if(!flag) { 
					//CR has been detected
					data->path[path_len+i-1] = '\0';
				}
				else {
					if(i == max_available) { 
						//all buffer content consumed, but path is still incomplete
						data->path[path_len+i] = '\0';
						return tot_check;
					}
					if(i == max_len) {
						fprintf(stdout, "-- Path too long, max path size: %ld characters\n", MAX_PATH_LEN-strlen(work_dir)-1);
						return-1;
					}
				}
				break;
				
			case 3:
				path_len = strlen(data->path);
				if(len - tot_check >= 1) {
					if(data->buff[tot_check] == (char)10) {
						//detecting LF character
						data->get_stage = 4;
						tot_check++;
					}
					else {
						//the CR received is in reality part of the filename
						data->path[path_len] = (char)13;
						data->path[path_len+1] = '\0'; 
						data->get_stage = 2;
					}
				}
				break;
			
			case 4:			
				fprintf(stdout, "\nRequest received: << GET %s >>\n", data->path + strlen(work_dir));
				
				//checking if other bytes have been received
				if(data->size != tot_check) {
					return -1;
				}
				
				
				//checking if path exits the server's working directory
				if(!checkPathPermissions(data->path)) {
					fprintf(stdout, "-- Permission denied to access file requested.\n");
					return -1;
				}
										
				//checking file existence
				if(access((const char*)data->path, F_OK) != 0) {
					fprintf(stdout, "-- File path requested doesn't exist.\n");
					return -1;
				}
				
				//checking if path is not a directory
				if(stat(data->path, &(data->file_info)) == 0) {
					if(S_ISDIR(data->file_info.st_mode))  {
						fprintf(stdout, "-- File path requested exists, but it's a directory.\n");
						return -1;
					}
				}
				else
					return -1;
				data->get_stage = 5;
				return tot_check;
				break;
				
			default:
				//this is redundant, it should never happen
				fprintf(stdout, "BUG: Unexpected situation during GET message checkings.\n");
				return -1;
				
		}
	}
	
	return tot_check;
}


//return number of bytes checked if QUIT message is recognized, otherwise return -1
int checkQuit(SockData * data) {
	
	//checking QUIT message sintax
	if(data->buff[4] == (char)13 && data->buff[5] == (char)10 && !memcmp(data->buff, "QUIT", 4))
		return 6;
	
	return -1;
}


//sending -ERR message
void sendErrMsg(int sock) {

	ssize_t nsend;
	char buff[6];
	fd_set set;
	int flag = 1, val;
	struct timeval tv;
	
	
	//filling buff with ERR message
	strncpy(buff, "-ERR", (size_t)4);
	buff[4] = (char)13;
	buff[5] = (char)10;
	
	FD_ZERO(&set);
	FD_SET(sock, &set);
	
	//setting timeout
	tv.tv_sec = TIMEOUT;
	tv.tv_usec = 0;

	//sending message
	while ( (val = select(sock+1,NULL, &set, NULL, &tv)) < 0 && flag)
	{
		if (INTERRUPTED_BY_SIGNAL)
			flag = 1;
		else {
			err_msg ("(%s) error - select() failed", prog_name);
			flag = 0;
		}
	}
	if(flag) {
		//select success	
		if(val == 0) {
			//time out exceeded
			fprintf(stdout, "Timeout exceeded: client unreachable.\nClosing connection...\n");
			//closing connection
			close_connection(sock);
		}
		else {
			if(FD_ISSET(sock, &set)) {
				nsend = sendn(sock, buff, (size_t)6, 0);
				if(nsend == -1)
					err_msg ("(%s) error - sendn() failed", prog_name);
				else
					fprintf(stdout, "Message sent: << -ERR >>\n");	
			}
			else {
				fprintf(stdout, "Error: ERR message not sent.\n");
			}
		}
	}
}


//shifts left by n position the content of buff between n and limit
void bufferShift(SockData *data, int n) {

	int i,j;
	char *buff = data->buff;
	int limit = data->size; 
	
	for(i = n, j = 0; i < limit; i++, j++)
		buff[j] = buff[i];
	
	data->size -= n;
}


//retrieve client working directory path from the name of the program
int getWorkDir(char *progname) {

	int i = strlen(progname) -1, flag = 1;
	
	for(; i >= 0 && flag; i--) {
		if(progname[i] == '/')
			flag = 0;
	}
			
	if(i >= MAX_WORK_DIR_LEN) {
		return 0;
	}
	
	if(flag) {
		work_dir[0] = '.';
		work_dir[1] = '/';
		work_dir[2] = '\0';
	}
	else {
		i++;
		for(int j = 0; j <= i; j++) {
			work_dir[j] = progname[j];
		}
			
		work_dir[i+1] = '\0';
	}
	
	return 1;
}


//check if the path of the file requested, in the filesystem directories hierarchy, 
//is at same level or below the client working directory
int checkPathPermissions(char * path) {

	int i = strlen(work_dir), n = strlen(path), count = 0;
	
	if(path[i] == '/')
		return 0;
	
	for(; i < n; i++) {
		if(path[i] == '/')
			count ++;
		else if(path[i] == '.') {
			i++;
			if(i < n) {
				if(path[i] == '.') {
					i++;
					if(i < n) {
						if(path[i] == '/') {
							count --;
						}
					}
				}
			}
		}
	}
	if(count < 0)
		return 0;
	return 1;
}


//this function try to close the socket received as argument.
//if success exits 0, otherwise exits 1
void close_connection(int socket) {			
	 while(close(socket) != 0) {
		if(errno != EINTR) {
			err_msg ("(%s) error - close() failed", prog_name);
			exit(1);
		}
	}
	exit(0);
}
