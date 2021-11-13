CC = clang
PIE = -no-pie
CCFLAGS = -Wno-microsoft-anon-tag

all:
	$(CC) -std=gnu11 -Wall -Wno-unused-function -fms-extensions $(CCFLAGS) $(PIE) -O3 -o bqn src/opt/single.c -lm

clean:
	rm -f ${bd}/*.o
	rm -f ${bd}/*.d
	rm -f bqn
