client: ./src/client/client_main.c ./lib/sockwrap.c ./lib/errlib.c ./lib/sockwrap.h ./lib/errlib.h
	gcc ./src/client/client_main.c ./lib/sockwrap.c ./lib/errlib.c -o client

server_sequential: ./src/server_sequential/server_sequential_main.c ./lib/sockwrap.c ./lib/errlib.c ./lib/sockwrap.h ./lib/errlib.h
	gcc ./src/server_sequential/server_sequential_main.c ./lib/sockwrap.c ./lib/errlib.c -o server_sequential
	
server_concurrent: ./src/server_concurrent/server_concurrent_main.c ./lib/sockwrap.c ./lib/errlib.c ./lib/sockwrap.h ./lib/errlib.h
	gcc ./src/server_concurrent/server_concurrent_main.c ./lib/sockwrap.c ./lib/errlib.c -o server_concurrent

clean: rm ./*.o
