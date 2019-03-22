# c_unix_socketapi_data_transfer
This repository contains the implementation of a file transfer protocol over TCP.
It contains:
* client application, that can request a list of files to the server through GET requests.
* sequential server application, that can serve multiple client in a simulated concurrante way.
* concurent server application, that create a thread for each request received.
Server applications respond to the GET requests sending the files or the ERR response if file is not available.
Both servers are as robust as possible, with very low possibility of crashes, tolerant to buffers/network congestions and detecting unusual requests.

## Compiling
To compile respectively client, sequential server and concurrent server applications use the makefile provided using the following commands in the root directory of the project:
```console
user@machine:$ make client
user@machine:$ make server_sequential
user@machine:$ make server_concurrent
```

## Usage
To use client and servers applications follow the guideline of the following commands.
If the argument passed are wrong, the applications themselves will rovide the information required.
```console
user@machine:$ make client
user@machine:$ server_sequential <port_IPV4only> [ <port_IPV6> ]
user@machine:$ server_concurrent <port_IPV4only> [ <port_IPV6> ]
```
The applications must be executed with superuser permissions in order to be able to create and maintain the connections through the sockets.
