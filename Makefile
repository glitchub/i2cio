CFLAGS = -Wall -Werror -Os -s

# Support for Centos 7 etc
# CFLAGS += -std=gnu99 -DI2C_RDWR_IOCTL_MAX_MSGS=16

i2cio: i2cio.c

clean:; rm -f i2cio
