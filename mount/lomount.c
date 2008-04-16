/* Originally from Ted's losetup.c */
/*
 * losetup.c - setup and control loop devices
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <inttypes.h>
#include <dirent.h>

#include "loop.h"
#include "lomount.h"
#include "rmd160.h"
#include "xstrncpy.h"
#include "nls.h"
#include "sundries.h"
#include "xmalloc.h"
#include "realpath.h"

#define SIZE(a) (sizeof(a)/sizeof(a[0]))

#ifdef LOOP_SET_FD

static int is_associated(int dev, struct stat *file, unsigned long long offset, int isoff);

static int
loop_info64_to_old(const struct loop_info64 *info64, struct loop_info *info)
{
        memset(info, 0, sizeof(*info));
        info->lo_number = info64->lo_number;
        info->lo_device = info64->lo_device;
        info->lo_inode = info64->lo_inode;
        info->lo_rdevice = info64->lo_rdevice;
        info->lo_offset = info64->lo_offset;
        info->lo_encrypt_type = info64->lo_encrypt_type;
        info->lo_encrypt_key_size = info64->lo_encrypt_key_size;
        info->lo_flags = info64->lo_flags;
        info->lo_init[0] = info64->lo_init[0];
        info->lo_init[1] = info64->lo_init[1];
        if (info->lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
                memcpy(info->lo_name, info64->lo_crypt_name, LO_NAME_SIZE);
        else
                memcpy(info->lo_name, info64->lo_file_name, LO_NAME_SIZE);
        memcpy(info->lo_encrypt_key, info64->lo_encrypt_key, LO_KEY_SIZE);

        /* error in case values were truncated */
        if (info->lo_device != info64->lo_device ||
            info->lo_rdevice != info64->lo_rdevice ||
            info->lo_inode != info64->lo_inode ||
            info->lo_offset != info64->lo_offset)
                return -EOVERFLOW;

        return 0;
}

#define DEV_LOOP_PATH		"/dev/loop"
#define DEV_PATH		"/dev"
#define SYSFS_BLOCK_PATH	"/sys/block"
#define LOOPMAJOR		7
#define NLOOPS_DEFAULT		8	/* /dev/loop[0-7] */

struct looplist {
	int		flag;		/* scanning options */
	int		ndef;		/* number of tested default devices */
	struct dirent   **names;	/* scandir-like list of loop devices */
	int		nnames;		/* number of items in names */
	int		ncur;		/* current possition in direcotry */
	char		name[32];	/* device name */
	int		ct_perm;	/* count permission problems */
	int		ct_succ;	/* count number of successfully
					   detected devices */
};

#define LLFLG_USEDONLY	(1 << 1)	/* return used devices only */
#define LLFLG_FREEONLY	(1 << 2)	/* return non-used devices */
#define LLFLG_DONE	(1 << 3)	/* all is done */
#define LLFLG_SYSFS	(1 << 4)	/* try to use /sys/block */
#define LLFLG_SUBDIR	(1 << 5)	/* /dev/loop/N */
#define LLFLG_DFLT	(1 << 6)	/* directly try to check default loops */

int
is_loop_device (const char *device) {
	struct stat st;

	return (stat(device, &st) == 0 &&
		S_ISBLK(st.st_mode) &&
		major(st.st_rdev) == LOOPMAJOR);
}

static int
is_loop_used(int fd)
{
	struct loop_info li;
	return ioctl (fd, LOOP_GET_STATUS, &li) == 0;
}

static char *
looplist_mk_devname(struct looplist *ll, int num)
{
	if (ll->flag & LLFLG_SUBDIR)
		snprintf(ll->name, sizeof(ll->name),
				DEV_LOOP_PATH "/%d", num);
	else
		snprintf(ll->name, sizeof(ll->name),
				DEV_PATH "/loop%d", num);

	return is_loop_device(ll->name) ? ll->name : NULL;
}

/* ignores all non-loop devices, default loop devices */
static int
filter_loop(const struct dirent *d)
{
	return strncmp(d->d_name, "loop", 4) == 0;
}

/* all loops exclude default loops */
static int
filter_loop_ndflt(const struct dirent *d)
{
	int mn;

	if (strncmp(d->d_name, "loop", 4) == 0 &&
			sscanf(d->d_name, "loop%d", &mn) == 1 &&
			mn >= NLOOPS_DEFAULT)
		return 1;
	return 0;
}

static int
filter_loop_num(const struct dirent *d)
{
	char *end = NULL;
	int mn = strtol(d->d_name, &end, 10);

	if (mn >= NLOOPS_DEFAULT && end && *end == '\0')
		return 1;
	return 0;
}

static int
looplist_open(struct looplist *ll, int flag)
{
	struct stat st;

	memset(ll, 0, sizeof(*ll));
	ll->flag = flag;
	ll->ndef = -1;
	ll->ncur = -1;

	if (stat(DEV_PATH, &st) == -1 || (!S_ISDIR(st.st_mode)))
		return -1;			/* /dev doesn't exist */

	if (stat(DEV_LOOP_PATH, &st) == 0 && S_ISDIR(st.st_mode))
		ll->flag |= LLFLG_SUBDIR;	/* /dev/loop/ exists */

	if ((ll->flag & LLFLG_USEDONLY) &&
			stat(SYSFS_BLOCK_PATH, &st) == 0 &&
			S_ISDIR(st.st_mode))
		ll->flag |= LLFLG_SYSFS;	/* try to use /sys/block/loopN */

	ll->flag |= LLFLG_DFLT;			/* required! */
	return 0;
}

static void
looplist_close(struct looplist *ll)
{
	if (ll->names) {
		for(++ll->ncur; ll->ncur < ll->nnames; ll->ncur++)
			free(ll->names[ll->ncur]);

		free(ll->names);
		ll->names = NULL;
		ll->nnames = 0;
	}
	ll->ncur = -1;
	ll->flag |= LLFLG_DONE;
}

static int
looplist_is_wanted(struct looplist *ll, int fd)
{
	int ret;

	if (!(ll->flag & (LLFLG_USEDONLY | LLFLG_FREEONLY)))
		return 1;
	ret = is_loop_used(fd);

	if ((ll->flag & LLFLG_USEDONLY) && ret == 0)
		return 0;
	if ((ll->flag & LLFLG_FREEONLY) && ret == 1)
		return 0;

	return 1;
}

static int
looplist_next(struct looplist *ll)
{
	int fd;
	int ret;
	char *dirname, *dev;

	if (ll->flag & LLFLG_DONE)
		return -1;

	/* A) try to use /sys/block/loopN devices (for losetup -a only)
	 */
	if (ll->flag & LLFLG_SYSFS) {
		int mn;

		if (!ll->nnames) {
			ll->nnames = scandir(SYSFS_BLOCK_PATH, &ll->names,
						filter_loop, versionsort);
			ll->ncur = -1;
		}
		for(++ll->ncur; ll->ncur < ll->nnames; ll->ncur++) {
			ret = sscanf(ll->names[ll->ncur]->d_name, "loop%d", &mn);
			free(ll->names[ll->ncur]);
			if (ret != 1)
				continue;
			dev = looplist_mk_devname(ll, mn);
			if (dev) {
				ll->ct_succ++;
				if ((fd = open(dev, O_RDONLY)) > -1) {
					if (looplist_is_wanted(ll, fd))
						return fd;
					close(fd);
				} else if (errno == EACCES)
					ll->ct_perm++;
			}
		}
		if (ll->nnames)
			free(ll->names);
		ll->names = NULL;
		ll->ncur = -1;
		ll->nnames = 0;
		ll->flag &= ~LLFLG_SYSFS;
		goto done;
	}

	/* B) Classic way, try first eight loop devices (default number
	 *    of loop devices). This is enough for 99% of all cases.
	 */
	if (ll->flag & LLFLG_DFLT) {
		for (++ll->ncur; ll->ncur < NLOOPS_DEFAULT; ll->ncur++) {
			dev = looplist_mk_devname(ll, ll->ncur);
			if (dev) {
				ll->ct_succ++;
				if ((fd = open(dev, O_RDONLY)) > -1) {
					if (looplist_is_wanted(ll, fd))
						return fd;
					close(fd);
				} else if (errno == EACCES)
					ll->ct_perm++;
			}
		}
		ll->flag &= ~LLFLG_DFLT;
		ll->ncur = -1;
	}


	/* C) the worst posibility, scan all /dev or /dev/loop
	 */
	dirname = ll->flag & LLFLG_SUBDIR ? DEV_LOOP_PATH : DEV_PATH;

	if (!ll->nnames) {
		ll->nnames = scandir(dirname, &ll->names,
				ll->flag & LLFLG_SUBDIR ?
					filter_loop_num : filter_loop_ndflt,
				versionsort);
		ll->ncur = -1;
	}

	for(++ll->ncur; ll->ncur < ll->nnames; ll->ncur++) {
		struct stat st;

		snprintf(ll->name, sizeof(ll->name),
			"%s/%s", dirname, ll->names[ll->ncur]->d_name);
		free(ll->names[ll->ncur]);
		ret = stat(ll->name, &st);

		if (ret == 0 &&	S_ISBLK(st.st_mode) &&
				major(st.st_rdev) == LOOPMAJOR &&
				minor(st.st_rdev) >= NLOOPS_DEFAULT) {
			ll->ct_succ++;
			fd = open(ll->name, O_RDONLY);

			if (fd != -1) {
				if (looplist_is_wanted(ll, fd))
					return fd;
				close(fd);
			} else if (errno == EACCES)
				ll->ct_perm++;
		}
	}
done:
	looplist_close(ll);
	return -1;
}

#ifdef MAIN

static int
show_loop_fd(int fd, char *device) {
	struct loop_info loopinfo;
	struct loop_info64 loopinfo64;
	int errsv;

	if (ioctl(fd, LOOP_GET_STATUS64, &loopinfo64) == 0) {

		loopinfo64.lo_file_name[LO_NAME_SIZE-2] = '*';
		loopinfo64.lo_file_name[LO_NAME_SIZE-1] = 0;
		loopinfo64.lo_crypt_name[LO_NAME_SIZE-1] = 0;

		printf("%s: [%04" PRIx64 "]:%" PRIu64 " (%s)",
		       device, loopinfo64.lo_device, loopinfo64.lo_inode,
		       loopinfo64.lo_file_name);

		if (loopinfo64.lo_offset)
			printf(_(", offset %" PRIu64 ), loopinfo64.lo_offset);

		if (loopinfo64.lo_sizelimit)
			printf(_(", sizelimit %" PRIu64 ), loopinfo64.lo_sizelimit);

		if (loopinfo64.lo_encrypt_type ||
		    loopinfo64.lo_crypt_name[0]) {
			char *e = (char *)loopinfo64.lo_crypt_name;

			if (*e == 0 && loopinfo64.lo_encrypt_type == 1)
				e = "XOR";
			printf(_(", encryption %s (type %" PRIu32 ")"),
			       e, loopinfo64.lo_encrypt_type);
		}
		printf("\n");
		return 0;
	}

	if (ioctl(fd, LOOP_GET_STATUS, &loopinfo) == 0) {
		printf ("%s: [%04x]:%ld (%s)",
			device, (unsigned int)loopinfo.lo_device, loopinfo.lo_inode,
			loopinfo.lo_name);

		if (loopinfo.lo_offset)
			printf(_(", offset %d"), loopinfo.lo_offset);

		if (loopinfo.lo_encrypt_type)
			printf(_(", encryption type %d\n"),
			       loopinfo.lo_encrypt_type);

		printf("\n");
		return 0;
	}

	errsv = errno;
	fprintf(stderr, _("loop: can't get info on device %s: %s\n"),
		device, strerror (errsv));
	return 1;
}

static int
show_loop(char *device) {
	int ret, fd;

	if ((fd = open(device, O_RDONLY)) < 0) {
		int errsv = errno;
		fprintf(stderr, _("loop: can't open device %s: %s\n"),
			device, strerror (errsv));
		return 2;
	}
	ret = show_loop_fd(fd, device);
	close(fd);
	return ret;
}


static int
show_used_loop_devices (void) {
	struct looplist ll;
	int fd;

	if (looplist_open(&ll, LLFLG_USEDONLY) == -1) {
		error(_("%s: /dev directory does not exist."), progname);
		return 1;
	}

	while((fd = looplist_next(&ll)) != -1) {
		show_loop_fd(fd, ll.name);
		close(fd);
	}
	looplist_close(&ll);

	if (ll.ct_succ && ll.ct_perm) {
		error(_("%s: no permission to look at /dev/loop#"), progname);
		return 1;
	}
	return 0;
}

/* list all associated loop devices */
static int
show_associated_loop_devices(char *filename, unsigned long long offset, int isoff)
{
	struct looplist ll;
	struct stat filestat;
	int fd;

	if (stat(filename, &filestat) == -1) {
		perror(filename);
		return 1;
	}

	if (looplist_open(&ll, LLFLG_USEDONLY) == -1) {
		error(_("%s: /dev directory does not exist."), progname);
		return 1;
	}

	while((fd = looplist_next(&ll)) != -1) {
		if (is_associated(fd, &filestat, offset, isoff) == 1)
			show_loop_fd(fd, ll.name);
		close(fd);
	}
	looplist_close(&ll);

	return 0;
}

#endif /* MAIN */

/* check if the loopfile is already associated with the same given
 * parameters.
 *
 * returns: -1 error
 *           0 unused
 *           1 loop device already used
 */
static int
is_associated(int dev, struct stat *file, unsigned long long offset, int isoff)
{
	struct loop_info64 linfo64;
	struct loop_info64 linfo;
	int ret = 0;

	if (ioctl(dev, LOOP_GET_STATUS64, &linfo64) == 0) {
		if (file->st_dev == linfo64.lo_device &&
	            file->st_ino == linfo64.lo_inode &&
		    (isoff == 0 || offset == linfo64.lo_offset))
			ret = 1;
		return ret;
	}
	if (ioctl(dev, LOOP_GET_STATUS, &linfo) == 0) {
		if (file->st_dev == linfo.lo_device &&
	            file->st_ino == linfo.lo_inode &&
		    (isoff == 0 || offset == linfo.lo_offset))
			ret = 1;
		return ret;
	}

	return errno == ENXIO ? 0 : -1;
}

/* check if the loop file is already used with the same given
 * parameters. We check for device no, inode and offset.
 * returns: associated devname or NULL
 */
char *
loopfile_used (const char *filename, unsigned long long offset) {
	struct looplist ll;
	char *devname = NULL;
	struct stat filestat;
	int fd;

	if (stat(filename, &filestat) == -1) {
		perror(filename);
		return NULL;
	}

	if (looplist_open(&ll, LLFLG_USEDONLY) == -1) {
		error(_("%s: /dev directory does not exist."), progname);
		return NULL;
	}

	while((fd = looplist_next(&ll)) != -1) {
		int res = is_associated(fd, &filestat, offset, 1);
		close(fd);
		if (res == 1) {
			devname = xstrdup(ll.name);
			break;
		}
	}
	looplist_close(&ll);

	return devname;
}

int
loopfile_used_with(char *devname, const char *filename, unsigned long long offset)
{
	struct stat statbuf;
	int fd, ret;

	if (!is_loop_device(devname))
		return 0;

	if (stat(filename, &statbuf) == -1) {
		perror(filename);
		return -1;
	}

	fd = open(devname, O_RDONLY);
	if (fd == -1) {
		perror(devname);
		return -1;
	}
	ret = is_associated(fd, &statbuf, offset, 1);

	close(fd);
	return ret;
}

char *
find_unused_loop_device (void) {
	struct looplist ll;
	char *devname = NULL;
	int fd;

	if (looplist_open(&ll, LLFLG_FREEONLY) == -1) {
		error(_("%s: /dev directory does not exist."), progname);
		return NULL;
	}

	if ((fd = looplist_next(&ll)) != -1) {
		close(fd);
		devname = xstrdup(ll.name);
	}
	looplist_close(&ll);
	if (devname)
		return devname;

	if (ll.ct_succ && ll.ct_perm)
		error(_("%s: no permission to look at /dev/loop#"), progname);
	else if (ll.ct_succ)
		error(_("%s: could not find any free loop device"), progname);
	else
		error(_(
		    "%s: Could not find any loop device. Maybe this kernel "
		    "does not know\n"
		    "       about the loop device? (If so, recompile or "
		    "`modprobe loop'.)"), progname);
	return NULL;
}

/*
 * A function to read the passphrase either from the terminal or from
 * an open file descriptor.
 */
static char *
xgetpass(int pfd, const char *prompt) {
	char *pass;
	int buflen, i;

        if (pfd < 0) /* terminal */
		return getpass(prompt);

	pass = NULL;
	buflen = 0;
	for (i=0; ; i++) {
		if (i >= buflen-1) {
				/* we're running out of space in the buffer.
				 * Make it bigger: */
			char *tmppass = pass;
			buflen += 128;
			pass = realloc(tmppass, buflen);
			if (pass == NULL) {
				/* realloc failed. Stop reading. */
				error("Out of memory while reading passphrase");
				pass = tmppass; /* the old buffer hasn't changed */
				break;
			}
		}
		if (read(pfd, pass+i, 1) != 1 ||
		    pass[i] == '\n' || pass[i] == 0)
			break;
	}

	if (pass == NULL)
		return "";

	pass[i] = 0;
	return pass;
}

static int
digits_only(const char *s) {
	while (*s)
		if (!isdigit(*s++))
			return 0;
	return 1;
}

int
set_loop(const char *device, const char *file, unsigned long long offset,
	 unsigned long long sizelimit, const char *encryption, int pfd, int *options,
	 int keysz, int hash_pass) {
	struct loop_info64 loopinfo64;
	int fd, ffd, mode, i;
	char *pass;
	char *filename;

	if (verbose) {
		char *xdev = loopfile_used(file, offset);

		if (xdev) {
			printf(_("warning: %s is already associated with %s\n"),
					file, xdev);
			free(xdev);
		}
	}

	mode = (*options & SETLOOP_RDONLY) ? O_RDONLY : O_RDWR;
	if ((ffd = open(file, mode)) < 0) {
		if (!(*options & SETLOOP_RDONLY) && errno == EROFS)
			ffd = open(file, mode = O_RDONLY);
		if (ffd < 0) {
			perror(file);
			return 1;
		}
		*options |= SETLOOP_RDONLY;
	}
	if ((fd = open(device, mode)) < 0) {
		perror (device);
		close(ffd);
		return 1;
	}
	memset(&loopinfo64, 0, sizeof(loopinfo64));

	if (!(filename = canonicalize(file)))
		filename = (char *) file;
	xstrncpy((char *)loopinfo64.lo_file_name, filename, LO_NAME_SIZE);

	if (encryption && *encryption) {
		if (digits_only(encryption)) {
			loopinfo64.lo_encrypt_type = atoi(encryption);
		} else {
			loopinfo64.lo_encrypt_type = LO_CRYPT_CRYPTOAPI;
			snprintf((char *)loopinfo64.lo_crypt_name, LO_NAME_SIZE,
				 "%s", encryption);
		}
	}

	loopinfo64.lo_offset = offset;
	loopinfo64.lo_sizelimit = sizelimit;

#ifdef MCL_FUTURE
	/*
	 * Oh-oh, sensitive data coming up. Better lock into memory to prevent
	 * passwd etc being swapped out and left somewhere on disk.
	 */
	if (loopinfo64.lo_encrypt_type != LO_CRYPT_NONE) {
		if(mlockall(MCL_CURRENT | MCL_FUTURE)) {
			perror("memlock");
			fprintf(stderr, _("Couldn't lock into memory, exiting.\n"));
			exit(1);
		}
	}
#endif


	if (keysz==0)
		keysz=LO_KEY_SIZE*8;
	switch (loopinfo64.lo_encrypt_type) {
	case LO_CRYPT_NONE:
		loopinfo64.lo_encrypt_key_size = 0;
		break;
	case LO_CRYPT_XOR:
		pass = getpass(_("Password: "));
		memset(loopinfo64.lo_encrypt_key, 0, LO_KEY_SIZE);
		xstrncpy((char *)loopinfo64.lo_encrypt_key, pass, LO_KEY_SIZE);
		memset(pass, 0, strlen(pass));
		loopinfo64.lo_encrypt_key_size = LO_KEY_SIZE;
		break;
#define HASHLENGTH 20
#if 0
	case LO_CRYPT_FISH2:
	case LO_CRYPT_BLOW:
	case LO_CRYPT_IDEA:
	case LO_CRYPT_CAST128:
	case LO_CRYPT_SERPENT:
	case LO_CRYPT_MARS:
	case LO_CRYPT_RC6:
	case LO_CRYPT_3DES:
	case LO_CRYPT_DFC:
	case LO_CRYPT_RIJNDAEL:
	    {
		char keybits[2*HASHLENGTH]; 
		char *pass2;
		int passwdlen;
		int keylength;
		int i;

  		pass = xgetpass(pfd, _("Password: "));
		passwdlen=strlen(pass);
		pass2=malloc(passwdlen+2);
		pass2[0]='A';
		strcpy(pass2+1,pass);
		rmd160_hash_buffer(keybits,pass,passwdlen);
		rmd160_hash_buffer(keybits+HASHLENGTH,pass2,passwdlen+1);
		memcpy((char*)loopinfo64.lo_encrypt_key,keybits,2*HASHLENGTH);
		memset(pass, 0, passwdlen);
		memset(pass2, 0, passwdlen+1);
		free(pass2);
		keylength=0;
		for(i=0; crypt_type_tbl[i].id != -1; i++){
		         if(loopinfo64.lo_encrypt_type == crypt_type_tbl[i].id){
			         keylength = crypt_type_tbl[i].keylength;
				 break;
			 }
		}
		loopinfo64.lo_encrypt_key_size=keylength;
		break;
	    }
#endif
	default:
		if (hash_pass) {
		    char keybits[2*HASHLENGTH]; 
		    char *pass2;
		    int passwdlen;

		    pass = xgetpass(pfd, _("Password: "));
		    passwdlen=strlen(pass);
		    pass2=malloc(passwdlen+2);
		    pass2[0]='A';
		    strcpy(pass2+1,pass);
		    rmd160_hash_buffer(keybits,pass,passwdlen);
		    rmd160_hash_buffer(keybits+HASHLENGTH,pass2,passwdlen+1);
		    memset(pass, 0, passwdlen);
		    memset(pass2, 0, passwdlen+1);
		    free(pass2);

		    memcpy((char*)loopinfo64.lo_encrypt_key,keybits,keysz/8);
		    loopinfo64.lo_encrypt_key_size = keysz/8;
		} else {
		    pass = xgetpass(pfd, _("Password: "));
		    memset(loopinfo64.lo_encrypt_key, 0, LO_KEY_SIZE);
		    xstrncpy(loopinfo64.lo_encrypt_key, pass, LO_KEY_SIZE);
		    memset(pass, 0, strlen(pass));
		    loopinfo64.lo_encrypt_key_size = LO_KEY_SIZE;
		}
	}

	if (ioctl(fd, LOOP_SET_FD, ffd) < 0) {
		int rc = 1;

		if (errno == EBUSY) {
			if (verbose)
				printf(_("ioctl LOOP_SET_FD failed: %s\n"),
							strerror(errno));
			rc = 2;
		} else
			perror("ioctl: LOOP_SET_FD");

		close(fd);
		close(ffd);
		if (file != filename)
			free(filename);
		return rc;
	}
	close (ffd);

	if (*options & SETLOOP_AUTOCLEAR)
		loopinfo64.lo_flags = LO_FLAGS_AUTOCLEAR;

	i = ioctl(fd, LOOP_SET_STATUS64, &loopinfo64);
	if (i) {
		struct loop_info loopinfo;
		int errsv = errno;

		i = loop_info64_to_old(&loopinfo64, &loopinfo);
		if (i) {
			errno = errsv;
			*options &= ~SETLOOP_AUTOCLEAR;
			perror("ioctl: LOOP_SET_STATUS64");
		} else {
			i = ioctl(fd, LOOP_SET_STATUS, &loopinfo);
			if (i)
				perror("ioctl: LOOP_SET_STATUS");
			else if (*options & SETLOOP_AUTOCLEAR)
			{
				i = ioctl(fd, LOOP_GET_STATUS, &loopinfo);
				if (i || !(loopinfo.lo_flags & LO_FLAGS_AUTOCLEAR))
					*options &= ~SETLOOP_AUTOCLEAR;
			}
		}
		memset(&loopinfo, 0, sizeof(loopinfo));
	}
	else if (*options & SETLOOP_AUTOCLEAR)
	{
		i = ioctl(fd, LOOP_GET_STATUS64, &loopinfo64);
		if (i || !(loopinfo64.lo_flags & LO_FLAGS_AUTOCLEAR))
			*options &= ~SETLOOP_AUTOCLEAR;
	}
	memset(&loopinfo64, 0, sizeof(loopinfo64));


	if (i) {
		ioctl (fd, LOOP_CLR_FD, 0);
		close (fd);
		if (file != filename)
			free(filename);
		return 1;
	}

	/*
	 * HACK: here we're leeking a file descriptor,
	 * but mount is a short-lived process anyway.
	 */
	if (!(*options & SETLOOP_AUTOCLEAR))
		close (fd);

	if (verbose > 1)
		printf(_("set_loop(%s,%s,%llu,%llu): success\n"),
		       device, filename, offset, sizelimit);
	if (file != filename)
		free(filename);
	return 0;
}

int
del_loop (const char *device) {
	int fd;

	if ((fd = open (device, O_RDONLY)) < 0) {
		int errsv = errno;
		fprintf(stderr, _("loop: can't delete device %s: %s\n"),
			device, strerror (errsv));
		return 1;
	}
	if (ioctl (fd, LOOP_CLR_FD, 0) < 0) {
		perror ("ioctl: LOOP_CLR_FD");
		close(fd);
		return 1;
	}
	close (fd);
	if (verbose > 1)
		printf(_("del_loop(%s): success\n"), device);
	return 0;
}

#else /* no LOOP_SET_FD defined */
static void
mutter(void) {
	fprintf(stderr,
		_("This mount was compiled without loop support. "
		  "Please recompile.\n"));
}

int
set_loop(const char *device, const char *file, unsigned long long offset,
	 unsigned long long sizelimit, const char *encryption, int pfd, int *options,
	 int keysz, int hash_pass) {
	mutter();
	return 1;
}

int
del_loop (const char *device) {
	mutter();
	return 1;
}

char *
find_unused_loop_device (void) {
	mutter();
	return 0;
}

#endif /* !LOOP_SET_FD */

#ifdef MAIN

#ifdef LOOP_SET_FD

#include <getopt.h>
#include <stdarg.h>

static void
usage(void) {
	fprintf(stderr, _("\nUsage:\n"
  " %1$s loop_device                             give info\n"
  " %1$s -a | --all                              list all used\n"
  " %1$s -d | --detach <loopdev>                 delete\n"
  " %1$s -f | --find                             find unused\n"
  " %1$s -j | --associated <file> [-o <num>]     list all associated with <file>\n"
  " %1$s [ options ] {-f|--find|loopdev} <file>  setup\n"),
		progname);

	fprintf(stderr, _("\nOptions:\n"
  " -e | --encryption <type> enable data encryption with specified <name/num>\n"
  " -h | --help              this help\n"
  " -o | --offset <num>      start at offset <num> into file\n"
  "      --sizelimit <num>   loop limited to only <num> bytes of the file\n"
  " -p | --pass-fd <num>     read passphrase from file descriptor <num>\n"
  " -r | --read-only         setup read-only loop device\n"
  "      --show              print device name (with -f <file>)\n"
  " -N | --nohashpass        Do not hash the given password (Debian hashes)\n"
  " -k | --keybits <num>     specify number of bits in the hashed key given\n"
  "                          to the cipher.  Some ciphers support several key\n"
  "                          sizes and might be more efficient with a smaller\n"
  "                          key size.  Key sizes < 128 are generally not\n"
  "                          recommended\n"
  " -v | --verbose           verbose mode\n\n"));
	exit(1);
 }

int
main(int argc, char **argv) {
	char *p, *offset, *sizelimit, *encryption, *passfd, *device, *file, *assoc;
	char *keysize;
	int delete, find, c, all;
	int res = 0;
	int showdev = 0;
	int ro = 0;
	int pfd = -1;
	int keysz = 0;
	int hash_pass = 1;
	unsigned long long off, slimit;
	struct option longopts[] = {
		{ "all", 0, 0, 'a' },
		{ "detach", 0, 0, 'd' },
		{ "encryption", 1, 0, 'e' },
		{ "find", 0, 0, 'f' },
		{ "help", 0, 0, 'h' },
		{ "keybits", 1, 0, 'k' },
		{ "nopasshash", 0, 0, 'N' },
		{ "nohashpass", 0, 0, 'N' },
		{ "associated", 1, 0, 'j' },
		{ "offset", 1, 0, 'o' },
		{ "sizelimit", 1, 0, 128 },
		{ "pass-fd", 1, 0, 'p' },
		{ "read-only", 0, 0, 'r' },
	        { "show", 0, 0, 's' },
		{ "verbose", 0, 0, 'v' },
		{ NULL, 0, 0, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	delete = find = all = 0;
	off = 0;
        slimit = 0;
	assoc = offset = encryption = passfd = NULL;
	keysize = NULL;

	progname = argv[0];
	if ((p = strrchr(progname, '/')) != NULL)
		progname = p+1;

	while ((c = getopt_long(argc, argv, "ade:E:fhj:k:No:p:rsv",
				longopts, NULL)) != -1) {
		switch (c) {
		case 'a':
			all = 1;
			break;
		case 'r':
			ro = 1;
			break;
		case 'd':
			delete = 1;
			break;
		case 'E':
		case 'e':
			encryption = optarg;
			break;
		case 'f':
			find = 1;
			break;
		case 'j':
			assoc = optarg;
		case 'k':
			keysize = optarg;
			break;
		case 'N':
			hash_pass = 0;
			break;
		case 'o':
			offset = optarg;
			break;
		case 'p':
			passfd = optarg;
			break;
		case 's':
			showdev = 1;
			break;
		case 'v':
			verbose = 1;
			break;

	        case 128:			/* --sizelimit */
			sizelimit = optarg;
                        break;

		default:
			usage();
		}
	}

	if (argc == 1) {
		usage();
	} else if (delete) {
		if (argc != optind+1 || encryption || offset || sizelimit ||
				find || all || showdev || assoc || ro)
			usage();
	} else if (find) {
		if (all || assoc || argc < optind || argc > optind+1)
			usage();
	} else if (all) {
		if (argc > 2)
			usage();
	} else if (assoc) {
		if (encryption || showdev || passfd || ro)
			usage();
	} else {
		if (argc < optind+1 || argc > optind+2)
			usage();
	}

	if (offset && sscanf(offset, "%llu", &off) != 1)
		usage();

	if (sizelimit && sscanf(sizelimit, "%llu", &slimit) != 1)
		usage();

	if (all)
		return show_used_loop_devices();
	else if (assoc)
		return show_associated_loop_devices(assoc, off, offset ? 1 : 0);
	else if (find) {
		device = find_unused_loop_device();
		if (device == NULL)
			return -1;
		if (argc == optind) {
			if (verbose)
				printf("Loop device is %s\n", device);
			printf("%s\n", device);
			return 0;
		}
		file = argv[optind];
	} else {
		device = argv[optind];
		if (argc == optind+1)
			file = NULL;
		else
			file = argv[optind+1];
	}

	if (delete)
		res = del_loop(device);
	else if (file == NULL)
		res = show_loop(device);
	else {
		if (passfd && sscanf(passfd, "%d", &pfd) != 1)
			usage();
		if (keysize && sscanf(keysize,"%d",&keysz) != 1)
			usage();
		do {
			res = set_loop(device, file, off, slimit, encryption, pfd, &ro, keysz, hash_pass);
			if (res == 2 && find) {
				if (verbose)
					printf("stolen loop=%s...trying again\n",
						device);
				free(device);
				if (!(device = find_unused_loop_device()))
					return -1;
			}
		} while (find && res == 2);

		if (verbose && res == 0)
			printf("Loop device is %s\n", device);

		if (res == 0 && showdev && find)
			printf("%s\n", device);
	}
	return res;
}

#else /* LOOP_SET_FD not defined */

int
main(int argc, char **argv) {
	fprintf(stderr,
		_("No loop support was available at compile time. "
		  "Please recompile.\n"));
	return -1;
}
#endif /* !LOOP_SET_FD*/
#endif /* MAIN */
