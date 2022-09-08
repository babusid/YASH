main:
	gcc main.c -g -o yash -lreadline -D DEBUG=0
debug:
	gcc main.c -g3 -o yash -lreadline -D DEBUG=1
clean:
	rm -rf yash