#define FUSE_USE_VERSION 29

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <pthread.h>
#include <limits.h>
#include <time.h>

#include <cuse_lowlevel.h>
#include <fuse_opt.h>

#include <soundcard.h>

#include "freearray.h"

#ifdef NDEBUG
#define logf(fmt, args...) ;
#else
#define logf(fmt, args...) fprintf(stderr, fmt, ##args)
#endif

static const char *usage =
"usage: proxyoss [options]\n"
"\n"
"options:\n"
"    --help|-h             print this help message\n"
"    --maj=MAJ|-M MAJ      device major number\n"
"    --min=MIN|-m MIN      device minor number\n"
"    --name=NAME|-n NAME   device name (defaults to /dev/dsp)\n"
"    --target=NAME|-t NAME target device name (defaults to /dev/dsp0)\n"
"\n";

struct params {
	unsigned		major;
	unsigned		minor;
	char			*dev_name;
	char			*target_name;
	int			is_help;
};
struct params params = { 0, 0, "dsp", "/dev/dsp0", 0 };

#define MKOPT(t, p) { t, offsetof(struct params, p), 1 }

static const struct fuse_opt opts[] = {
	MKOPT("-M %u",		major),
	MKOPT("--maj=%u",		major),
	MKOPT("-m %u",		minor),
	MKOPT("--min=%u",		minor),
	MKOPT("-n %s",		dev_name),
	MKOPT("--name=%s",	dev_name),
	MKOPT("-t %s",		target_name),
	MKOPT("--target=%s",	target_name),
	FUSE_OPT_KEY("-h",		0),
	FUSE_OPT_KEY("--help",		0),
	FUSE_OPT_END
};

typedef struct {
	int fd;
	int open_flags;
	oss_label_t label;
	int rate;
	int channels;
	int fmt;
	int fragment;
} fd_t;

FREEARRAY_TYPE(fdarr_t, fd_t);
fdarr_t fdarr;
pthread_rwlock_t fdarr_lock;
bool stopped = false;

static void get_proc_name(pid_t pid, char *dest, size_t len) {
	char path[NAME_MAX + 16];
	snprintf(path, NAME_MAX + 16, "/proc/%lld/cmdline", (long long)pid);
	FILE *f = fopen(path, "r");
	char tmpbuf[len];
	fgets(tmpbuf, len, f);
	strncpy(dest, tmpbuf, len);
	fclose(f);
}

static void my_open(fuse_req_t req, struct fuse_file_info *fi) {
	int fd;
	if (stopped) { 
		fd = -1;
	} else {
		fd = open(params.target_name, fi->flags);
		if (fd == -1)
			fuse_reply_err(req, errno);
	}

	fd_t *fdi;
	pthread_rwlock_wrlock(&fdarr_lock);
	FREEARRAY_ALLOC(&fdarr, fdi);
	fdi->fd = fd;
	fdi->open_flags = fi->flags;
	fdi->fragment = 0;
	get_proc_name(fuse_req_ctx(req)->pid, (char *)&fdi->label, OSS_LABEL_SIZE);
	ioctl(fd, SNDCTL_SETLABEL, &fdi->label);
	pthread_rwlock_unlock(&fdarr_lock);
	fi->fh = FREEARRAY_ID(&fdarr, fdi);

	fuse_reply_open(req, fi);
}

static void my_release(fuse_req_t req, struct fuse_file_info *fi) {
	pthread_rwlock_wrlock(&fdarr_lock);
	fd_t *fdi = &FREEARRAY_ARR(&fdarr)[fi->fh];
	close(fdi->fd);
	pthread_rwlock_unlock(&fdarr_lock);
	fuse_reply_err(req, 0);
}

void reopen(fd_t *fdi) {
	logf("reopening the audio device\n");
	int fd = open(params.target_name, fdi->open_flags);
	fdi->fd = fd;
	ioctl(fd, SNDCTL_DSP_SPEED, &fdi->rate);
	ioctl(fd, SNDCTL_DSP_CHANNELS, &fdi->channels);
	ioctl(fd, SNDCTL_DSP_SETFMT, &fdi->fmt);
	ioctl(fd, SNDCTL_SETLABEL, &fdi->label);
	if (fdi->fragment)
		ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &fdi->fragment);
}

void reopen_if_needed(fd_t *fdi) {
	pthread_rwlock_wrlock(&fdarr_lock);
		if (fdi->fd == -1)
			reopen(fdi);
	pthread_rwlock_unlock(&fdarr_lock);
}

void update_flags(fd_t *fdi, struct fuse_file_info *fi) {
	if (fi->flags != fdi->open_flags) {
		logf("updating flags from %x to %x\n", fdi->open_flags, fi->flags);
		if (!stopped)
			fcntl(fdi->fd, F_SETFL, fi->flags);
		fdi->open_flags = fi->flags;
	}
}

static void my_read(fuse_req_t req, size_t size, off_t off, struct fuse_file_info *fi) {
	(void)off;
	char *buf = calloc(size, 1);
	fd_t *fdi = &FREEARRAY_ARR(&fdarr)[fi->fh];
	if (stopped) { 
		pthread_rwlock_rdlock(&fdarr_lock);
		update_flags(fdi, fi);
		int fmtdiv = 1;
		switch (fdi->fmt) {
			case AFMT_S16_LE:
			case AFMT_S16_BE:
			case AFMT_U16_LE:
			case AFMT_U16_BE:
				fmtdiv = 2;
				break;
			case AFMT_S24_PACKED:
				fmtdiv = 3;
				break;
			case AFMT_S24_LE:
			case AFMT_S24_BE:
			case AFMT_S32_LE:
			case AFMT_S32_BE:
				fmtdiv = 4;
				break;
		}
		struct timespec ts;
		ts.tv_sec = 0;
		ts.tv_nsec = 1000000000 / fdi->rate / fdi->channels / fmtdiv;
		pthread_rwlock_unlock(&fdarr_lock);
		while (nanosleep(&ts, &ts) == -1);
		fuse_reply_buf(req, buf, size);
	} else {
		reopen_if_needed(fdi);
		pthread_rwlock_rdlock(&fdarr_lock);
		update_flags(fdi, fi);
		int nfd = dup(fdi->fd);
		pthread_rwlock_unlock(&fdarr_lock);
		int rv = read(nfd, buf, size);
		close(nfd);

		if (rv == -1)
			fuse_reply_err(req, errno);

		fuse_reply_buf(req, buf, rv);
	}
	free(buf);
}

static void my_write(fuse_req_t req, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
	(void)off;
	if (stopped) { 
		fuse_reply_write(req, size);
		return;
	}
	fd_t *fdi = &FREEARRAY_ARR(&fdarr)[fi->fh];
	reopen_if_needed(fdi);
	pthread_rwlock_rdlock(&fdarr_lock);
	update_flags(fdi, fi);
	int nfd = dup(fdi->fd);
	pthread_rwlock_unlock(&fdarr_lock);
	int rv = write(nfd, buf, size);
	close(nfd);

	if (rv == -1)
		fuse_reply_err(req, errno);

	fuse_reply_write(req, rv);
}

static void my_ioctl(fuse_req_t req, int cmd, void *arg, struct fuse_file_info *fi, unsigned flags, const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
	(void)flags;
	logf("ioctl %x\n", cmd);
	fd_t *fdi = &FREEARRAY_ARR(&fdarr)[fi->fh];
	int fd;
	if (!stopped) {
		reopen_if_needed(fdi);
		pthread_rwlock_rdlock(&fdarr_lock);
		fd = fdi->fd;
		// The flags are zeroed out on ioctl call somewhy
		//update_flags(fdi, fi);
	} else {
		pthread_rwlock_rdlock(&fdarr_lock);
	}

#define WANT(in_wanted, out_wanted) \
	do { \
		if (in_bufsz < in_wanted || out_bufsz < out_wanted) { \
			struct iovec iiov = { arg, in_wanted }; \
			struct iovec oiov = { arg, out_wanted }; \
			fuse_reply_ioctl_retry(req, in_wanted ? &iiov : NULL, in_wanted ? 1 : 0, out_wanted ? &oiov : NULL, out_wanted ? 1 : 0); \
			goto out; \
		} \
	} while (0)

#define IOCTL_(c, addr, size) \
	if (!stopped) { \
		int rv = ioctl(fd, c, addr); \
		fuse_reply_ioctl(req, rv, addr, size); \
	} else { \
		fuse_reply_ioctl(req, 0, NULL, 0); \
	}
#define IOCTL(c, a) IOCTL_(c, &a, sizeof(a))

#define CASE(ioc) case (uint32_t)(ioc)

	switch (cmd) {
		CASE(SNDCTL_DSP_HALT):
			{
				IOCTL_(SNDCTL_DSP_HALT, NULL, 0);
			}
			break;
		CASE(SNDCTL_DSP_SYNC):
			{
				IOCTL_(SNDCTL_DSP_SYNC, NULL, 0);
			}
			break;
		CASE(SNDCTL_DSP_SPEED):	// 2
			{
				WANT(sizeof(int), sizeof(int));
				int a = *(int *)in_buf;
				logf("rate: want %d\n", a);
				IOCTL(SNDCTL_DSP_SPEED, a);
				logf("rate: got %d\n", a);
				fdi->rate = a;
			}
			break;
		CASE(SNDCTL_DSP_STEREO):	// 3
			{
				WANT(sizeof(int), sizeof(int));
				int a = *(int *)in_buf;
				IOCTL(SNDCTL_DSP_STEREO, a);
				fdi->channels = a ? 2 : 1;
			}
			break;
		CASE(SNDCTL_DSP_GETBLKSIZE):
			{
				// Somewhy it's WR, so we retrieve an int just to stay on the safe side
				WANT(sizeof(int), sizeof(int));
				int a = *(int *)in_buf;
				IOCTL(SNDCTL_DSP_GETBLKSIZE, a);
			}
			break;
		CASE(SNDCTL_DSP_SETFMT):	// 5
			{
				WANT(sizeof(int), sizeof(int));
				int a = *(int *)in_buf;
				logf("fmt: want %x\n", a);
				IOCTL(SNDCTL_DSP_SETFMT, a);
				logf("fmt: got %x\n", a);
				fdi->fmt = a;
			}
			break;
		CASE(SNDCTL_DSP_CHANNELS):	// 6
			{
				WANT(sizeof(int), sizeof(int));
				int a = *(int *)in_buf;
				logf("chans: want %d\n", a);
				IOCTL(SNDCTL_DSP_CHANNELS, a);
				logf("chans: got %d\n", a);
				fdi->channels = a;
			}
			break;
		CASE(OSS_GETVERSION):
			{
				WANT(0, sizeof(int));
				int a;
				IOCTL(OSS_GETVERSION, a);
			}
			break;
		CASE(SNDCTL_DSP_SETFRAGMENT):
			{
				WANT(sizeof(int), sizeof(int));
				int a = *(int *)in_buf;
				IOCTL(SNDCTL_DSP_SETFRAGMENT, a);
				fdi->fragment = a;
			}
			break;
		CASE(SNDCTL_DSP_GETFMTS):
			{
				WANT(0, sizeof(int));
				int a;
				IOCTL(SNDCTL_DSP_GETFMTS, a);
			}
			break;
		CASE(SNDCTL_DSP_GETOSPACE):	// 12
			{
				WANT(0, sizeof(audio_buf_info));
				audio_buf_info a;
				IOCTL(SNDCTL_DSP_GETOSPACE, a);
			}
			break;
		CASE(SNDCTL_DSP_GETISPACE):	// 13
			{
				WANT(0, sizeof(audio_buf_info));
				audio_buf_info a;
				IOCTL(SNDCTL_DSP_GETISPACE, a);
			}
			break;
		CASE(SNDCTL_DSP_GETCAPS):
			{
				WANT(0, sizeof(int));
				int a;
				IOCTL(SNDCTL_DSP_GETCAPS, a);
			}
			break;
		CASE(SNDCTL_DSP_GETIPTR):	// 17
			{
				WANT(0, sizeof(count_info));
				count_info a;
				IOCTL(SNDCTL_DSP_GETIPTR, a);
			}
			break;
		CASE(SNDCTL_DSP_GETOPTR):	// 18
			{
				WANT(0, sizeof(count_info));
				count_info a;
				IOCTL(SNDCTL_DSP_GETOPTR, a);
			}
			break;
		CASE(SNDCTL_SYSINFO):
			{
				WANT(0, sizeof(oss_sysinfo));
				oss_sysinfo a;
				IOCTL(SNDCTL_SYSINFO, a);
			}
			break;
		CASE(SNDCTL_AUDIOINFO):
			{
				// TODO replace device names to ours or mess with the filesystem so the original device files will get overlapped
				WANT(sizeof(oss_audioinfo), sizeof(oss_audioinfo));
				oss_audioinfo a;
				IOCTL(SNDCTL_AUDIOINFO, a);
			}
			break;
		CASE(SNDCTL_SETLABEL):
			{
				WANT(sizeof(oss_label_t), 0);
				oss_label_t *a = (oss_label_t *)in_buf;
				IOCTL(SNDCTL_SETLABEL, a);
				memcpy(&fdi->label, a, sizeof(oss_label_t));
			}
			break;
		default:
			printf("ioctl failed %x\n", cmd);
			fuse_reply_err(req, ENOSYS);
			break;
	}

out:
	pthread_rwlock_unlock(&fdarr_lock);
}

static const struct cuse_lowlevel_ops cuseops = {
	.open		= my_open,
	.release	= my_release,
	.read		= my_read,
	.write		= my_write,
	.ioctl		= my_ioctl,
};

static int process_arg(void *data, const char *arg, int key, struct fuse_args *outargs) {
	struct params *param = data;

	(void)outargs;
	(void)arg;

	switch (key) {
		case 0:
			param->is_help = 1;
			fprintf(stderr, "%s", usage);
			return fuse_opt_add_arg(outargs, "-ho");
		default:
			return 1;
	}
}

void stop(int sig) {
	(void)sig;
	if (stopped) return;
	pthread_rwlock_wrlock(&fdarr_lock);
	stopped = true;
	for (unsigned i = 0; i < FREEARRAY_LEN(&fdarr); i++) {
		fd_t *fdi = &FREEARRAY_ARR(&fdarr)[i];
		if (fdi->fd != -1) {
			close(fdi->fd);
			fdi->fd = -1;
		}
	}
	pthread_rwlock_unlock(&fdarr_lock);
}

void cont(int sig) {
	(void)sig;
	if (!stopped) return;
	pthread_rwlock_wrlock(&fdarr_lock);
	for (unsigned i = 0; i < FREEARRAY_LEN(&fdarr); i++) {
		fd_t *fdi = &FREEARRAY_ARR(&fdarr)[i];
		reopen(fdi);
	}
	stopped = false;
	pthread_rwlock_unlock(&fdarr_lock);
}

int main(int argc, char **argv) {
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	char dev_name[128] = "DEVNAME=";
	const char *dev_info_argv[] = { dev_name };
	struct cuse_info ci;

	if (fuse_opt_parse(&args, &params, opts, process_arg)) {
		printf("failed to parse option\n");
		return 1;
	}

	if (!params.is_help) {
		strncat(dev_name, params.dev_name, sizeof(dev_name) - 9);
	}


	memset(&ci, 0, sizeof(ci));
	ci.dev_major = params.major;
	ci.dev_minor = params.minor;
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	FREEARRAY_CREATE(&fdarr);
	pthread_rwlock_init(&fdarr_lock, NULL);

	struct sigaction sta, cta;
	sta.sa_handler = stop;
	sta.sa_flags = 0;
	sigemptyset(&sta.sa_mask);
	sigaction(SIGUSR1, &sta, NULL);
	cta.sa_handler = cont;
	cta.sa_flags = 0;
	sigemptyset(&cta.sa_mask);
	sigaction(SIGUSR2, &cta, NULL);

	return cuse_lowlevel_main(args.argc, args.argv, &ci, &cuseops, NULL);
}
