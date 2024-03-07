flit: flit.c
	$(CC) flit.c -o flit -Wall -Wextra -pedantic -std=c99

clean:
	rm -f flit