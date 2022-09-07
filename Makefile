DEBUG = 0
CFLAGS=-D DEBUG=$(DEBUG)
main:
	gcc main.c -g -o yash -lreadline $(CFLAGS)
clean:
	rm -rf yash