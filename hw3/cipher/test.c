#include <stdio.h>      // printf, perror
#include <stdlib.h>     // exit
#include <string.h>     // strlen, memset
#include <unistd.h>     // read, write, close
#include <fcntl.h>      // open, O_*
#include <sys/ioctl.h>  // ioctl
#include "cipher.h"     // our driver-specific IOCTL macros

int main(void)
{
    int fd;
    int ret;
    int remain;

    /* 1) Open /dev/cipher0 in write-only mode */
    fd = open("/dev/cipher0", O_WRONLY);
    if (fd < 0) {
        perror("open(/dev/cipher0, O_WRONLY)");
        return 1;
    }

    /* 2) Set a new key via IOCTL */
    {
        char newkey[64] = "MY-SECRET-KEY";
        ret = ioctl(fd, CIPHER_IOCSKEY, newkey);
        if (ret < 0) {
            perror("ioctl(CIPHER_IOCSKEY)");
            close(fd);
            return 1;
        }
    }

    /* 3) Write some data */
    {
        const char *msg = "Testing new key";
        ssize_t w = write(fd, msg, strlen(msg));
        if (w < 0) {
            perror("write");
        } else {
            printf("Wrote %zd bytes.\n", w);
        }
    }

    /* 4) Query how many bytes remain to write (out of 8192) */
    remain = 0;
    ret = ioctl(fd, CIPHER_IOCQREM, &remain);
    if (ret < 0)
        perror("ioctl(CIPHER_IOCQREM)");
    else
        printf("Bytes remaining to write: %d\n", remain);

    close(fd);


    /* 5) Read back what we wrote */
    fd = open("/dev/cipher0", O_RDONLY);
    if (fd < 0) {
        perror("open(/dev/cipher0, O_RDONLY)");
        return 1;
    }

    {
        char buf[100] = {0};
        ret = read(fd, buf, sizeof(buf)-1);
        if (ret < 0) {
            perror("read");
        } else {
            printf("Read back %d bytes: \"%s\"\n", ret, buf);
        }
    }
    close(fd);

    /* 6) Call "clear" (reset) via IOCTL, just as a final test */
    fd = open("/dev/cipher0", O_WRONLY);
    if (fd >= 0) {
        ret = ioctl(fd, CIPHER_IOCCLR);
        if (ret < 0) {
            perror("ioctl(CIPHER_IOCCLR)");
        } else {
            printf("Device cleared.\n");
        }
        close(fd);
    }

    return 0;
}
