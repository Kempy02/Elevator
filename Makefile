# define compiler and flags as variables
CC = gcc
# CFLAGS = -Wall -pthread

# Define targets
all: car #controller call internal safety

# Define individual target dependencies
car: car.c
	$(CC) -o car car.c

# controller: controller.c
#     $(CC) $(CFLAGS) -o controller controller.c

# call: call.c
#     $(CC) $(CFLAGS) -o call call.c

# internal: internal.c
#     $(CC) $(CFLAGS) -o internal internal.c

# safety: safety.c
#     $(CC) $(CFLAGS) -o safety safety.c

# Define clean target
# clean:
    # rm -f car controller call internal safety

# define phony targets
# .PHONY: all clean car controller call internal safety

# End of Makefile