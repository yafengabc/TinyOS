#ifndef TINYOS_USER_LIB_H
#define TINYOS_USER_LIB_H

#include "types.h"

/* system calls */
int  write(int fd, const void *buf, int len);
int  read(int fd, void *buf, int len);
int  exec(const char *path, char *const argv[], int argc);
int  open(const char *path);
int  create(const char *path);
int  close(int fd);
int  readdir(int index, char *name, int len);
int  getpid(void);
int  sleep(int ms);
int  uptime(void);
void *sbrk(int bytes);
void exit(int code) __attribute__((noreturn));

/* tiny libc */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    atoi(const char *s);
int    printf(const char *fmt, ...);
void   puts(const char *s);
void   putchar(int c);
int    putint(int v);
int    getline(char *buf, int max);
void  *malloc(size_t n);
void   free(void *p);

#endif
