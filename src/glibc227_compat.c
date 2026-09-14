/* glibc227_compat.c -- brings the binary down to the MOD Dwarf's glibc (2.27).
 *
 * A DANGER MEASURED THE HARD WAY: these functions carry glibc function names.
 * Exported, they supersede the HOST process's own -- mod-jackd, lilv, the other
 * plugins -- which then end up running MY implementations, written for my use
 * alone. Hence the audio server dying when the effect was added. So every one
 * of them is hidden: usable inside this binary, invisible outside it.
 */
#define COMPAT_LOCAL __attribute__((visibility("hidden")))

/* (original header)
 * Every function here REPLACES a symbol that is too recent: the linker resolves
 * the reference against our local definition, so no versioned UND entry is
 * left. NEVER call here the function being defined (endless recursion): go
 * either through a direct system call, or through an asm alias to the real
 * symbol.
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE

__attribute__((used)) static const char build_tag[] =
	"NAMTEST_BUILD67_AARCH64_20260914";
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>

/* --- the stat family (GLIBC_2.33) ----------------------------------------
 * On aarch64 glibc's struct stat is the kernel's, and the only system call
 * available is newfstatat. glibc is therefore bypassed entirely, which settles
 * the argument about which version of __xstat to use.
 */
COMPAT_LOCAL int stat(const char *path, struct stat *buf)
{
	return syscall(SYS_newfstatat, AT_FDCWD, path, buf, 0);
}

COMPAT_LOCAL int lstat(const char *path, struct stat *buf)
{
	return syscall(SYS_newfstatat, AT_FDCWD, path, buf, AT_SYMLINK_NOFOLLOW);
}

COMPAT_LOCAL int fstat(int fd, struct stat *buf)
{
	return syscall(SYS_newfstatat, fd, "", buf, AT_EMPTY_PATH);
}

COMPAT_LOCAL int fstat64(int fd, struct stat64 *buf)
{
	return syscall(SYS_newfstatat, fd, "", buf, AT_EMPTY_PATH);
}

COMPAT_LOCAL int stat64(const char *path, struct stat64 *buf)
{
	return syscall(SYS_newfstatat, AT_FDCWD, path, buf, 0);
}

COMPAT_LOCAL int lstat64(const char *path, struct stat64 *buf)
{
	return syscall(SYS_newfstatat, AT_FDCWD, path, buf, AT_SYMLINK_NOFOLLOW);
}

/* --- __libc_single_threaded (GLIBC_2.32) ---------------------------------
 * Data, not a function. 0 = "assume several threads": the conservative choice,
 * which makes libstdc++ take its locked paths.
 */
COMPAT_LOCAL char __libc_single_threaded = 0;

/* --- pthread_once (GLIBC_2.34) -------------------------------------------
 * In 2.27 it lives in libpthread, not in libc, and a modern toolchain cannot
 * link against it cleanly. Reimplemented with atomics.
 */
COMPAT_LOCAL int pthread_once(int *control, void (*init_routine)(void))
{
	int expected = 0;
	if (__atomic_load_n(control, __ATOMIC_ACQUIRE) == 2)
		return 0;
	if (__atomic_compare_exchange_n(control, &expected, 1, 0,
					__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		init_routine();
		__atomic_store_n(control, 2, __ATOMIC_RELEASE);
		return 0;
	}
	while (__atomic_load_n(control, __ATOMIC_ACQUIRE) != 2)
		usleep(100);
	return 0;
}


/* --- thread-local keys (GLIBC_2.34) --------------------------------------
 * Static libstdc++ references __pthread_key_create to find out whether the
 * program is multi-threaded. In 2.27 those functions live in libpthread, not
 * in libc, and a modern toolchain can no longer link against them: they are
 * reimplemented entirely, which avoids EVERY versioned reference (no patching
 * of the ELF afterwards -- an attempt of that kind crashed the loader).
 * Accepted limit: destructors are not called when a thread ends.
 */
#define COMPAT_MAX_KEYS 64
static void (*compat_key_dtor[COMPAT_MAX_KEYS])(void *);
static int compat_key_used[COMPAT_MAX_KEYS];
static __thread void *compat_key_val[COMPAT_MAX_KEYS];

COMPAT_LOCAL int __pthread_key_create(pthread_key_t *key, void (*dtor)(void *));

COMPAT_LOCAL int __pthread_key_create(pthread_key_t *key, void (*dtor)(void *))
{
	int i, expected;
	for (i = 0; i < COMPAT_MAX_KEYS; i++) {
		expected = 0;
		if (__atomic_compare_exchange_n(&compat_key_used[i], &expected, 1, 0,
						__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			compat_key_dtor[i] = dtor;
			*key = (pthread_key_t)i;
			return 0;
		}
	}
	return EAGAIN;
}

COMPAT_LOCAL int pthread_key_create(pthread_key_t *key, void (*dtor)(void *))
{
	return __pthread_key_create(key, dtor);
}

COMPAT_LOCAL int pthread_key_delete(pthread_key_t key)
{
	if ((unsigned)key >= COMPAT_MAX_KEYS)
		return EINVAL;
	compat_key_dtor[key] = 0;
	__atomic_store_n(&compat_key_used[key], 0, __ATOMIC_RELEASE);
	return 0;
}

COMPAT_LOCAL void *pthread_getspecific(pthread_key_t key)
{
	if ((unsigned)key >= COMPAT_MAX_KEYS)
		return 0;
	return compat_key_val[key];
}

COMPAT_LOCAL int pthread_setspecific(pthread_key_t key, const void *value)
{
	if ((unsigned)key >= COMPAT_MAX_KEYS)
		return EINVAL;
	compat_key_val[key] = (void *)value;
	return 0;
}

/* --- _dl_find_object (GLIBC_2.35) ----------------------------------------
 * Used by libgcc's exception unwinder as a SHORTCUT. Returning -1 ("not
 * found") makes it fall back on dl_iterate_phdr, which has always been there.
 * Exceptions therefore keep working, a little more slowly.
 */
COMPAT_LOCAL int _dl_find_object(void *address, void *result)
{
	(void)address;
	(void)result;
	return -1;
}

/* --- arc4random (GLIBC_2.36) --------------------------------------------- */
COMPAT_LOCAL uint32_t arc4random(void)
{
	uint32_t v = 0;
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		ssize_t n = read(fd, &v, sizeof(v));
		close(fd);
		if (n == (ssize_t)sizeof(v))
			return v;
	}
	return (uint32_t)random();
}

COMPAT_LOCAL void arc4random_buf(void *buf, size_t n)
{
	unsigned char *p = (unsigned char *)buf;
	size_t i;
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		ssize_t got = read(fd, buf, n);
		close(fd);
		if (got == (ssize_t)n)
			return;
	}
	for (i = 0; i < n; i++)
		p[i] = (unsigned char)random();
}


/* --- getentropy / getrandom ---------------------------------------------
 * The version exists on MOD but the symbol is not exported by their libc. We
 * go through the getrandom system call (kernel >= 3.17; the Dwarf runs 6.1),
 * falling back on /dev/urandom.
 */
COMPAT_LOCAL int getentropy(void *buf, size_t len)
{
	unsigned char *p = (unsigned char *)buf;
	size_t done = 0;
	int fd;

	if (len > 256) {
		errno = EIO;
		return -1;
	}
	while (done < len) {
		long n = syscall(SYS_getrandom, p + done, len - done, 0);
		if (n <= 0)
			break;
		done += (size_t)n;
	}
	if (done == len)
		return 0;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;
	while (done < len) {
		ssize_t n = read(fd, p + done, len - done);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		done += (size_t)n;
	}
	close(fd);
	return 0;
}

COMPAT_LOCAL ssize_t getrandom(void *buf, size_t len, unsigned int flags)
{
	return syscall(SYS_getrandom, buf, len, flags);
}

/* --- the __isoc23_strtol family (GLIBC_2.38) -----------------------------
 * Recent headers rename strtol to __isoc23_strtol. That name is given back to
 * the real strtol, reached through an asm alias so the macro does not send us
 * back to ourselves.
 */
extern long real_strtol(const char *, char **, int) __asm__("strtol");
extern unsigned long real_strtoul(const char *, char **, int) __asm__("strtoul");
extern long long real_strtoll(const char *, char **, int) __asm__("strtoll");
extern unsigned long long real_strtoull(const char *, char **, int) __asm__("strtoull");

COMPAT_LOCAL long __isoc23_strtol(const char *n, char **e, int b) { return real_strtol(n, e, b); }
COMPAT_LOCAL unsigned long __isoc23_strtoul(const char *n, char **e, int b) { return real_strtoul(n, e, b); }
COMPAT_LOCAL long long __isoc23_strtoll(const char *n, char **e, int b) { return real_strtoll(n, e, b); }
COMPAT_LOCAL unsigned long long __isoc23_strtoull(const char *n, char **e, int b) { return real_strtoull(n, e, b); }

/* The trace has to read the REAL mark: the "build" line of the state file used
   to be frozen text, which had not followed for several versions and made it
   look as though an old build was running. */
const char* nam_build_tag(void)
{
	return build_tag;
}
