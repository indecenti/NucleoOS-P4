// WString.h — a small Arduino String (heap-backed) and the F() string helper. Clean-room, covers
// the common calls (construct from text/numbers, +=, +, compare, c_str, length, charAt, indexOf,
// substring, toInt). Part of ports/arduboy.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

class __FlashStringHelper;
#define F(string_literal) (reinterpret_cast<const __FlashStringHelper *>(PSTR(string_literal)))

class String {
public:
    String(const char *s = "") { set(s ? s : "", s ? strlen(s) : 0); }
    String(const __FlashStringHelper *s) : String(reinterpret_cast<const char *>(s)) {}
    String(const String &o) { set(o.buf_, o.len_); }
    String(char c) { char b[2] = {c, 0}; set(b, 1); }
    String(unsigned char v, unsigned char base = 10) { num((unsigned long)v, base, false); }
    String(int v, unsigned char base = 10) { num((long)v, base, true); }
    String(unsigned int v, unsigned char base = 10) { num((unsigned long)v, base, false); }
    String(long v, unsigned char base = 10) { num(v, base, true); }
    String(unsigned long v, unsigned char base = 10) { num((long)v, base, false); }
    String(double v, unsigned char decimals = 2);
    ~String() { free(buf_); }
    String &operator=(const String &o) { if (this != &o) { free(buf_); set(o.buf_, o.len_); } return *this; }
    String &operator=(const char *s) { free(buf_); set(s ? s : "", s ? strlen(s) : 0); return *this; }
    unsigned int length() const { return len_; }
    const char *c_str() const { return buf_; }
    char charAt(unsigned int i) const { return i < len_ ? buf_[i] : 0; }
    char operator[](unsigned int i) const { return charAt(i); }
    char &operator[](unsigned int i) { static char dummy; return i < len_ ? buf_[i] : dummy; }
    void setCharAt(unsigned int i, char c) { if (i < len_) buf_[i] = c; }
    bool concat(const char *s) { return append(s, strlen(s)); }
    bool concat(const String &s) { return append(s.buf_, s.len_); }
    bool concat(char c) { return append(&c, 1); }
    bool concat(int v) { return concat(String(v)); }
    bool concat(unsigned int v) { return concat(String(v)); }
    bool concat(long v) { return concat(String(v)); }
    bool concat(unsigned long v) { return concat(String(v)); }
    bool concat(unsigned char v) { return concat(String(v)); }
    template <class T> String &operator+=(const T &v) { concat(v); return *this; }
    bool equals(const String &o) const { return len_ == o.len_ && !memcmp(buf_, o.buf_, len_); }
    bool equals(const char *s) const { return !strcmp(buf_, s ? s : ""); }
    bool operator==(const String &o) const { return equals(o); }
    bool operator==(const char *s) const { return equals(s); }
    bool operator!=(const String &o) const { return !equals(o); }
    bool operator!=(const char *s) const { return !equals(s); }
    int compareTo(const String &o) const { return strcmp(buf_, o.buf_); }
    int indexOf(char c, unsigned int from = 0) const {
        for (unsigned int i = from; i < len_; i++) if (buf_[i] == c) return (int)i;
        return -1;
    }
    String substring(unsigned int a, unsigned int b = 0xFFFFFFFFu) const {
        if (b > len_) b = len_;
        if (a > b) a = b;
        String r;
        free(r.buf_);
        r.set(buf_ + a, b - a);
        return r;
    }
    long toInt() const { return atol(buf_); }
    float toFloat() const { return (float)atof(buf_); }
    void toUpperCase() { for (unsigned int i = 0; i < len_; i++) if (buf_[i] >= 'a' && buf_[i] <= 'z') buf_[i] -= 32; }
    void toLowerCase() { for (unsigned int i = 0; i < len_; i++) if (buf_[i] >= 'A' && buf_[i] <= 'Z') buf_[i] += 32; }
    void toCharArray(char *out, unsigned int n) const { if (!n) return; unsigned int k = len_ < n - 1 ? len_ : n - 1; memcpy(out, buf_, k); out[k] = 0; }
    void getBytes(unsigned char *out, unsigned int n) const { toCharArray((char *)out, n); }
    bool reserve(unsigned int) { return true; }
    void trim() {
        unsigned int a = 0, b = len_;
        while (a < b && (buf_[a] == ' ' || buf_[a] == '\t' || buf_[a] == '\n' || buf_[a] == '\r')) a++;
        while (b > a && (buf_[b - 1] == ' ' || buf_[b - 1] == '\t' || buf_[b - 1] == '\n' || buf_[b - 1] == '\r')) b--;
        memmove(buf_, buf_ + a, b - a);
        len_ = b - a;
        buf_[len_] = 0;
    }

private:
    char *buf_ = nullptr;
    unsigned int len_ = 0;
    void set(const char *s, size_t n) {
        buf_ = (char *)malloc(n + 1);
        if (!buf_) { len_ = 0; return; }
        memcpy(buf_, s, n);
        buf_[n] = 0;
        len_ = (unsigned int)n;
    }
    bool append(const char *s, size_t n) {
        char *nb = (char *)realloc(buf_, len_ + n + 1);
        if (!nb) return false;
        buf_ = nb;
        memcpy(buf_ + len_, s, n);
        len_ += (unsigned int)n;
        buf_[len_] = 0;
        return true;
    }
    void num(long v, unsigned char base, bool sign) {
        char b[40];
        char *p = b + sizeof b - 1;
        *p = 0;
        unsigned long u = (sign && v < 0) ? (unsigned long)(-v) : (unsigned long)v;
        if (base < 2) base = 10;
        do { const int d = (int)(u % base); *--p = (char)(d < 10 ? '0' + d : 'A' + d - 10); u /= base; } while (u);
        if (sign && v < 0) *--p = '-';
        set(p, strlen(p));
    }
};
inline String operator+(const String &a, const String &b) { String r(a); r.concat(b); return r; }
inline String operator+(const String &a, const char *b) { String r(a); r.concat(b); return r; }
inline String operator+(const char *a, const String &b) { String r(a); r.concat(b); return r; }
inline String operator+(const String &a, char b) { String r(a); r.concat(b); return r; }
inline String operator+(const String &a, int b) { String r(a); r.concat(b); return r; }
inline String operator+(const String &a, long b) { String r(a); r.concat(b); return r; }
inline String operator+(const String &a, unsigned int b) { String r(a); r.concat(b); return r; }
inline String operator+(const String &a, unsigned long b) { String r(a); r.concat(b); return r; }
