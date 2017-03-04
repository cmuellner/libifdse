CFLAGS+=-fPIC -Iext
LDFLAGS+=-shared

SRC=ifdhandler.c kerkey.c utils.c
OBJ=$(patsubst %.c,%.o, $(SRC))

all: libifdkerkey.so

libifdkerkey.so: $(OBJ)
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $(OBJ)

clean:
	$(RM) $(OBJ) libifdkerkey.so
