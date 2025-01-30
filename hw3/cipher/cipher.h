#ifndef _CIPHER_H_
#define _CIPHER_H_

/* Gives us access to ioctl macros _IO and friends below. */
#include <linux/ioctl.h>


#define CIPHER_NR_DEVS	8

/* Use 222 as magic number */
#define CIPHER_IOC_MAGIC  222
/* Please use a different 8-bit number in your code */


/*
 * S means "Set" through a ptr,
 * T means "Tell" directly with the argument value
 * G means "Get": reply by setting through a pointer
 * Q means "Query": response is on the return value
 * X means "eXchange": switch G and S atomically
 * H means "sHift": switch T and Q atomically
 */
#define CIPHER_IOCCLR    _IO(CIPHER_IOC_MAGIC,0)
#define CIPHER_IOCSKEY _IOW(CIPHER_IOC_MAGIC, 1, char)
#define _IOR(type, nr, size) _IOC(_IOC_READ, (type), (nr), (_IOC_TYPECHECK(size)))
#define CIPHER_IOC_MAXNR      2
#define CIPHER_IOCQREM _IOR(CIPHER_IOC_MAGIC, 2, int)


#endif /* _CIPHER_H_ */
