/****************************************************************************
 * Copyright (c) 2012, the Konoha project authors. All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ***************************************************************************/

#ifndef PLATFORM_POSIX_H_
#define PLATFORM_POSIX_H_
#ifndef MINIOKNOHA_H_
#error Do not include platform_posix.h without minikonoha.h.
#endif

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sys/time.h>
#include <syslog.h>
#include <dlfcn.h>
#include <sys/stat.h>
#ifdef HAVE_ICONV_H
#include <iconv.h>
#endif /* HAVE_ICONV_H */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PATH_MAX
#define K_PATHMAX PATH_MAX
#else
#define K_PATHMAX 256
#endif

#define kunused __attribute__((unused))
// -------------------------------------------------------------------------

static const char *getSystemCharset(void)
{
#if defined(K_USING_WINDOWS_)
	static char codepage[64];
	knh_snprintf(codepage, sizeof(codepage), "CP%d", (int)GetACP());
	return codepage;
#else
	return "UTF-8";
#endif
}

// -------------------------------------------------------------------------

static unsigned long long getTimeMilliSecond(void)
{
//#if defined(K_USING_WINDOWS)
//	DWORD tickCount = GetTickCount();
//	return (knh_int64_t)tickCount;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// -------------------------------------------------------------------------

#ifdef K_USE_PTHREAD
#include <pthread.h>

static int pthread_mutex_init_recursive(kmutex_t *mutex)
{
	pthread_mutexattr_t attr;
	bzero(&attr, sizeof(pthread_mutexattr_t));
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	return pthread_mutex_init((pthread_mutex_t*)mutex, &attr);
}

#else

static int pthread_mutex_destroy(kmutex_t *mutex)
{
	return 0;
}

static int pthread_mutex_init(kmutex_t *mutex, const kmutexattr_t *attr)
{
	return 0;
}

static int pthread_mutex_lock(kmutex_t *mutex)
{
	return 0;
}

static int pthread_mutex_trylock(kmutex_t *mutex)
{
	return 0;
}

static int pthread_mutex_unlock(kmutex_t *mutex)
{
	return 0;
}

static int pthread_mutex_init_recursive(kmutex_t *mutex)
{
	return 0;
}

#endif

// -------------------------------------------------------------------------

static const char* formatSystemPath(char *buf, size_t bufsiz, const char *path)
{
	return path;  // stub (in case of no conversion)
}

static const char* formatKonohaPath(char *buf, size_t bufsiz, const char *path)
{
	return path;  // stub (in case of no conversion)
}

static kbool_t isDir(const char *path)
{
	struct stat buf;
	char pathbuf[K_PATHMAX];
	if (stat(formatSystemPath(pathbuf, sizeof(pathbuf), path), &buf) == 0) {
		return S_ISDIR(buf.st_mode);
	}
	return false;
}

// -------------------------------------------------------------------------

typedef struct {
	char   *buffer;
	size_t  size;
	size_t  allocSize;
} SimpleBuffer;

static void SimpleBuffer_putc(SimpleBuffer *simpleBuffer, int ch)
{
	if(!(simpleBuffer->size < simpleBuffer->allocSize)) {
		simpleBuffer->allocSize *= 2;
		simpleBuffer->buffer = (char *)realloc(simpleBuffer->buffer, simpleBuffer->allocSize);
	}
	simpleBuffer->buffer[simpleBuffer->size] = ch;
	simpleBuffer->size += 1;
}

static kfileline_t readQuote(FILE *fp, kfileline_t line, SimpleBuffer *simpleBuffer, int quote)
{
	int ch, prev = quote;
	while((ch = fgetc(fp)) != EOF) {
		if(ch == '\r') continue;
		if(ch == '\n') line++;
		SimpleBuffer_putc(simpleBuffer, ch);
		if(ch == quote && prev != '\\') {
			return line;
		}
		prev = ch;
	}
	return line;
}

static kfileline_t readComment(FILE *fp, kfileline_t line, SimpleBuffer *simpleBuffer)
{
	int ch, prev = 0, level = 1;
	while((ch = fgetc(fp)) != EOF) {
		if(ch == '\r') continue;
		if(ch == '\n') line++;
		SimpleBuffer_putc(simpleBuffer, ch);
		if(prev == '*' && ch == '/') level--;
		if(prev == '/' && ch == '*') level++;
		if(level == 0) return line;
		prev = ch;
	}
	return line;
}

static kfileline_t readChunk(FILE *fp, kfileline_t line, SimpleBuffer *simpleBuffer)
{
	int ch;
	int prev = 0, isBLOCK = 0;
	while((ch = fgetc(fp)) != EOF) {
		if(ch == '\r') continue;
		if(ch == '\n') line++;
		SimpleBuffer_putc(simpleBuffer, ch);
		if(prev == '/' && ch == '*') {
			line = readComment(fp, line, simpleBuffer);
			continue;
		}
		if(ch == '\'' || ch == '"' || ch == '`') {
			line = readQuote(fp, line, simpleBuffer, ch);
			continue;
		}
		if(isBLOCK != 1 && prev == '\n' && ch == '\n') {
			break;
		}
		if(prev == '{') {
			isBLOCK = 1;
		}
		if(prev == '\n' && ch == '}') {
			isBLOCK = 0;
		}
		prev = ch;
	}
	return line;
}

static int isEmptyChunk(const char *t, size_t len)
{
	size_t i;
	for(i = 0; i < len; i++) {
		if(!isspace(t[i])) return false;
	}
	return true;
}

static int loadScript(const char *filePath, long uline, void *thunk, int (*evalFunc)(const char*, long, int *, void *))
{
	int isSuccessfullyLoading = false;
	if (isDir(filePath)) {
		return isSuccessfullyLoading;
	}
	FILE *fp = fopen(filePath, "r");
	if(fp != NULL) {
		SimpleBuffer simpleBuffer;
		simpleBuffer.buffer = (char*)malloc(K_PAGESIZE);
		simpleBuffer.allocSize = K_PAGESIZE;
		isSuccessfullyLoading = true;
		while(!feof(fp)) {
			kfileline_t rangeheadline = uline;
			kshort_t sline = (kshort_t)uline;
			bzero(simpleBuffer.buffer, simpleBuffer.allocSize);
			simpleBuffer.size = 0;
			uline = readChunk(fp, uline, &simpleBuffer);
			const char *script = (const char*)simpleBuffer.buffer;
			if(sline == 1 && simpleBuffer.size > 2 && script[0] == '#' && script[1] == '!') {
				// fall through this line
				simpleBuffer.size = 0;
				//TODO: do we increment uline??
			}
			if(!isEmptyChunk(script, simpleBuffer.size)) {
				int isBreak = false;
				isSuccessfullyLoading = evalFunc(script, rangeheadline, &isBreak, thunk);
				if(!isSuccessfullyLoading|| isBreak) {
					break;
				}
			}
		}
		fclose(fp);
	}
	return isSuccessfullyLoading;
}

static const char* shortFilePath(const char *path)
{
	char *p = (char *) strrchr(path, '/');
	return (p == NULL) ? path : (const char*)p+1;
}

static const char* shortText(const char *msg)
{
	return msg;
}

static const char *formatTransparentPath(char *buf, size_t bufsiz, const char *parentPath, const char *path)
{
	const char *p = strrchr(parentPath, '/');
	if(p != NULL && path[0] != '/') {
		size_t len = (p - parentPath) + 1;
		if(len < bufsiz) {
			memcpy(buf, parentPath, len);
			snprintf(buf + len, bufsiz - len, "%s", path);
			return (const char*)buf;
		}
	}
	return path;
}

#ifndef K_PREFIX
#define K_PREFIX  "/usr/local"
#endif

static const char* packname(const char *str)
{
	char *p = (char *) strrchr(str, '.');
	return (p == NULL) ? str : (const char*)p+1;
}

static const char* formatPackagePath(char *buf, size_t bufsiz, const char *packageName, const char *ext)
{
	FILE *fp = NULL;
	char *path = getenv("KONOHA_PACKAGEPATH");
	const char *local = "";
	if(path == NULL) {
		path = getenv("KONOHA_HOME");
		local = "/package";
	}
	if(path == NULL) {
		path = getenv("HOME");
		local = "/.minikonoha/package";
	}
	snprintf(buf, bufsiz, "%s%s/%s/%s%s", path, local, packageName, packname(packageName), ext);
#ifdef K_PREFIX
	fp = fopen(buf, "r");
	if(fp != NULL) {
		fclose(fp);
		return (const char*)buf;
	}
	snprintf(buf, bufsiz, K_PREFIX "/minikonoha/package" "/%s/%s%s", packageName, packname(packageName), ext);
#endif
	fp = fopen(buf, "r");
	if(fp != NULL) {
		fclose(fp);
		return (const char*)buf;
	}
	return NULL;
}

static KonohaPackageHandler *loadPackageHandler(const char *packageName)
{
	char pathbuf[256];
	formatPackagePath(pathbuf, sizeof(pathbuf), packageName, "_glue" K_OSDLLEXT);
	void *gluehdr = dlopen(pathbuf, RTLD_LAZY);
	//fprintf(stderr, "pathbuf=%s, gluehdr=%p", pathbuf, gluehdr);
	if(gluehdr != NULL) {
		char funcbuf[80];
		snprintf(funcbuf, sizeof(funcbuf), "%s_init", packname(packageName));
		PackageLoadFunc f = (PackageLoadFunc)dlsym(gluehdr, funcbuf);
		if(f != NULL) {
			return f();
		}
	}
	return NULL;
}

static const char* beginTag(kinfotag_t t)
{
	DBG_ASSERT(t <= NoneTag);
	static const char* tags[] = {
		"\x1b[1m\x1b[31m", /*CritTag*/
		"\x1b[1m\x1b[31m", /*ErrTag*/
		"\x1b[1m\x1b[31m", /*WarnTag*/
		"\x1b[1m", /*NoticeTag*/
		"\x1b[1m", /*InfoTag*/
		"", /*DebugTag*/
		"", /* NoneTag*/
	};
	return tags[(int)t];
}

static const char* endTag(kinfotag_t t)
{
	DBG_ASSERT(t <= NoneTag);
	static const char* tags[] = {
		"\x1b[0m", /*CritTag*/
		"\x1b[0m", /*ErrTag*/
		"\x1b[0m", /*WarnTag*/
		"\x1b[0m", /*NoticeTag*/
		"\x1b[0m", /*InfoTag*/
		"", /* Debug */
		"", /* NoneTag*/
	};
	return tags[(int)t];
}

static void debugPrintf(const char *file, const char *func, int line, const char *fmt, ...)
{
	va_list ap;
	va_start(ap , fmt);
	fflush(stdout);
	fprintf(stderr, "DEBUG(%s:%d) ", func, line);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

static void reportCaughtException(const char *exceptionName, const char *scriptName, int line, const char *optionalMessage)
{
	if(line != 0) {
		if(optionalMessage != NULL && optionalMessage[0] != 0) {
			fprintf(stderr, " ** (%s:%d) %s: %s\n", scriptName, line, exceptionName, optionalMessage);
		}
		else {
			fprintf(stderr, " ** (%s:%d) %s\n", scriptName, line, exceptionName);
		}
	}
	else {
		if(optionalMessage != NULL && optionalMessage[0] != 0) {
			fprintf(stderr, " ** %s: %s\n", exceptionName, optionalMessage);
		}
		else {
			fprintf(stderr, " ** %s\n", exceptionName);
		}
	}
}


static void NOP_debugPrintf(const char *file, const char *func, int line, const char *fmt, ...)
{
}

typedef uintptr_t (*ficonv_open)(const char *, const char *);
typedef size_t (*ficonv)(uintptr_t, char **, size_t *, char **, size_t *);
typedef int    (*ficonv_close)(uintptr_t);

static kunused uintptr_t dummy_iconv_open(const char *t, const char *f)
{
	return -1;
}
static kunused size_t dummy_iconv(uintptr_t i, char **t, size_t *ts, char **f, size_t *fs)
{
	return 0;
}
static kunused int dummy_iconv_close(uintptr_t i)
{
	return 0;
}

static void loadIconv(PlatformApiVar *plat)
{
#ifdef _ICONV_H
	plat->iconv_open_i    = (ficonv_open)iconv_open;
	plat->iconv_i         = (ficonv)iconv;
	plat->iconv_close_i   = (ficonv_close)iconv_close;
#else
	void *handler = dlopen("libiconv" K_OSDLLEXT, RTLD_LAZY);
	if(handler != NULL) {
		plat->iconv_open_i = (ficonv_open)dlsym(handler, "iconv_open");
		plat->iconv_i = (ficonv)dlsym(handler, "iconv");
		plat->iconv_close_i = (ficonv_close)dlsym(handler, "iconv_close");
	}
	else {
		plat->iconv_open_i = dummy_iconv_open;
		plat->iconv_i = dummy_iconv;
		plat->iconv_close_i = dummy_iconv_close;
	}
#endif /* _ICONV_H */
}

#define SMALLDATA 0
#define BIGDATA   1

static void monitorResource(int flag)
{
	return;
};

static PlatformApi* KonohaUtils_getDefaultPlatformApi(void)
{
	static PlatformApiVar plat = {};
	plat.name            = "shell";
	plat.stacksize       = K_PAGESIZE * 4;
	plat.getenv_i        =  (const char *(*)(const char*))getenv;
	plat.malloc_i        = malloc;
	plat.free_i          = free;
	plat.setjmp_i        = ksetjmp;
	plat.longjmp_i       = klongjmp;
	loadIconv(&plat);
	plat.getSystemCharset = getSystemCharset;
	plat.syslog_i        = syslog;
	plat.vsyslog_i       = vsyslog;
	plat.printf_i        = printf;
	plat.vprintf_i       = vprintf;
	plat.snprintf_i      = snprintf;  // avoid to use Xsnprintf
	plat.vsnprintf_i     = vsnprintf; // retreating..
	plat.qsort_i         = qsort;
	plat.exit_i          = exit;

	// mutex
	plat.pthread_mutex_init_i = pthread_mutex_init;
	plat.pthread_mutex_init_recursive = pthread_mutex_init_recursive;
	plat.pthread_mutex_lock_i = pthread_mutex_lock;
	plat.pthread_mutex_unlock_i = pthread_mutex_unlock;
	plat.pthread_mutex_trylock_i = pthread_mutex_trylock;
	plat.pthread_mutex_destroy_i = pthread_mutex_destroy;

	// high level
	plat.getTimeMilliSecond  = getTimeMilliSecond;
	plat.shortFilePath       = shortFilePath;
	plat.formatPackagePath   = formatPackagePath;
	plat.formatTransparentPath = formatTransparentPath;
	plat.formatKonohaPath = formatKonohaPath;
	plat.formatSystemPath = formatSystemPath;
	plat.loadPackageHandler  = loadPackageHandler;
	plat.loadScript          = loadScript;
	plat.beginTag            = beginTag;
	plat.endTag              = endTag;
	plat.shortText           = shortText;
	plat.reportCaughtException = reportCaughtException;
	plat.debugPrintf         = (!verbose_debug) ? NOP_debugPrintf : debugPrintf;
	plat.monitorResource     = monitorResource;
	return (PlatformApi*)(&plat);
}

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* PLATFORM_POSIX_H_ */
