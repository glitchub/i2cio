// This software is released as-is into the public domain, as described at
// https://unlicense.org. Do whatever you like with it.
//
// See https://github.com/glitchub/i2cio for more information.

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define MAXMSGS I2C_RDWR_IOCTL_MAX_MSGS // max messages per transaction
#define MAXLEN 256                      // max message length

#define die(...) fprintf(stderr,__VA_ARGS__), exit(1)

#define usage() die("Usage:\n\
\n\
    i2cio [options] < commands > read_data\n\
\n\
Perform I2C transactions specified by commands read from stdin.\n\
\n\
A transaction may consist of up to %d messasges for the same device, separated\n\
by RESTART and performed atomically.\n\
\n\
The command text consists of single-character commands followed by some number\n\
of numeric fields.\n\
\n\
    D addr bus        - specify the 7-bit I2C address and bus number for\n\
                        subsequent R and W operations.\n\
    R length          - where length is 1-256, read specified number of bytes.\n\
    W byte [... byte] - where N's are numeric values 0-255, write specified\n\
                        bytes. Up to 256 bytes may be specified.\n\
    ;                 - end the current transaction, next R or W starts a new\n\
                        one.\n\
    # ...             - ignore text to end of line (aka a comment)\n\
\n\
Character case is not significant. Numeric values can be specified in\n\
decimal, hex, or octal (per strtoul()), followed by at least one whitespace\n\
character. Other whitespace is ignored.\n\
\n\
Example, to send command 0x06 to device 0x18 on bus 1 and read the two-byte\n\
result (i.e. to get the temperature from DDR4 SPD):\n\
\n\
    echo D 0x18 1 W 0x06 R 2 | i2cio\n\
\n\
By default, each R command will produce one line of space-separated hex\n\
values. Use the -d option to output decimal or -b option to output raw\n\
binary instead.\n\
\n\
If the -n option is given, then a dry run is performed. The specified I2C\n\
device will not be opened and read command results will report as 0x55's.\n\
", MAXMSGS)

bool dryrun = false, decimal = false, binary = false;

// Perform an I2C transaction and output received data
void transact(struct i2c_msg *msgs, int nmsgs, int i2cfd)
{
    struct i2c_rdwr_ioctl_data transaction = { .msgs = msgs, .nmsgs = nmsgs };
    if (!dryrun && ioctl(i2cfd, I2C_RDWR, &transaction) < 0) die ("I2C_RDWR ioctl failed: %s\n", strerror(errno));
    for (int n = 0; n < nmsgs; n++)
    {
        if (msgs[n].flags & I2C_M_RD)
        {
            if (dryrun) memset(msgs[n].buf, 0x55, msgs[n].len); // fake it if dryrun
            if (binary)
                // write raw data
                write(1, msgs[n].buf, msgs[n].len);
            else
            {
                // write formatted data
                for (int i = 0; i < msgs[n].len; i++) printf(decimal ? "%d " : "0x%.02X ", msgs[n].buf[i]);
                printf("\n");
            }
        }
    }
}

int main(int argc, char **argv)
{
    // command line switches
    while (*++argv)
    {
        char *o = *argv;
        if (*o != '-') usage();
        while (*++o) switch(*o)
        {
            case 'b': binary = true; break;
            case 'd': decimal = true; break;
            case 'n': dryrun = true; break;
            default: usage();
        }
    }

    unsigned int addr = 0;              // current I2C device address
    int i2cfd = -1;                     // current I2C bus file descriptor (/dev/i2c-X)

    struct i2c_msg msgs[MAXMSGS];       // The largest possible transaction

    for (int n = 0; n < MAXMSGS; n++)   // Each gets a buffer
        if (!(msgs[n].buf = malloc(MAXLEN)))
            die("malloc failed: %s\n", strerror(errno));

    int nmsgs = 0;                      // Number of messages in current transaction

    // parser state
    enum
    {
        INIT,       // expecting D
        IDLE,       // expecting D, R, W, ; or EOF
        READ,       // expecting read length
        WRITE,      // expecting byte to write
        WRITING,    // expecting byte, D, R, W, ; or EOF
        ADDR,       // expecting device address
        BUS         // expecting bus number
    } state = INIT;

    int lines = 1;
    while (1)
    {
        char *line = NULL; size_t size = 0;
        if (getline(&line, &size, stdin) < 0)
        {
            if (errno) die("Input error in line %d: %s\n", lines, strerror(errno));
            break;
        }

        int ofs = 0;
        while (1)
        {
            while (isspace(line[ofs])) ofs++;
            if (line[ofs] == 0 || line[ofs] == '#') break;

            switch (toupper(line[ofs]))
            {
                case 'R':
                    // add read message to transaction
                    switch (state)
                    {
                        case WRITING:
                            nmsgs++;
                            break;

                        case IDLE:
                            break;

                        default:
                        unexpected:
                            die("Unexpected '%c' at line %d offset %d\n", line[ofs], lines, ofs+1);
                    }
                    if (nmsgs >= MAXMSGS) die("Max %d messages per transaction\n",MAXMSGS);

                    // init next message
                    msgs[nmsgs].addr = addr;
                    msgs[nmsgs].flags = I2C_M_RD;

                    state = READ;
                    ofs++;
                    break;

                case 'W':
                    // add write message to transaction
                    switch (state)
                    {
                        case WRITING:
                            nmsgs++;
                            break;

                        case IDLE:
                            break;

                        default:
                            goto unexpected;
                    }
                    if (nmsgs >= MAXMSGS) die("Max %d messages per transaction\n",MAXMSGS);

                    // init next message
                    msgs[nmsgs].addr = addr;
                    msgs[nmsgs].flags = 0;
                    msgs[nmsgs].len = 0;

                    state = WRITE;
                    ofs++;
                    break;

                case ';':
                    // end current transaction and return idle
                    switch (state)
                    {
                        case WRITING:
                            nmsgs++;
                            transact(msgs, nmsgs, i2cfd);
                            nmsgs = 0;
                            break;

                        case INIT:
                            break; // sugar

                        case IDLE:
                            if (nmsgs)
                            {
                                transact(msgs, nmsgs, i2cfd);
                                nmsgs = 0;
                            }
                            break; // sugar

                        default:
                            goto unexpected;
                    }

                    state = IDLE;
                    ofs++;
                    break;

                case 'D':
                    // set device address and bus
                    switch (state)
                    {
                        case WRITING:
                            nmsgs++;
                            transact(msgs, nmsgs, i2cfd);
                            nmsgs = 0;
                            break;

                        case INIT:
                            break;

                        case IDLE:
                            if (nmsgs) transact(msgs, nmsgs, i2cfd);
                            break;

                        default:
                            goto unexpected;
                    }
                    state = ADDR;
                    ofs++;
                    break;

                case '0' ... '9':
                {
                    char *end;
                    unsigned int N = strtoul(line+ofs, &end, 0);

                    switch (state)
                    {
                        case ADDR:
                            if (N > 127) die("Device address exceeds 127 at line %d offset %d\n", lines, ofs+1);
                            addr = N;
                            state = BUS;
                            break;

                        case BUS:
                            if (!dryrun)
                            {
                                char name[32];
                                if (i2cfd > 0) close(i2cfd); // close existing
                                sprintf((char *)&name, "/dev/i2c-%d", N);
                                i2cfd = open(name, O_RDWR);
                                if (i2cfd < 0) die("Invalid bus at line %d offset %d (%s: %s)\n", lines, ofs+1, name, strerror(errno));
                            }
                            state = IDLE;
                            break;

                         case READ:
                            if (N < 1 || N > MAXLEN) die("Read length must be 1 to %d at line %d offset %d\n", MAXLEN, lines, ofs+1);
                            msgs[nmsgs++].len = N;
                            state = IDLE;
                            break;

                         case WRITE:
                         case WRITING:
                            if (N > 255) die("Write value exceeds 255 at line %d offset %d\n", lines, ofs+1);
                            msgs[nmsgs].buf[msgs[nmsgs].len++] = N;
                            if (msgs[nmsgs].len > MAXLEN) die("Write length exceeds %d at line %d offset %d\n", MAXLEN, lines, ofs+1);
                            state = WRITING;
                            break;

                         default:
                            goto unexpected;
                    }
                    ofs = (int)(end-line);
                    break;
                }

                default:
                    die ("Invalid '%c' line %d offset %d\n", line[ofs], lines, ofs+1);
            }
        }
        free(line);
        lines++;
    }

    switch (state)
    {
        case WRITING:
            nmsgs++;
            transact(msgs, nmsgs, i2cfd);
            break;

        case IDLE:
            if (nmsgs) transact(msgs, nmsgs, i2cfd);
            break;

        default:
            die("Unexpected end of input\n");
    }

    return 0;
}
