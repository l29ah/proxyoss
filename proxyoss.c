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

#include <cuse_lowlevel.h>
#include <fuse_opt.h>

#include <soundcard.h>

#include "freearray.h"

static const char *usage =
"usage: cusexmp [options]\n"
"\n"
"options:\n"
"    --help|-h             print this help message\n"
"    --maj=MAJ|-M MAJ      device major number\n"
"    --min=MIN|-m MIN      device minor number\n"
"    --name=NAME|-n NAME   device name (mandatory)\n"
"    --target=NAME|-t NAME target device name (defaults to /dev/dsp)\n"
"\n";

struct cusexmp_param {
	unsigned		major;
	unsigned		minor;
	char			*dev_name;
	char			*target_name;
	int			is_help;
};

#define CUSEXMP_OPT(t, p) { t, offsetof(struct cusexmp_param, p), 1 }

static const struct fuse_opt cusexmp_opts[] = {
	CUSEXMP_OPT("-M %u",		major),
	CUSEXMP_OPT("--maj=%u",		major),
	CUSEXMP_OPT("-m %u",		minor),
	CUSEXMP_OPT("--min=%u",		minor),
	CUSEXMP_OPT("-n %s",		dev_name),
	CUSEXMP_OPT("--name=%s",	dev_name),
	CUSEXMP_OPT("-t %s",		target_name),
	CUSEXMP_OPT("--target=%s",	target_name),
	FUSE_OPT_KEY("-h",		0),
	FUSE_OPT_KEY("--help",		0),
	FUSE_OPT_END
};

char *target_name;

typedef struct {
	int fd;
	int open_flags;
	int rate;
	int channels;
	int fmt;
} fd_t;

FREEARRAY_TYPE(fdarr_t, fd_t);
fdarr_t fdarr;
pthread_rwlock_t fdarr_lock;
bool stopped = false;

static void my_open(fuse_req_t req, struct fuse_file_info *fi) {
	puts("lolopen");
	int fd;
	if (stopped) { 
		fd = -1;
	} else {
		fd = open(target_name, fi->flags);
		if (fd == -1)
			fuse_reply_err(req, -errno);
	}

	fd_t *fdi;
	pthread_rwlock_wrlock(&fdarr_lock);
	FREEARRAY_ALLOC(&fdarr, fdi);
	fdi->fd = fd;
	fdi->open_flags = fi->flags;
	pthread_rwlock_unlock(&fdarr_lock);
	fi->fh = FREEARRAY_ID(&fdarr, fdi);

	fuse_reply_open(req, fi);
}

static void my_read(fuse_req_t req, size_t size, off_t off, struct fuse_file_info *fi) {
	(void)off;
	char *buf = calloc(size, 1);
	if (stopped) { 
		fuse_reply_buf(req, buf, size);
		// TODO may need to include some smart sleeping
		return;
	}
	pthread_rwlock_rdlock(&fdarr_lock);
	int rv = read(FREEARRAY_ARR(&fdarr)[fi->fh].fd, buf, size);
	pthread_rwlock_unlock(&fdarr_lock);

	if (rv == -1)
		fuse_reply_err(req, errno);

	fuse_reply_buf(req, buf, rv);
}

static void my_write(fuse_req_t req, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
	(void)off;
	if (stopped) { 
		fuse_reply_write(req, size);
		return;
	}
	pthread_rwlock_rdlock(&fdarr_lock);
	int rv = write(FREEARRAY_ARR(&fdarr)[fi->fh].fd, buf, size);
	pthread_rwlock_unlock(&fdarr_lock);

	if (rv == -1)
		fuse_reply_err(req, errno);

	fuse_reply_write(req, rv);
}

static void my_ioctl(fuse_req_t req, int cmd, void *arg, struct fuse_file_info *fi, unsigned flags, const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
	if (stopped) { 
		fuse_reply_ioctl(req, 0, NULL, 0);
		return;
	}
	pthread_rwlock_rdlock(&fdarr_lock);
	fd_t *fdi = &FREEARRAY_ARR(&fdarr)[fi->fh];
	int fd = fdi->fd;
	printf("ioctl %x\n", cmd);

	if (flags & FUSE_IOCTL_COMPAT) {
		fuse_reply_err(req, ENOSYS);
		return;
	}

#define WANT(in_wanted, out_wanted) \
	do { \
		if (in_bufsz < in_wanted || out_bufsz < out_wanted) { \
			puts("retry!"); \
			struct iovec iiov = { arg, in_wanted }; \
			struct iovec oiov = { arg, out_wanted }; \
			fuse_reply_ioctl_retry(req, in_wanted ? &iiov : NULL, in_wanted ? 1 : 0, out_wanted ? &oiov : NULL, out_wanted ? 1 : 0); \
			goto out; \
		} \
	} while(0)

	switch (cmd) {
		case SNDCTL_DSP_SPEED:	// 2
			{
				WANT(sizeof(int), sizeof(int));
				int a = *(int *)in_buf;
				int rv = ioctl(fd, SNDCTL_DSP_SPEED, &a);
				fdi->rate = a;
				fuse_reply_ioctl(req, rv, &a, sizeof(int));
			}
			fuse_reply_ioctl(req, 0, NULL, 0);
			break;
		case SNDCTL_DSP_STEREO:	// 3
			{
				WANT(sizeof(int), sizeof(int));
				int a = *(int *)in_buf;
				int rv = ioctl(fd, SNDCTL_DSP_STEREO, &a);
				fdi->channels = a ? 2 : 1;
				fuse_reply_ioctl(req, rv, &a, sizeof(int));
			}
			break;
		case SNDCTL_DSP_SETFMT:	// 5
			{
				WANT(sizeof(int), sizeof(int));
				int a = *(int *)in_buf;
				int rv = ioctl(fd, SNDCTL_DSP_SETFMT, &a);
				fdi->fmt = a;
				fuse_reply_ioctl(req, rv, &a, sizeof(int));
			}
			break;
		case SNDCTL_DSP_CHANNELS:	// 6
			{
				WANT(sizeof(int), sizeof(int));
				int a = *(int *)in_buf;
				int rv = ioctl(fd, SNDCTL_DSP_CHANNELS, &a);
				fdi->channels = a;
				fuse_reply_ioctl(req, rv, &a, sizeof(int));
			}
			break;
		case SNDCTL_DSP_GETOSPACE:	// 12
			{
				WANT(0, sizeof(audio_buf_info));
				audio_buf_info a;
				int rv = ioctl(fd, SNDCTL_DSP_GETOSPACE, &a);
				fuse_reply_ioctl(req, rv, &a, sizeof(audio_buf_info));
			}
			break;
		case SNDCTL_DSP_GETISPACE:	// 13
			{
				WANT(0, sizeof(audio_buf_info));
				audio_buf_info a;
				int rv = ioctl(fd, SNDCTL_DSP_GETISPACE, &a);
				fuse_reply_ioctl(req, rv, &a, sizeof(audio_buf_info));
			}
			break;
		case SNDCTL_DSP_GETIPTR:	// 17
			{
				WANT(0, sizeof(count_info));
				count_info a;
				int rv = ioctl(fd, SNDCTL_DSP_GETIPTR, &a);
				fuse_reply_ioctl(req, rv, &a, sizeof(count_info));
			}
			break;
		case SNDCTL_DSP_GETOPTR:	// 18
			{
				WANT(0, sizeof(count_info));
				count_info a;
				int rv = ioctl(fd, SNDCTL_DSP_GETOPTR, &a);
				fuse_reply_ioctl(req, rv, &a, sizeof(count_info));
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
	.read		= my_read,
	.write		= my_write,
	.ioctl		= my_ioctl,
};

static int cusexmp_process_arg(void *data, const char *arg, int key,
			       struct fuse_args *outargs)
{
	struct cusexmp_param *param = data;

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
		int fd = open(target_name, fdi->open_flags);
		fdi->fd = fd;
		ioctl(fd, SNDCTL_DSP_SPEED, &fdi->rate);
		ioctl(fd, SNDCTL_DSP_CHANNELS, &fdi->channels);
		ioctl(fd, SNDCTL_DSP_SETFMT, &fdi->fmt);
	}
	stopped = false;
	pthread_rwlock_unlock(&fdarr_lock);
}

int main(int argc, char **argv) {
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct cusexmp_param param = { 0, 0, NULL, "/dev/dsp", 0 };
	char dev_name[128] = "DEVNAME=";
	const char *dev_info_argv[] = { dev_name };
	struct cuse_info ci;

	if (fuse_opt_parse(&args, &param, cusexmp_opts, cusexmp_process_arg)) {
		printf("failed to parse option\n");
		return 1;
	}

	if (!param.is_help) {
		if (!param.dev_name) {
			fprintf(stderr, "Error: device name missing\n");
			return 1;
		}
		strncat(dev_name, param.dev_name, sizeof(dev_name) - 9);
	}

	if (param.target_name)
		target_name = param.target_name;

	memset(&ci, 0, sizeof(ci));
	ci.dev_major = param.major;
	ci.dev_minor = param.minor;
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
