flit: flit.c
	$(CC) flit.c -o flit -Wall -Wextra -O3 -pedantic -std=c99

clean:
	rm -f flit