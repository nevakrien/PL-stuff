#ifndef IO_H
#define IO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

typedef ssize_t (*WriteFn)(
    const char* data,
    size_t len,
    void* writer
);

typedef struct Writer {
    void* stream;
    WriteFn write;
    SLICE(char) buf;
} Writer;

static inline ssize_t writer_write(Writer w, const char* data, size_t len) {
    if (!w.write) return 0;
    return w.write(data, len, w.stream);
}

static inline int wprintf(Writer w, const char* fmt, ...) {
    if (!w.write || !fmt) return 1;
    if (!w.buf.data || w.buf.len == 0) return 1;

    va_list args;
    va_start(args, fmt);

    int n = vsnprintf(w.buf.data, w.buf.len, fmt, args);

    va_end(args);

    if (n < 0) return n;

    /*
        vsnprintf returns the number of chars it wanted to write,
        excluding the null terminator.
        If n >= cap, the output was truncated.
    */

    ssize_t written = w.write(w.buf.data, w.buf.len, w.stream);

    if ((size_t)n >= w.buf.len || written!=n) {
        return -1;
    }


    return 0;
}

#endif // IO_H

