/*

	Client simply establish connection with server and sends, one after the other, all filename received as arguments.
	It waits for the completion of the reception of the requested file before sending the following request.
	When all files requested are received correctly, it sends the QUIT message.
	The information used to connect to the server and to create the socket are retrieved through the getaddrinfo function
	This allows to connect to both ipv4 and ipv6 addresses, if the server port selected is configured for both ip versions.
	It also allows name resolution (tested with "localhost").
	
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include	"../../lib/errlib.h"
#include	"../../lib/sockwrap.h"

// defining constant parameters
#define BUF_LEN 4096
#define MAX_PATH_LEN 250
#define MAX_WORK_DIR_LEN 100
#define TIMEOUT 15

//global variables
fd_set readfds;	//set of read from active sockets for select execution
fd_set writefds;	//set of write to active sockets for select execution
struct timeval tv;
char *prog_name;
char **arguments;	//it is a copy of the reference to program arguments
char localpath[MAX_PATH_LEN]; //stores the file local path, used to save the file received
char buff[BUF_LEN];
char work_dir[MAX_WORK_DIR_LEN];
int 	sock,		//descriptor of the socket used for communication
	outfile = -1, 	//descriptor of the output file
	buff_size = 0, //number of bytes stored in buff
	stage = 0,	//keep track of the step reached in the response reception sequence 
	missing = 0,	//stores number of file's bytes not yet received
	filesize = 0,	//stores expected size of the file received
	timestamp = 0,
	filename_index,//index in argv array correpnding to the current filename
	flag_ext,		//flag used to iterate or not into the external while
	n_requests;	//keep track of number of requests to send
struct addrinfo *result;
struct addrinfo *serverinfo;
struct addrinfo *tmp;


//functions prototypes
int elaborate(void);
void bufferShift(int n);
void getFileLocalPath(int n);
int checkErr(int offset);
int getWorkDir(char *progname);


int main(int argc, char **argv) {
	uint16_t port;
	int 	nrec, nsend, val, n,
		tot_check = 0,		//keep track of the byte in buffer already checked
		filename_size = 0,	//size of the current filename
		flag = 1;
			
	//checking on constant values consistency for string length of file path and working directory path
	if(MAX_PATH_LEN <= MAX_WORK_DIR_LEN) {
		fprintf(stderr, "Error: constants invalid values, MAX_PATH_LEN must be higher than MAX_WORK_DIR_LEN.\n");
		exit(1);
	}
	
	if(!getWorkDir(argv[0])) {
		fprintf(stdout, "Path to working directory is too long, max %d characters\n", MAX_WORK_DIR_LEN-1);
		exit(1);
	}
	
	//checking arguments number
	if(argc < 4) {
		fprintf(stderr, "Error: invalid number of arguments.\nThe correct syntax is:\t%s <address> <port> <file_path1> [<file_path2>...]\n", argv[0]);
		exit(1);
	}
	n_requests = argc -2;

	prog_name = argv[0];
	
	//copying reference to argv to make it global
	arguments = argv;
	
	//using getaddrinfo to retrieve the informations about the address to be reached and create the socket
	if(getaddrinfo(argv[1], argv[2], NULL, &result) != 0) {
		fprintf(stdout, "getaddrinfo failure. Exit...\n");
		exit(1);
	}
	
	//creating socket and trying to connect with all results received by getaddrinfo
	for (serverinfo = result; serverinfo != NULL && flag; serverinfo = serverinfo->ai_next) {
     	if((sock = socket(serverinfo->ai_family, serverinfo->ai_socktype, serverinfo->ai_protocol)) != -1) {
     		if(connect(sock, (struct sockaddr *)serverinfo->ai_addr, serverinfo->ai_addrlen) != -1) {
     			flag = 0;
     		}
     	}
     }
     
     if(flag) {
     	//failure establishing connection
     	printf("Failure establishing connection.\n");
     	for (serverinfo = result; serverinfo != NULL; ) {
			tmp = serverinfo->ai_next;
			free(serverinfo);
			serverinfo = tmp;
		}
     	exit(1);
     }
     //connection established
     port = *((uint16_t *)(serverinfo->ai_addr+2));
     fprintf(stdout, "\nTrying to connect to %s:", argv[1]);
     fprintf(stdout, "%" PRIu16 "...\n", ntohs(port));
	
	//setup of fds sets
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	FD_SET(sock, &readfds);

	//iterates untill there are requests to be sent
	while(n_requests) {
	
		//sending GET message
		if(n_requests > 1) {
			
			//calculating index corresponding to the filename, according to the request number
			filename_index = argc-n_requests+1;
			
			//creating message
			strncpy(buff, "GET ", 4);
			buff_size+=4;
			
			//copying filename into buffer's message to be sent
			if((filename_size = strlen(argv[filename_index])) > BUF_LEN-6) {
				fprintf(stdout, "Error: filename requested is too big to fit the sending buffer, chenge BUF_LEN value.\n");
				Close(sock);
				for (serverinfo = result; serverinfo != NULL; ) {
					tmp = serverinfo->ai_next;
					free(serverinfo);
					serverinfo = tmp;
				}
				exit(1);
			}
			memcpy(buff + buff_size, argv[filename_index], filename_size);
			
			buff_size += filename_size;
			buff[buff_size++] = (char)13;
			buff[buff_size++] = (char)10;
			
			//setting socket to be noticed by select to be written to
			FD_SET(sock, &writefds);
			
			flag_ext = 1;
			
		}
		else { //sending QUIT message
		
			//creating quit message
			strncpy(buff, "QUIT", 4);
			buff[4] = (char)13;
			buff[5] = (char)10;
			buff_size = 6;
			
			//setting socket to be noticed by select to be written to
			FD_SET(sock, &writefds);
			
			flag_ext = 1;
		}
		
		//this loop is interrupted if a new request has to be sent and the previous has been completed 
		while(flag_ext) { 
			
			//setting timeout
			tv.tv_sec = TIMEOUT;
     		tv.tv_usec = 0;
     		
			//select the socket with whom interact			
			while ( (val = select(sock+1, &readfds, &writefds, NULL, &tv)) < 0)
			{
				if (!INTERRUPTED_BY_SIGNAL) {
					//closing connection
					close(sock);
					//closing file
					if(outfile != -1)
						Close(outfile);
					for (serverinfo = result; serverinfo != NULL; ) {
						tmp = serverinfo->ai_next;
						free(serverinfo);
						serverinfo = tmp;
					}
					err_sys ("(%s) error - select() failed", prog_name);
				}
			}
			
			//select success	
			if(val == 0) {
				//time out exceeded
				fprintf(stdout, "Timeout exceeded, no response received from server.\nClosing connection...\n");
				//closing connection
				close(sock);
				//closing file
				if(outfile != -1)
					Close(outfile);
				for (serverinfo = result; serverinfo != NULL; ) {
					tmp = serverinfo->ai_next;
					free(serverinfo);
					serverinfo = tmp;
				}			
				exit(1);
			}
			else {			
				//checking first socket from which read, then the ones to which write
				//this ensure tempestive response to closing messages
				if(FD_ISSET(sock, &readfds)) {
					
					//filling buffer with received data starting from the offset
					nrec = recv(sock, buff + buff_size, BUF_LEN-1-buff_size, 0);
					
					if(nrec < 0) { 
						//failure during reception
						if(errno == ECONNRESET) {
							//received RST TCP message
							fprintf(stdout, "Received RST from %u socket.\n", sock);
							//closing connection
							close(sock);
							//closing file
							if(outfile != -1)
								Close(outfile);
							for (serverinfo = result; serverinfo != NULL; ) {
								tmp = serverinfo->ai_next;
								free(serverinfo);
								serverinfo = tmp;
							}
							return 0;;
						}
						else {
							//closing connection
							close(sock);
							//closing file
							if(outfile != -1)
								Close(outfile);
							for (serverinfo = result; serverinfo != NULL; ) {
								tmp = serverinfo->ai_next;
								free(serverinfo);
								serverinfo = tmp;
							}
							err_sys ("(%s) error - recv() failed", prog_name);
						}
					}
					else {
						if(nrec == 0) { //server closed the connection
							
							//closing connection
							close(sock);
							//closing file
							if(outfile != -1)
								Close(outfile);
								
							for (serverinfo = result; serverinfo != NULL; ) {
								tmp = serverinfo->ai_next;
								free(serverinfo);
								serverinfo = tmp;
							}
							fprintf(stdout, "Server on socket %u closed connection.\n", sock);
							
							return 0;;
						}
						else { 
							//message effectively received
							
							buff_size += nrec;
							
							if(stage == 0) {
								//clients is not expecting any data, because:
								// it hasn't yet sent any requests, or 
								// is going to send the next one, and new data reception means that more bytes than expected have been sent to it
								
								//check for -ERR message, maybe something went wrong, and server sent -ERR
								//checking for -ERR
								if(checkErr(tot_check)) {
									if(buff_size-tot_check >= 6) {
										//-ERR has been received
										fprintf(stdout, "Received -ERR, server is closing connection...\n");
										
										//removing socket from the write-to set, client is waiting for the close
										if(FD_ISSET(sock, &writefds))
											FD_CLR(sock, &writefds);
										tot_check+=6;
									}
									//bytes remaining are likely to be a partial part of -ERR, but not enough bytes to state it
								}
								else {
									//Otherwise unexpected data received
									fprintf(stdout, "Unexpected Data received. Closing Connection.\n");
									//closing connection
									close(sock);
									//closing file
									if(outfile != -1)
										Close(outfile);
									for (serverinfo = result; serverinfo != NULL; ) {
										tmp = serverinfo->ai_next;
										free(serverinfo);
										serverinfo = tmp;
									}
									
									exit(1);
								}
							}
							else {
								//client was expecting data, than elaborate it.
								
								//executing checks on the received data
								n = elaborate();
								
								if(n < 0) {
									
									fprintf(stdout, "Received mismatching data. Closing connection.\n");
									//closing connection
									close(sock);
									//closing file
									if(outfile != -1)
										Close(outfile);
									for (serverinfo = result; serverinfo != NULL; ) {
										tmp = serverinfo->ai_next;
										free(serverinfo);
										serverinfo = tmp;
									}
									exit(1);
								}
								
								tot_check += n;
								
								//shifting buffer content to erase already checked data
								if(buff_size == tot_check)  //all buffer has been already checked
									buff_size = 0;
								else
									bufferShift(tot_check);	//shifting unchecked bytes in the buffer 
									
							}
								
							//resetting number of bytes checked
							tot_check = 0;
						}
					}
				}
				else	if(FD_ISSET(sock, &writefds)) {  
						//message has to be senT on the socket
				
						//sending data
						if ((nsend = sendn(sock, buff, buff_size, 0)) != buff_size) {
							//closing connection
							close(sock);
							for (serverinfo = result; serverinfo != NULL; ) {
								tmp = serverinfo->ai_next;
								free(serverinfo);
								serverinfo = tmp;
							}
							err_sys ("(%s) error - sendn() failed", prog_name);
						}
						
						if(n_requests > 1) {
							//setting stage to activate the reception phase
							//if stage 
							stage = 1;
						}
						else {
							//QUIT has been sent closing connection
							Close(sock);
							n_requests--;
							flag_ext = 0;
						}
						
						//removing socket from the write-to set
						FD_CLR(sock, &writefds);
						
						//resetting buff_size for reception
						buff_size = 0;
				}
			}
			
			//resetting select fd set for read from socket
			FD_ZERO(&readfds);
			FD_SET(sock, &readfds);	
		}		
	}
	for (serverinfo = result; serverinfo != NULL; ) {
		tmp = serverinfo->ai_next;
		free(serverinfo);
		serverinfo = tmp;
	}
	return(0);
}


//this function implements the elaboration of received data by the client
//It firstly look for correctness of the message syntax, then stores locally info about file size and timestamp received
//lastly receive data untill the size expected is reached. If less data is received, select's timeout should be reached and connection closed.
//If more data received, this function notices it and close connection.
//The reception stages are managed through a global variable that allows this function to continue checks from the point they were executed during the previous call
//this is done in order to have adaptability to reception of consequent chuncks of bytes.
//Returns the number of bytes in the buffer that Have been elaborated, -1 in case of mismatching data
int elaborate(void) {
	int tot_check = 0, nwrite;
	uint32_t tmp_int;
				
	//loop to perform checks and data processing on received bytes. 
	//It is interrupted when more data is needed to continue and a new recv should be called
	while(1) {
	
		//stage identify which is next operation to be performed
		switch(stage) {				
			case 1: 
				//check on OK reception
				
				if(buff_size >= 6) {
					//if enough data to be a "+OK|CR|LF" or "-ERR|CR|LF" message is received, starts checking
					if(!memcmp(buff, "+OK", 3) && buff[3] == (char)13 && buff[3] != (char)10) { 
						//if "+OK|CR|LF" reception confirmed
						//next stage
						stage++;
						//increment checked bytes counter
						tot_check+=5; 
					}
					else {
						//if not a +OK message
						//checking for -ERR
						if(checkErr(tot_check)) {
							//-ERR has been received
							stage = 0;
							fprintf(stdout, "Received -ERR, server is closing connection...\n");
							
							//removing socket from the write-to set, client is waiting for the close
							if(FD_ISSET(sock, &writefds))
								FD_CLR(sock, &writefds);
							tot_check+=6;
							
							return tot_check;
						}
						else
							return -1;
					}
				}
				else
					return tot_check;
				break;
			
			case 2: //file size reception
				
				if(buff_size - tot_check >= 4) {
					
					//converting size received in a 4 byte string + termination character
					memcpy(&tmp_int, buff + tot_check, 4);
					
					//converting size received into host format
					filesize = (int)ntohl(tmp_int);
					
					//updating missing file's bytes with the filesize
					missing = filesize;
					
					//next stage
					stage++;
					
					//increment checked bytes counter
					tot_check+=4;
				}	
				else
					return tot_check;						
				break;
				
			case 3: //timestamp reception
				
				if(buff_size - tot_check >= 4) {
					
					//converting timestamp received in a 4 byte string + termination character
					memcpy(&tmp_int, buff + tot_check, 4);
					
					//converting size received into host format
					timestamp = (int)ntohl(tmp_int);
					
					//next stage
					stage++;
					
					//increment checked bytes counter
					tot_check+=4;
					
					//opening output file on which write the received data
					getFileLocalPath(filename_index); 
					outfile = open(localpath, O_WRONLY | O_CREAT, 0777);
					
					//if all data received untill now has been checked
					if(!(buff_size-tot_check)) {
						//further bytes must be received
						return tot_check;
					}
				}
				else
					return tot_check;
				break;
				
			case 4: //file data reception
			
				//checking if there is some unchecked data in buffer 
				// and if it is not exceeding the filesize expected
				nwrite = buff_size - tot_check;
				
				if(nwrite > missing) {
					//writing on file only the byte expected
					nwrite = missing;
				}					
				
				//writing bytes from buffer to output file
				if(writen(outfile, buff + tot_check, nwrite) == -1) {
					//failure writing the file
					//closing connection
					close(sock);
					//closing file
					if(outfile != -1)
						Close(outfile);
					for (serverinfo = result; serverinfo != NULL; ) {
						tmp = serverinfo->ai_next;
						free(serverinfo);
						serverinfo = tmp;
					}
					err_sys ("(%s) error - writen() failed", prog_name);
				}
				else {
					
					//increment checked bytes counter
					tot_check+=nwrite;
					//decrement missing bytes counter by the number of received ones
					missing-=nwrite;
					
					//check if file reception is completed
					if(missing == 0) {
					
						//closing file and setting variable for the next request
						Close(outfile);
						n_requests--;
						stage = 0;
						outfile = -1;
						
						if(tot_check != buff_size) {
							//enough bytes received, file is being stored, but too many bytes in the receiving buffer
							//checking for -ERR
							if(checkErr(tot_check)) {
								if(buff_size-tot_check >= 6) {
									//-ERR has been received
									fprintf(stdout, "Received -ERR, server is closing connection...\n");
									
									//removing socket from the write-to set, client is waiting for the close
									if(FD_ISSET(sock, &writefds))
										FD_CLR(sock, &writefds);
									tot_check+=6;
								}
							}
							else {
								return -1;
							}
						}
						else {
							//all is correct, go one with next request
							fprintf(stdout, "============================================================================\n");
							fprintf(stdout, "Received file %s\nReceived file size [BYTES] %d\nReceived timestamp %d\n",
									arguments[filename_index], filesize, timestamp);
							fprintf(stdout, "============================================================================\n");
							flag_ext = 0;
						}
					}
					
					return tot_check;
				}
				break;
			
			default:
				//this is redundant, it should never happen
				fprintf(stdout, "BUG: Unexpected situation happened during received data elaboration.\n");
				//closing connection
				close(sock);
				//closing file
				if(outfile != -1)
					Close(outfile);
				for (serverinfo = result; serverinfo != NULL; ) {
					tmp = serverinfo->ai_next;
					free(serverinfo);
					serverinfo = tmp;
				}
				exit(1);
				break;
		}
	}
}


//shifts left by n position the content of buff between n and limit
void bufferShift(int n) {

	int i,j;
	
	for(i = n, j = 0; i < buff_size; i++, j++)
		buff[j] = buff[i];
	
	buff_size -= n;
}


//given the index of argv, save the path of the file into localpath
void getFileLocalPath(int n) {

	int size = strlen(arguments[n]), len = strlen(arguments[n]), i, j, flag = 1;
	
	//copying working directory as initial portion of local path
	memcpy(localpath, work_dir, strlen(work_dir));
	
	//retrieving filename from file path received as argument
	for(i = size-1; i >= 0 && flag; i--) {
		if(arguments[n][i] == '/')
			flag = 0;
	}

	i++;	
	
	if(!flag) {
		i++;
	}

	if(len-i > (MAX_PATH_LEN-strlen(work_dir)-1)) {
		fprintf(stdout, "File name is too long.\n");
						
		//closing connection
		close(sock);
		//closing file
		if(outfile != -1)
			Close(outfile);
		for (serverinfo = result; serverinfo != NULL; ) {
			tmp = serverinfo->ai_next;
			free(serverinfo);
			serverinfo = tmp;
		}

		exit(1);
	}
	
	//creating local path to store the file requested 
	for(j = strlen(work_dir); i < size; i++, j++) {
		localpath[j] = arguments[n][i];
	}
	localpath[j] = '\0';	
}
	

//check in the buffer, starting from offset, if the available received bytes correspond to at least the beginning of "-ERR|CR|LF" message
int checkErr(int offset) {

	char err_msg[7] = "-ERR\r\n";
	int k = buff_size - offset; //k stores the number of bytes that are available to be checked
	
	if(!memcmp(buff + offset, err_msg, (size_t)k)) {
		//if equal to -ERR|CR|LF
		return 1;
	}
	return 0;
}


//store the working directory in work_dir. Return 1 on success, 0 otherwise
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
	
