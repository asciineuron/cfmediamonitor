CFLAGS = -DDEBUG -g
LDFLAGS = -framework CoreServices

SRC = main.c mediamonitor.c 
OBJ = ${SRC:.c=.o}

# Should be automatic by make, to translate each .c to .o:
%.o : %.c
	$(CC) -c $(CFLAGS) $< -o $@

all: mediamonitor

mediamonitor: $(OBJ)
	$(CC) -o mediamonitor $(OBJ) $(LDFLAGS)

clean:
	rm -f mediamonitor $(OBJ)
