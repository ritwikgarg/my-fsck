.PHONY: all
all: myfsck mkfs lsfs

myfsck: myfsck.c
	gcc -o myfsck myfsck.c

mkfs: mkfs.c
	gcc -o mkfs mkfs.c

lsfs: lsfs.c
	gcc -o lsfs lsfs.c

clean:
	rm -f myfsck mkfs lsfs