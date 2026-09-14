/* glibc227_compat.c — ramène le binaire au niveau de la glibc du MOD Dwarf (2.27).
 *
 * DANGER MESURÉ : ces fonctions portent les noms de fonctions de la glibc.
 * Exportées, elles supplantent celles du processus HÔTE — mod-jackd, lilv, les
 * autres plugins — qui se retrouvent à utiliser MES implantations, écrites pour
 * mon seul usage. D'où le plantage du serveur audio à l'ajout de l'effet.
 * Elles sont donc TOUTES cachées : utilisables dans ce binaire, invisibles
 * au dehors.
 */
#define COMPAT_LOCAL __attribute__((visibility("hidden")))

/* (en-tête d'origine)
 * glibc227_compat.c — ramène le binaire au niveau de la glibc du MOD Dwarf (2.27).
 * Chaque fonction ici REMPLACE un symbole trop récent : le linker resout la
 * reference sur notre definition locale, donc plus aucune entree UND versionnee.
 * Ne JAMAIS appeler ici la fonction qu'on definit (recursion infinie) : on passe
 * soit par un appel systeme direct, soit par un alias asm vers le vrai symbole.
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE

__attribute__((used)) static const char build_tag[] =
	"NAMTEST_BUILD66_AARCH64_20260914";
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>

/* --- famille stat (GLIBC_2.33) -------------------------------------------
 * Sur aarch64 la struct stat de la glibc est celle du noyau, et le seul appel
 * systeme disponible est newfstatat. On court-circuite donc entierement la
 * glibc, ce qui evite le debat sur la version de __xstat.
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
 * Donnee, pas fonction. 0 = « suppose plusieurs fils » : c'est le choix
 * conservateur, la libstdc++ prend alors ses chemins verrouilles.
 */
COMPAT_LOCAL char __libc_single_threaded = 0;

/* --- pthread_once (GLIBC_2.34) -------------------------------------------
 * En 2.27 il vit dans libpthread, pas dans libc, et on ne peut pas s'y lier
 * proprement depuis une chaine moderne. Reimplantation par atomiques.
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


/* --- cles locales au fil (GLIBC_2.34) ------------------------------------
 * La libstdc++ statique reference __pthread_key_create pour savoir si le
 * programme est multi-fils. En 2.27 ces fonctions vivent dans libpthread, pas
 * dans libc, et une chaine moderne ne sait plus s'y lier : on les reimplante
 * entierement, ce qui evite TOUTE reference versionnee (aucune retouche de
 * l'ELF apres coup -- une tentative de ce genre a fait planter le chargeur).
 * Limite assumee : les destructeurs ne sont pas appeles a la fin d'un fil.
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
 * Utilise par le derouleur d'exceptions de libgcc comme RACCOURCI. Renvoyer
 * -1 (« pas trouve ») le fait retomber sur dl_iterate_phdr, present depuis
 * toujours. Les exceptions continuent donc de fonctionner, un peu plus lentement.
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
 * La version existe chez MOD mais le symbole n'est pas exporte par leur libc.
 * On passe par l'appel systeme getrandom (noyau >= 3.17 ; le Dwarf est en 6.1),
 * avec repli sur /dev/urandom.
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

/* --- famille __isoc23_strtol (GLIBC_2.38) --------------------------------
 * Les en-tetes recentes renomment strtol en __isoc23_strtol. On redonne ce nom
 * au vrai strtol, atteint par un alias asm pour eviter que la macro ne nous
 * renvoie sur nous-memes.
 */
extern long real_strtol(const char *, char **, int) __asm__("strtol");
extern unsigned long real_strtoul(const char *, char **, int) __asm__("strtoul");
extern long long real_strtoll(const char *, char **, int) __asm__("strtoll");
extern unsigned long long real_strtoull(const char *, char **, int) __asm__("strtoull");

COMPAT_LOCAL long __isoc23_strtol(const char *n, char **e, int b) { return real_strtol(n, e, b); }
COMPAT_LOCAL unsigned long __isoc23_strtoul(const char *n, char **e, int b) { return real_strtoul(n, e, b); }
COMPAT_LOCAL long long __isoc23_strtoll(const char *n, char **e, int b) { return real_strtoll(n, e, b); }
COMPAT_LOCAL unsigned long long __isoc23_strtoull(const char *n, char **e, int b) { return real_strtoull(n, e, b); }

/* La trace doit lire la VRAIE marque : la ligne "build" du fichier d'etat etait
   un texte fige, qui n'a pas suivi depuis plusieurs versions et faisait croire
   qu'une ancienne version tournait. */
const char* nam_build_tag(void)
{
	return build_tag;
}
