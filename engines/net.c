/*
 * Transfer data over the net. Pretty basic setup, will only support
 * 1 file per thread/job.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "../fio.h"
#include "../os.h"

struct net_data {
	int send_to_net;
	struct io_u *last_io_u;
};

static int fio_netio_getevents(struct thread_data *td, int fio_unused min,
				int max, struct timespec fio_unused *t)
{
	assert(max <= 1);

	/*
	 * we can only have one finished io_u for sync io, since the depth
	 * is always 1
	 */
	if (list_empty(&td->io_u_busylist))
		return 0;

	return 1;
}

static struct io_u *fio_netio_event(struct thread_data *td, int event)
{
	struct net_data *nd = td->io_ops->data;

	assert(event == 0);

	return nd->last_io_u;
}

static int fio_netio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct net_data *nd = td->io_ops->data;
	struct fio_file *f = io_u->file;

	if (nd->send_to_net) {
		if (io_u->ddir == DDIR_READ) {
			td_verror(td, EINVAL);
			return 1;
		}
	} else {
		if (io_u->ddir == DDIR_WRITE) {
			td_verror(td, EINVAL);
			return 1;
		}
	}

	if (io_u->ddir == DDIR_SYNC)
		return 0;
	if (io_u->offset == f->last_completed_pos)
		return 0;

	td_verror(td, EINVAL);
	return 1;
}

static int fio_netio_queue(struct thread_data *td, struct io_u *io_u)
{
	struct net_data *nd = td->io_ops->data;
	struct fio_file *f = io_u->file;
	unsigned int ret = 0;

	if (io_u->ddir == DDIR_WRITE)
		ret = write(f->fd, io_u->buf, io_u->buflen);
	else if (io_u->ddir == DDIR_READ)
		ret = read(f->fd, io_u->buf, io_u->buflen);

	if (ret != io_u->buflen) {
		if (ret > 0) {
			io_u->resid = io_u->buflen - ret;
			io_u->error = EIO;
		} else
			io_u->error = errno;
	}

	if (!io_u->error)
		nd->last_io_u = io_u;

	return io_u->error;
}

static int fio_netio_setup_connect(struct thread_data *td, const char *host,
				   const char *port)
{
	struct sockaddr_in addr;
	struct fio_file *f;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(atoi(port));

	if (inet_aton(host, &addr.sin_addr) != 1) {
		struct hostent *hent = gethostbyname(host);

		if (!hent) {
			td_vmsg(td, errno, "gethostbyname");
			return 1;
		}

		memcpy(&addr.sin_addr, hent->h_addr, 4);
	}

	f = &td->files[0];

	f->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (f->fd < 0) {
		td_vmsg(td, errno, "socket");
		return 1;
	}

	if (connect(f->fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		td_vmsg(td, errno, "connect");
		return 1;
	}

	return 0;

}

static int fio_netio_setup_listen(struct thread_data *td, const char *port)
{
	struct sockaddr_in addr;
	socklen_t socklen;
	int fd, opt;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		td_vmsg(td, errno, "socket");
		return 1;
	}

	opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		td_vmsg(td, errno, "setsockopt");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(atoi(port));

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		td_vmsg(td, errno, "bind");
		return 1;
	}
	if (listen(fd, 1) < 0) {
		td_vmsg(td, errno, "listen");
		return 1;
	}

	socklen = sizeof(addr);
	td->files[0].fd = accept(fd, (struct sockaddr *) &addr, &socklen);
	if (td->files[0].fd < 0) {
		td_vmsg(td, errno, "accept");
		return 1;
	}

	return 0;
}

static int fio_netio_setup(struct thread_data *td)
{
	char host[64], port[64], buf[128];
	struct net_data *nd;
	char *sep;
	int ret;

	/*
	 * work around for late init call
	 */
	if (td->io_ops->init(td))
		return 1;

	nd = td->io_ops->data;

	if (td->iomix) {
		log_err("fio: network connections must be read OR write\n");
		return 1;
	}
	if (td->nr_files > 1) {
		log_err("fio: only one file supported for network\n");
		return 1;
	}

	strcpy(buf, td->filename);

	sep = strchr(buf, ':');
	if (!sep) {
		log_err("fio: bad network host:port <<%s>>\n", td->filename);
		return 1;
	}

	*sep = '\0';
	sep++;
	strcpy(host, buf);
	strcpy(port, sep);

	if (td->ddir == READ) {
		nd->send_to_net = 0;
		ret = fio_netio_setup_listen(td, port);
	} else {
		nd->send_to_net = 1;
		ret = fio_netio_setup_connect(td, host, port);
	}

	if (!ret) {
		td->io_size = td->total_file_size;
		td->total_io_size = td->io_size;
		td->files[0].real_file_size = td->io_size;
	}

	return ret;
}

static void fio_netio_cleanup(struct thread_data *td)
{
	if (td->io_ops->data) {
		free(td->io_ops->data);
		td->io_ops->data = NULL;
	}
}

static int fio_netio_init(struct thread_data *td)
{
	struct net_data *nd;

	if (td->io_ops->data)
		return 0;

	nd  = malloc(sizeof(*nd));
	nd->last_io_u = NULL;
	td->io_ops->data = nd;
	return 0;
}

static struct ioengine_ops ioengine = {
	.name		= "net",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_netio_init,
	.prep		= fio_netio_prep,
	.queue		= fio_netio_queue,
	.getevents	= fio_netio_getevents,
	.event		= fio_netio_event,
	.cleanup	= fio_netio_cleanup,
	.setup		= fio_netio_setup,
	.flags		= FIO_SYNCIO | FIO_NETIO,
};

static void fio_init fio_netio_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_netio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
