/*
	The server proposed is a sequential program (executed by one single process) implementing the simulated concurrency model.
	The strategy followed exploits a fdList data structure storing general informations about interaction with sockets, and an array of pointer to sock_data struct,
	which are associated each to a particular active socket and store all data used by it during the service execution.
	The approach is based on a main loop that executes firstly a select on active sockets, using both an fd_set for the sockets from which receive and one for the ones for which
	some data is ready to be sent to. Accoding to the result of select, the server then checks for new connection requests, if none looks for data to be received (also for 
	conneciton closure detection), if none looks for available sockets for sending data.
	Service is the function implementing the service: 
	---	for receiving, it implements all checks (through checkGet and checkQuit functions) keeping track of the already checked
		fields of a GET request through the get_stage variable (see SockData structure).
	--- for sending, it also exploits a variable (w_stage) to define what has to be sent next.
	In order to serve all socket in a distributed way, the lastr and lastw variable of the fdList structure, store the reference to the last served socket respectively for 
	reception and sending; so next iteration will try to serve another socket if available
	
	ipv6 interoperability is supported by a passive socket listening for ipv4(automatically mapped to ipv6) or ipv6 connection requests.
	To activate a second socket, additional to the default one, and used contemporarely for the pure ipv4 interaction, a second argument must specify 
	the port number on which it has to listen.

*/

#include	<stdlib.h>
#include	<string.h>
#include	<inttypes.h>
#include	<unistd.h>
#include	<sys/time.h>
#include	<sys/types.h>
#include	<sys/select.h>
#include	<sys/stat.h>
#include	<fcntl.h>
#include	<errno.h>
#include 	<time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include	"../errlib.h"
#include	"../sockwrap.h"

// defining constant parameters
#define BUF_LEN 4096
#define BCKLOG 10
#define MAX_PATH_LEN 500
#define MAX_WORK_DIR_LEN 200
#define TIMEOUT 10


//types definitions
struct sock_data {
	size_t size;
	char buff[BUF_LEN];		//communication buffer
	char path[MAX_PATH_LEN];	//stores file path
	struct stat file_info;	//stores file information, if file exists
	int infile;			//file descriptor used to read the file
	int w_stage;			//used to know what is going to be sent
	int get_stage;			//used to know what is going to be checked in a GET message
};
typedef struct sock_data * SockData;

typedef struct fd_list {
	int nr;
	int nw;
	int rfds[FD_SETSIZE]; 		//keep track of fds of active read from sockets
	int wfds[FD_SETSIZE]; 		//keep track of fds of active write to sockets
							//these arrays are used to retrieve more efficiently the active sockets after the select has been used
	int lastr;				//stores the index corresponding to last socket from which server has received data
	int lastw;				//stores the index corresponding to last socket to which server has sent data
	SockData data[FD_SETSIZE];	//stores data used by each socket
	time_t timestamps[FD_SETSIZE];//stores, for each socket, timestamp of last time it has been served
} fdList;

//global variable
fd_set readfds;	//set of read from active sockets for select execution
fd_set writefds;	//set of write to active sockets for select execution
fdList list;		//struct keeping track of the active socket fds and data 
char *prog_name;
char work_dir[MAX_WORK_DIR_LEN];	//storing path of current working directory
int connection_request_socket_ipv4;	//store file descriptor of the passive socket used to receive connection requests
int connection_request_socket_ipv6;	//store file descriptor of the passive socket used to receive connection requests
int ipv4_flag = 0;


//function prototypes
void sendErrMsg(int sock);
int checkGet(int k);
int checkQuit(int k);
void service(int socket, int rw);
int getNextReadAvailSkt(fd_set *rset);
int getNextWriteAvailSkt(fd_set *wset);
int getSockIndex(int socket);
int getWriteToSockIndex(int socket);
void bufferShift(int k, int n);
void shut_down_server(void);
void close_connection(int socket);
int getWorkDir(char *progname);
int checkPathPermissions(char * path);


int main(int argc, char **argv) {
	uint16_t porth4, portn4, porth6, portn6;
	int sock;
	struct sockaddr_in sock_addr_ipv4, connected_addr_ipv4;
	struct sockaddr_in6 sock_addr_ipv6, connected_addr_ipv6;
	socklen_t sock_addr_len, connected_addr_len;
	int val, flag = 1;
	fd_set readyRfds;	//fd_set used by select execution, before select it is filled with same content of readfds
	fd_set readyWfds;	//fd_set used by select execution, before select it is filled with same content of writefds
	struct timeval tv;	//used by select for timeout
	char string[200];
	
	//checking on constant values consistency for string length of file path and working directory path
	if(MAX_PATH_LEN <= MAX_WORK_DIR_LEN) {
		fprintf(stdout, "Error: constants invalid values, MAX_PATH_LEN must be higher than MAX_WORK_DIR_LEN.\n");
		exit(1);
	}
	
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
	FD_ZERO(&writefds);
	FD_SET(connection_request_socket_ipv6, &readfds);
	list.nr = 0;
	list.nw = 0;
	
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
	
	
	//initializing last interaction timestamps
	for(int i = 0; i < FD_SETSIZE; i++)
		list.timestamps[i] = -1;
	
	//main loop
	for (;;)
	{
		//preparing fd_sets used by select (readyRfds and readyWfds)
		//readfds is used to keep track of the interesting socket to be checked
		memcpy((void *)&readyRfds, (void *)&readfds, sizeof(readfds));
		memcpy((void *)&readyWfds, (void *)&writefds, sizeof(writefds));
		
		//setting timeout
		tv.tv_sec = TIMEOUT;
		tv.tv_usec = 0;
			
		//looking for a socket with a ready message to be read
		while ( (val = select(FD_SETSIZE, &readyRfds, &readyWfds, NULL, &tv)) < 0 && flag)
		{
			if (INTERRUPTED_BY_SIGNAL)
				flag = 1;
			else {
				err_msg ("(%s) error - select() failed", prog_name);
				flag = 0;
			}
		}
		
		if(flag) {
			if(val == 0) {
				//timeout exceeded
				//closing all connections
				for(int i = 0; i < list.nr; i++) {
					close_connection(list.rfds[i]);
				}
			}
			else {
				//checks on sockets' last timestamp
				//if difference with current one is more than TIMEOUT, than close socket
				for(int i = 0; i< list.nr; i++) {
					//checking difference form last interaction time and now
					if(difftime(time(NULL),list.timestamps[i]) > TIMEOUT) {
					
						printf("\nTimeout exceeded for client on socket %u.\n", list.rfds[i]);
						close_connection(list.rfds[i]);
						
						//removing also from ready sets
						if(FD_ISSET(list.rfds[i], &readyRfds)) {
							FD_CLR(list.rfds[i], &readyRfds);
						}
						if(FD_ISSET(list.rfds[i], &readyWfds)) {
							FD_CLR(list.rfds[i], &readyWfds);
						}
						
					}
				}
				//if passive socket is ready, then a connection must be accepted
				if(FD_ISSET(connection_request_socket_ipv6, &readyRfds)) {
				
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
						
						//adding socket to the active ones
						FD_SET(sock, &readfds);
						if((list.data[list.nr] = (SockData)malloc(sizeof(struct sock_data))) == NULL) {
							err_msg ("(%s) error - malloc() failed", prog_name);
							close_connection(sock);
						}
						else {
							//initializing socket associated data structure
							list.rfds[list.nr] = sock;
							list.timestamps[list.nr] = time(NULL);
							list.data[list.nr]->w_stage = -1;
							list.data[list.nr]->get_stage = -1;
							list.data[list.nr]->infile = -1;
							list.data[list.nr]->size = 0;
							list.nr++;
						}
					}
				}
				else {
					//if passive socket is ready, then a connection must be accepted
					if(ipv4_flag && FD_ISSET(connection_request_socket_ipv4, &readyRfds)) {
					
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
							
							//adding socket to the active ones
							FD_SET(sock, &readfds);
							if((list.data[list.nr] = (SockData)malloc(sizeof(struct sock_data))) == NULL) {
								err_msg ("(%s) error - malloc() failed", prog_name);
								close_connection(sock);
							}
							else {
								//initializing socket associated data structure
								list.rfds[list.nr] = sock;
								list.timestamps[list.nr] = time(NULL);
								list.data[list.nr]->w_stage = -1;
								list.data[list.nr]->get_stage = -1;
								list.data[list.nr]->infile = -1;
								list.data[list.nr]->size = 0;
								list.nr++;
							}
						}
					}
					//one of the active socket is ready to be read-from or written-to
					else {
					
						//getting ready-sockets' file descriptors from read-from set
						sock = getNextReadAvailSkt(&readyRfds);
			
						if(sock != -1) {
							//updatig socket's last interaction time
							list.timestamps[getSockIndex(sock)] = time(NULL);
							//execute service, receiving mode
							service(sock, 0);
						}
						else {
							//getting ready-sockets' file descriptors from write-to set
							sock = getNextWriteAvailSkt(&readyWfds);
							
							if(sock != -1) {
								//updatig socket's last interaction time
								list.timestamps[getSockIndex(sock)] = time(NULL);
								//execute service, sending mode
								service(sock, 1);
							}
						}
					}
				}
			}
		}
		flag = 1;
	}	
}


//function implementing the service of the server, both sending and receiving functionalities
void service(int socket, int rw) {
	ssize_t	nrec,	//number of bytes received from socket
			nread, 	//number of bytes read from input file
			nsend;	//number of bytes sent to socket
	uint32_t tmp;							//temporaty uint32_t for size and timestamp conversion
	int 		k = getSockIndex(socket),		//index identifying the socket considered in the list.data array
			i = getWriteToSockIndex(socket),	//index identifyng socket considered in the list.wfds array
		 	flag = 1,						//flag used for error management
			tot_check = 0, 				//number of bytes, from the beginning of the buffer, already checked during current service call
			n;
	char *buff = list.data[k]->buff;	//copy of the reference of the buffer used with the socket considered
	
	//receiving client request
	if(rw == 0) {
	
		//updating last served socket; 
		if(k != -1) {
			list.lastr = k;
		}
		else {
			//this should never be executed
			fprintf(stdout, "BUG: socket-index association behaved wrongly.\n");
			shut_down_server();
		}
		 
		if(list.data[k]->w_stage != -1) {
			//if data is received during sending operation, connection is closed
			//nothing should be received but TCP messages linked to connection closure (FIN or RST) in this scenario
			//even a GET request should be considered anomaly, because the protocol considers a request from a client valid only only if the previous has been satisfied	
			
			//calling receive overwriting buffer data (this has to be done to understand what is received, even if buffer is storing data to be sent)
			//this is done because anyway the connection would be closed
			nrec = recv(socket, buff , (ssize_t)(BUF_LEN-1-list.data[k]->size), 0);
			
			if(nrec < 0) { //if failure during receiving
				if(errno != ECONNRESET) {
					//if errno == ECONNRESET a TCP RST message has been received
					err_msg ("(%s) error - recv() failed", prog_name);
				}
				else
					fprintf(stdout, "Client on socket %u closed connection.\n", socket);
			}
			else {
				if(nrec == 0) {
					//if nrec is zero and the max number of bytes requested by recv is > 0, 
					//FIN message is being received, conection is closed
					fprintf(stdout, "Client on socket %u closed connection.\n", socket);
				}
				else {
					fprintf(stdout, "Client on socket %u sent unexpected data, closing connection...\n", socket);		
					sendErrMsg(socket);	
				}
					
			}
			//closing connection
			close_connection(socket);				
		}
		else { 
			//if server wasn't going to send anything to the socket
			//data is received and bytes are appended to the content of buffer
			nrec = recv(socket, buff + list.data[k]->size, (ssize_t)(BUF_LEN-1-list.data[k]->size), 0);
		
			if(nrec < 0) {
				//failure during reception
				if(errno != ECONNRESET) {
					//if errno == ECONNRESET a TCP RST message has been received
					err_msg ("(%s) error - recv() failed", prog_name);
				}
				close_connection(socket);
			}
			else {
				if(nrec == 0 && BUF_LEN-1 > list.data[k]->size) {
					//if nrec is zero and the max number of bytes requested by recv is > 0, 
					//FIN message is being received, conection is closed
					fprintf(stdout, "Client on socket %u closed connection.\n", socket);
					close_connection(socket);
				}
				else {
					//normal message reception
					
					//updating size of the content of the buffer
					list.data[k]->size+=nrec;
					
					//adding string terminator
					buff[list.data[k]->size] = '\0'; 
		
					if(list.data[k]->get_stage < 0) {				
						//if socket's buffer is not being checked yet for a GET message
						
						//preventively checking the first character received to decide whether to
						//apply checks for the QUIT message or for the GET one
						switch(buff[0]) {
						
							case 'G': //GET check
								
								//setting 0 to execute GET checks from the beginning stage
								list.data[k]->get_stage = 0;
								
								n = checkGet(k);
								
								if(n == -1) {
									fprintf(stdout, "-- Invalid request, closing connection...\n");
									sendErrMsg(socket);
									close_connection(socket);
								}
								else {
								
									//updating number of checked bytes
									tot_check += n;
									
									if(list.data[k]->get_stage == 5) {
										//GET checking complete, start preparing response
										list.data[k]->w_stage = 0;
										list.data[k]->get_stage = -1;
										buff[0] = '+';
										buff[1] = 'O';
										buff[2] = 'K';
										buff[3] = (char)13;
										buff[4] = (char)10;
										list.data[k]->size = 5;
										tot_check = 0;
										
										//setting socket for the sending operations
										list.wfds[list.nw] = socket;
										list.nw++;
										FD_SET(socket, &writefds);
									}
									
									//shifting left buffer content, erasing checked bytes
									if(list.data[k]->size == tot_check)
										list.data[k]->size = 0;
									else
										bufferShift(k, tot_check); 
								}
								break;
							
							case 'Q': //QUITcheck
								if(list.data[k]->size >= 6) {
									if(checkQuit(k) == 6) {
									
										tot_check += 6;
										
										//quit message received
										fprintf(stdout, "\nRequest received: << QUIT >>\n");
										
										//client is closing the connection
										//removing socket from ready write-to set and also from the list.wfds array
										if(FD_ISSET(socket, &writefds)) {
											FD_CLR(socket, &writefds);
										
											//i is the index where the socket-id being closed is stored in the list.wfds array 
											//shifting left all other elements at its right by one position
											for(; i < list.nw-1; i++) 
												list.wfds[i] = list.wfds[i+1];
											
											list.nw--;
											
											//decreasing for security also the list.lastw value
											if(list.lastw != 0)
												list.lastw--;
										}
										
										//setting w_stage != -1 in order to allow reception of closure message only
										list.data[k]->w_stage = 0;
									}
									else {
										fprintf(stdout, "-- Invalid request, closing connection...\n");
										sendErrMsg(socket);
										close_connection(socket);
									}		
								}						
								break;
							
							default: //message received is surely wrong
								fprintf(stdout, "-- Invalid request, closing connection...\n");
								sendErrMsg(socket);
								close_connection(socket);							
						}
					}
					else {
					
						//if a GET message checking is in execution, continues checking
						n = checkGet(k);
						
						if(n == -1) {
							fprintf(stdout, "-- Invalid request, closing connection...\n");
							sendErrMsg(socket);
							close_connection(socket);
						}
						else {
						
							//updating number of checked bytes
							tot_check += n;
							
							if(list.data[k]->get_stage == 5) {
								//GET checking complete, start sending response
								list.data[k]->w_stage = 0;
								list.data[k]->get_stage = -1;
								buff[0] = '+';
								buff[1] = 'O';
								buff[2] = 'K';
								buff[3] = (char)13;
								buff[4] = (char)10;
								list.data[k]->size = 5;
								tot_check = 0;
								
								//setting socket for the sending operations
								list.wfds[list.nw] = socket;
								list.nw++;
								FD_SET(socket, &writefds);
							}
							
							//shifting left buffer content, erasing checked bytes
							if(list.data[k]->size == tot_check)
								list.data[k]->size = 0;
							else
								bufferShift(k, tot_check); 
							
						}
					}	
				}
			}
		}
	}
	//sending response messages
	else if(rw == 1) { 
		
		//updating last served socket
		if(i != -1) {
			list.lastw = i;
		}
		else {
			//this should never be executed
			fprintf(stdout, "BUG: socket-index association behaved wrongly.\n");
			shut_down_server();
		 }
			
		//sending response
		nsend = sendn(socket, buff, list.data[k]->size, 0);
		
		//checking if succefull
		if(nsend == -1) {
			err_msg ("(%s) error - sendn() failed", prog_name);
			close_connection(socket);
		}
		else {
		
			//shifting left buffer content, erasing sent bytes
			if(list.data[k]->size == nsend)
				list.data[k]->size = 0;
			else
				bufferShift(k, nsend); 
			
			//executing operations according to the sending stage
			switch(list.data[k]->w_stage) {
			
				case 0:	
					//setting up second response portion
					//converting file size info in network byte order
					tmp = htonl((uint32_t)list.data[k]->file_info.st_size);
					memcpy(buff + list.data[k]->size, &tmp, (size_t)4);
					list.data[k]->size += 4;
					list.data[k]->w_stage = 1;
					break;
				
				case 1:
					//setting up third response portion
					//converting file last modification time info in network byte order
					tmp = htonl((uint32_t)list.data[k]->file_info.st_mtime);
					memcpy(buff + list.data[k]->size, (char *)&tmp, (size_t)4);
					list.data[k]->size += 4;
					list.data[k]->w_stage = 2;
					break;
				
				case 2:
					//setting up file data sending
					if(list.data[k]->infile == -1) {
						//opening file selected as input
						list.data[k]->infile = open(list.data[k]->path, O_RDONLY, 0);
					}
					
					//try to read from file as many bytes as to completely fill the available buffer
					flag = 1;
					while((nread = read(list.data[k]->infile, buff + list.data[k]->size, BUF_LEN - list.data[k]->size)) < 0 && flag) {
						if(errno != EINTR) {
							//error reading file
							sendErrMsg(socket);
							close_connection(socket);
							flag = 0;
						}
					}
					if(flag) {
						if(nread == 0) {
							//if file had been completely read
							//message success notification
							fprintf(stdout, "\n============================================================================\n");
							fprintf(stdout, "-- <%s>:file requested successfully sent to client on socket %u.\n", list.data[k]->path + strlen(work_dir),socket);
							fprintf(stdout, "============================================================================\n");
							
							//file is terminated, so close it
							flag = 1;
							while(close(list.data[k]->infile) != 0 && flag) {
								if(errno != EINTR) {
									err_msg ("(%s) error - close() failed", prog_name);
									flag = 0;
									close_connection(socket);
								}
							}
							list.data[k]->infile = -1;
							list.data[k]->w_stage = -1;
							list.data[k]->size = 0;
							
							//removing socket from ready write-to set and also from the list.wfds array
							FD_CLR(socket, &writefds);
							
							//i is the index where the socket-id being closed is stored in the list.wfds array 
							//shifting left all other elements at its right by one position
							for(; i < list.nw-1; i++) 
								list.wfds[i] = list.wfds[i+1];
							
							list.nw--;
							
							//decreasing for security also the list.lastw value
							if(list.lastw != 0)
								list.lastw--;
						}
						else {
							//updating buffer content size
							list.data[k]->size += nread;
						}
					}
					break;
				
				default:
					//this is redundant, it should never happen
					fprintf(stdout, "BUG: Unexpected situation happened during file sending.\n");
					shut_down_server();	
										
			}
		}	
	}
	
	return;
}
			
									
//this function execute all checkings necessary to find out if a correct GET message is received and if the file requested is available 
//it only needs the index associated to the receiving socket to get access to its data structure SockeData:
//--> buff stores the message's bytes already received to be checked
//--> size stores the number of bytes in the buffer
//--> path is filled with the file path
//--> file_info is used to store and check file properties
//--> get_stage is used for partially checked messages that has to be completed. 
//			 it defines which checkings have been already performed and wchich are the next ones
//All checks are performed, untill the end of buffer is reached. At each step the get_stage value is updated.
//It returns the number of bytes checked, from the beginning of the buffer; -1 if message format is not respected
int checkGet(int k) {
	int 	len = list.data[k]->size, //buffer content size
		path_len,		//length of the current file path received
		max_len,		//max number of char that can be appended to the currently received file path
		max_available,	//number of char available in the buffer to be appended to the currently received file path 
		tot_check = 0,	//total of the bytes in the buffer already checked/consumed
		flag = 1, 	
		i;				
	
	while(1)   {
		//selecting GET checking stage
		switch(list.data[k]->get_stage) {
	
			case 0: 
				//"GET " check
				if(len >= 4) {					
					 //checking GET header
					if(memcmp(list.data[k]->buff + tot_check, "GET ", 4)) {
						return -1;
					}
					tot_check += 4;
					list.data[k]->get_stage = 1;
				}
				break;
			
			case 1:
				//preparing path string, retrieving the path to server working directory
				memcpy(list.data[k]->path, work_dir, strlen(work_dir));
				path_len = strlen(work_dir);
				list.data[k]->path[path_len] = '\0'; 
				list.data[k]->get_stage++;				
				break;
			
			case 2: 
				//filename received is appended after the working directory path
				flag = 1;
				path_len = strlen(list.data[k]->path);
				max_available = len - tot_check;
				max_len = MAX_PATH_LEN-1-path_len;				
				
				for(i = 0; i < max_available && flag && i < max_len; i++) {
					if(list.data[k]->buff[tot_check+i] == (char)13) {
						//if detected CR
						list.data[k]->get_stage = 3;
						flag = 0;
					}
					else {
						list.data[k]->path[path_len+i] = list.data[k]->buff[tot_check+i];
					}
				}
				
				tot_check+=i;
				
				if(!flag) { 
					//CR has been detected
					list.data[k]->path[path_len+i-1] = '\0';
				}
				else {
					if(i == max_available) { 
						//all buffer content consumed, but path is still incomplete
						list.data[k]->path[path_len+i] = '\0';
						return tot_check;
					}
					if(i == max_len) {
						fprintf(stdout, "Path too long, max path size: %ld characters\n", MAX_PATH_LEN-strlen(work_dir)-1);
						return-1;
					}
				}
				break;
				
			case 3:
				path_len = strlen(list.data[k]->path);
				if(len - tot_check >= 1) {
					if(list.data[k]->buff[tot_check] == (char)10) {
						//detecting LF character
						list.data[k]->get_stage = 4;
						tot_check++;
					}
					else {
						//the CR received is in reality part of the filename
						list.data[k]->path[path_len] = (char)13;
						list.data[k]->path[path_len+1] = '\0';
						list.data[k]->get_stage = 2;
					}
				}
				break;
			
			case 4:			
				//last checks on file requested validity and availability
				fprintf(stdout, "\nRequest received: << GET %s >>\n", list.data[k]->path + strlen(work_dir));
				
				//checking if other bytes have been received
				if(list.data[k]->size != tot_check) {
					return -1;
				}
				
				//checking if path exits the server's working directory
				if(!checkPathPermissions(list.data[k]->path)) {
					fprintf(stdout, "Permission denied to access file requested.\n");
					return -1;
				}
										
				//checking file existence
				if(access((const char*)list.data[k]->path, F_OK) != 0) {
					fprintf(stdout, "File path requested doesn't exist.\n");
					return -1;
				}
				
				//checking if path is not a directory
				if(stat(list.data[k]->path, &(list.data[k]->file_info)) == 0) {
					if(S_ISDIR(list.data[k]->file_info.st_mode))  {
						fprintf(stdout, "-- File path requested exists, but it's a directory.\n");
						return -1;
					}
				}
				else
					return -1;
				list.data[k]->get_stage = 5;
				return tot_check;
				break;
				
			default:
				//this is redundant, it should never happen
				fprintf(stdout, "BUG: Unexpected situation happened during GET message checkings.\n");
				shut_down_server();
				
		}
	}
	
	return tot_check;
}


//return number of bytes checked if QUIT message is recognized, otherwise return -1
int checkQuit(int k) {
	
	//checking QUIT message sintax
	if(list.data[k]->buff[4] == (char)13 && list.data[k]->buff[5] == (char)10 && !memcmp(list.data[k]->buff, "QUIT", 4))
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
			fprintf(stdout, "\nTimeout exceeded: client unreachable.\nClosing connection...\n");
			//closing connection
			close_connection(sock);
		}
		else {
			if(FD_ISSET(sock, &set)) {
				nsend = sendn(sock, buff, (size_t)6, 0);
				if(nsend == -1)
					err_msg ("(%s) error - sendn() failed", prog_name);
				else
					fprintf(stdout, "\nMessage sent: << -ERR >>\n");	
			}
			else {
				fprintf(stdout, "Error: ERR message not sent.\n");
			}
		}
	}
}


//return the fd of next available socket in fd_set
int getNextReadAvailSkt(fd_set *rset) {

	int k = list.lastr+1, i;
	
	//loop that start checking from the socket right after the last one served
	for(i = k; i < list.nr; i++) 
		if(FD_ISSET(list.rfds[i], rset))
			return list.rfds[i];
	for(i = 0; i < k; i++)
		if(FD_ISSET(list.rfds[i], rset))
			return list.rfds[i];
	return -1;
}


//return the fd of next available socket in fd_set
int getNextWriteAvailSkt(fd_set *wset) {
	int k = list.lastw+1, i;
	
	//loop that start checking from the socket right after the last one served
	for(i = k; i < list.nw; i++) 
		if(FD_ISSET(list.wfds[i], wset))
			return list.wfds[i];
	for(i = 0; i < k; i++) 
		if(FD_ISSET(list.wfds[i], wset))
			return list.wfds[i];
	return -1;
}


//return the index in the list.data and list.rfds arrays corresponding to the socket id received
int getSockIndex(int socket) {
	for(int i = 0; i < list.nr; i++) {
		if(list.rfds[i] == socket)
			return i;
	}
	return -1;
}


//return the index in the list.wfds array corresponding to the socket id received
int getWriteToSockIndex(int socket) {
	for(int i = 0; i < list.nw; i++) {
		if(list.wfds[i] == socket)
			return i;
	}
	return -1;
}


//shifts left by n position the content of buff between n and limit
void bufferShift(int k, int n) {
	int i,j;
	char *buff = list.data[k]->buff;
	int limit = list.data[k]->size; 
	
	for(i = n, j = 0; i < limit; i++, j++)
		buff[j] = buff[i];
	
	list.data[k]->size -= n;
}


//closing all active sockets and exit
void shut_down_server() {
	
	fprintf(stdout, "Shutting down server.\n");
	for(int i = 0; i < list.nr; i++) {
		close_connection(list.rfds[i]);
	}
	list.nr = 0;
	list.nw = 0;
	
	Close(connection_request_socket_ipv6);
	if(ipv4_flag)
		Close(connection_request_socket_ipv4);
	
	exit(1);
}


//close connection established on the specified socket
void close_connection( int socket) {

	int i, k = getSockIndex(socket), flag = 1;
	
	//removing socket from active list
	if(FD_ISSET(socket, &readfds)) {
		//if it is set as read-from socket (all sockets are at least in this set
		FD_CLR(socket, &readfds);
		
		if(list.data[k]->infile != -1) {
			while(close(list.data[k]->infile) != 0 && flag) {
				if(errno != EINTR) {
					err_msg ("(%s) error - close() failed", prog_name);
					flag = 0;
				}
			}
		}
		
		free(list.data[k]);		
		
		//removing blank space created in the list of sockData structs and also in the rfds array used for socket-index association
		for(i = k; i < list.nr-1; i++) {
			list.data[i] = list.data[i+1];
			list.rfds[i] = list.rfds[i+1];
			list.timestamps[i] = list.timestamps[i+1];
		}
		
		list.nr--;
		
		//decrementing also the index of the last socket served to make it reference surely to an active socket
		if(list.lastr != 0)
			list.lastr--;
	}
	
	if(FD_ISSET(socket, &writefds)) {
	
		FD_CLR(socket, &writefds);
	
		//retrieving the socket index in the write-to array
		i = getWriteToSockIndex(socket);
		if(i == -1) {
			fprintf(stdout, "BUG: socket-index association behaved wrongly.\n");
			shut_down_server();
		 }
		
		//now i is the index where the socket id being closed is stored
		//shifting left all other elements at its right by one position
		for(; i < list.nw-1; i++) {
			list.wfds[i] = list.wfds[i+1];
		}
		
		list.nw--;
		
		//decrementing also the index of the last socket served to make it reference surely to an active socket
		if(list.lastw != 0)
			list.lastw--;
	}
		
	//closing connection
	while(close(socket) != 0 && flag) {
		if(errno != EINTR) {
			err_msg ("(%s) error - close() failed", prog_name);
			flag = 0;
		}
	}
	
	return;
}


//retrieve server working directory path from the name of the program
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


//check if the path of the file requested, in the filesystem directories hierarchy, is at same level or below the server working directory
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

