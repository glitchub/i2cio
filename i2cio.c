#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <ctype.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define MAXMSGS 32 // max messages per transaction
#define MAXLEN 256 // max message length

#define die(...) fprintf(stderr,__VA_ARGS__), exit(1)

#define usage() die("Usage:\n\
\n\
    i2cio [-d] < command > read_data\n\
\n\
Perform I2C transactions specified by the command string. Note in this context\n\
an I2C transaction is two or more messages for the same device, separated by\n\
RESTART and transmitted atomically.\n\
\n\
The command text consists of single-character commands followed by some number\n\
of numeric fields.\n\
\n\
    D addr bus        - specify I2C address and bus number for all subsequent R and W operations.\n\
    R length          - where length is 1-128, read specified number of bytes.\n\
    W byte [... byte] - where N's are numeric values 0-255, write specified bytes.\n\
    ;                 - end the current transaction, next R or W starts a new one.\n\
    # ...             - ignore text to end of line (aka a comment)\n\
\n\
Up to 32 R/W messages can be supported in a single transaction. \n\
\n\
Transactions are atomic, therefore not actually performed until ';' (or 'D' or\n\
EOF, if encountered after R or W).\n\
\n\
Each R command will produce its own line of output.\n\
\n\
Character case and line breaks are insignificant.\n\
\n\
Numbers can be specified as hex, decimal, or octal (per strtoul).\n\
\n\
-d option enables dry run, bus device will not actually be opened.\n\
")

// malloc or die
void *alloc(int n)
{
    void *p=malloc(n);
    if (!p) die("malloc failed: %s\n", strerror(errno));
    return p;
}

bool dryrun = 0;

// Perform I2C transaction ond print received data (or die)
void transact(struct i2c_msg *msgs, int nmsgs, int i2cfd)
{
    struct i2c_rdwr_ioctl_data transaction = { .msgs=msgs, .nmsgs=nmsgs };
    if (!dryrun && ioctl(i2cfd, I2C_RDWR, &transaction)) die ("I2C_RDWR ioctl failed: %s\n", strerror(errno));
    for (int n=0; n < nmsgs; n++)
    {
        if (msgs[n].flags & I2C_M_RD)
        {
            for (int i=0; i < msgs[n].len; i++) printf("0x%02X ",msgs[n].buf[i]);
            printf("\n");
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc > 1) 
    {
        if (strcmp(argv[1], "-d")) usage();
        dryrun=true;
    }   
    
    uint16_t addr;                      // current I2C device address
    int i2cfd = -1;                     // current I2C bus file descriptor (/dev/i2c-X)

    struct i2c_msg msgs[MAXMSGS];       // The largest possible transaction
    for (int n=0; n < MAXMSGS; n++)     // Each one gets a buffer 
        msgs[n].buf=alloc(MAXLEN);
    
    int nmsgs=0;                        // Number of messages in current transaction

    // parse state
    enum { INIT,                        // expecting D
           IDLE,                        // expecting D, R, W, ; or EOF
           READ,                        // expecting read length
           WRITE,                       // expecting byte to write
           WRITING,                     // expecting byte, D, R, W, ; or EOF
           ADDR,                        // expecting device address
           BUS                          // expecting bus number
           };
    int parser=INIT;                    // Initially, expect 'D'

    int lines=1;
    while (1)
    {
        char *line=NULL; size_t size=0;
        if (getline(&line, &size, stdin) < 0) 
        {
            if (errno) die("Input error in line %d: %s\n", lines, strerror(errno));
            break;
        }    

        int ofs=0;
        while (1)
        {
            while (isspace(line[ofs])) ofs++;
            if (line[ofs] == 0 || line[ofs] == '#') break;

            switch (toupper(line[ofs]))
            {
                case 'R':
                    // add read message to transaction
                    switch(parser)
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
                    
                    parser = READ;
                    ofs++;
                    break;

                case 'W':
                    // add write message to transaction
                    switch(parser)
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

                    parser = WRITE;
                    ofs++;
                    break;

                case ';':
                    // end current transaction and return idle
                    switch(parser)
                    {
                        case WRITING:
                            nmsgs++;
                            transact(msgs, nmsgs, i2cfd);
                            nmsgs=0;
                            break;

                        case INIT:
                            break; // sugar

                        case IDLE:
                            if (nmsgs)
                            {
                                transact(msgs, nmsgs, i2cfd);
                                nmsgs=0;
                            }
                            break; // sugar

                        default:
                            goto unexpected;
                    }

                    parser=IDLE;
                    ofs++;
                    break;

                case 'D':
                    // set device address and bus
                    switch(parser)
                    {
                        case WRITING:
                            nmsgs++;
                            transact(msgs, nmsgs, i2cfd);
                            nmsgs=0;
                            break;

                        case INIT:
                            break;

                        case IDLE:
                            if (nmsgs) transact(msgs, nmsgs, i2cfd);
                            break;

                        default:
                            goto unexpected;
                    }
                    parser = ADDR;
                    ofs++;
                    break;

                case '0' ... '9':
                {
                    char *end;
                    uint16_t N = strtoul(line+ofs, &end, 0);

                    switch(parser)
                    {
                        case ADDR:
                            if (N > 127) die("Device address exceeds 127 at line %d offset %d\n", lines, ofs+1);
                            addr=N;
                            parser = BUS;
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
                            parser = IDLE;
                            break;

                         case READ:
                            if (N == 0) die("Read length must be at least 1 at line %d offset %d\n", lines, ofs+1);
                            if (N > MAXLEN) die("Read length exceeds %d at line %d offset %d\n", MAXLEN, lines, ofs+1);
                            msgs[nmsgs++].len=N;
                            parser = IDLE;
                            break;

                         case WRITE:
                         case WRITING:
                            if (N > 255) die("Write value exceeds 255 at line %d offset %d\n", lines, ofs+1);
                            msgs[nmsgs].buf[msgs[nmsgs].len++] = N;
                            if (msgs[nmsgs].len > MAXLEN) die("Write length exceeds %d at line %d offset %d\n", MAXLEN, lines, ofs+1);
                            parser = WRITING;
                            break;

                         default:
                            goto unexpected;
                    }
                    ofs=(int)(end-line);
                    break;
                }

                default:
                    die ("Invalid '%c' line %d offset %d\n", line[ofs], lines, ofs+1);
            }
        }
        free(line);
        lines++;
    }

    switch(parser)
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
