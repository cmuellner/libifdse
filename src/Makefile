CFLAGS+=-fPIC -Iext
LDFLAGS+=-shared

SRC=\
	halgpio.c \
	halgpio_kernel.c \
	halgpio_sysfs.c \
	hali2c.c \
	hali2c_kernel.c \
	halse.c \
	halse_kerkey.c \
	halse_se05x.c \
	ifdhandler.c \

OBJ=$(patsubst %.c,%.o, $(SRC))

all: libifdkerkey.so

libifdkerkey.so: $(OBJ)
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $(OBJ)

clean:
	$(RM) $(OBJ) libifdkerkey.so
