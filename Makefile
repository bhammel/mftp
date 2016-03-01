CC          = gcc
EXECUTABLES = mftp
CFLAGS      = -g -Wall
LIB         = -lpthread -lm
USER_OBJS   = mftp.o

$(EXECUTABLES): $(USER_OBJS)
	$(CC) $(CFLAGS) $(USER_OBJS) $(LIB) -o $@

.c.o:
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(USER_OBJS) $(EXECUTABLES)
