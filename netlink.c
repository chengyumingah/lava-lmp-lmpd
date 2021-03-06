#include <lmpd.h>

static int lmp_find_by_devpath(const char *devpath)
{
	int n;
	int len = strlen(devpath);

	/*
	 * if the devpath of what is being removed matches the lhs of the
	 * devpath, it's enough to mean the ancestor is removed so must the
	 * child be
	 */

	for (n = 0; n < nlmp; n++)
		if (strncmp(lmp[n].tree_path, devpath, len) == 0)
			return n;

	return -1;
}

static int lmpd_sort_compare(const void *a, const void *b)
{
	return strcmp(((struct lmp *)a)->serial, ((struct lmp *)b)->serial);
}

static void lmpd_sort(void)
{
	qsort(lmp, nlmp, sizeof lmp[0], lmpd_sort_compare);
}

static int
lmpd_cmd(struct lmp *plmp, const char *cmd)
{
	int n;

	n = write(plmp->fd, cmd, strlen(cmd));
	lwsl_notice("lmpd_cmd: %s %d\n", cmd, n);
	if (n < 0)
		return n;

	return 0;
}

int
lmpd_service_netlink(struct libwebsocket_context *context, struct pollfd *pfd)
{
	char buf[4096];
	int len;
	int relevant = 0;
	char *devname = NULL, *act = NULL, *serial = NULL, *devpath;
	int i;

	len = recv(pfd->fd, buf, sizeof(buf), MSG_DONTWAIT);
	if (len <= 0) {
		perror("recv\n");
		return len;
	}

	devpath = NULL;
	relevant = 0;
	i = 0;
	while (i < len) {
		if (!strcmp(&buf[i], "ID_MODEL=LavaLMP"))
			relevant++;
		if (!strncmp(&buf[i], "ACTION=", 7)) {
			relevant++;
			act = &buf[i + 7];
		}
		if (!strncmp(&buf[i], "DEVPATH=", 8)) {
			relevant++;
			devpath = &buf[i + 8];
		}
		if (!strncmp(&buf[i], "DEVNAME=", 8) &&
						    strstr(&buf[i], "ttyACM")) {
			relevant++;
			devname = &buf[i + 8];
		}
		if (!strncmp(&buf[i], "ID_SERIAL_SHORT=", 16)) {
			relevant++;
			serial = &buf[i + 16];
		}

//		fprintf(stderr, "tok %s\n", buf + i);
		i += strlen(buf + i) + 1;
	}

	if (!devpath)
		return 0;

	i = lmp_find_by_devpath(devpath);
	if (relevant == 5 && (!strcmp(act, "add") || !strcmp(act, "change"))) {
		if (i >= 0) {
			lwsl_notice("Found %s\n", devpath);
			return 0;
		}
		/* create the struct lmp */
		if (nlmp == MAX_LMP) {
			lwsl_err("Too many lmp\n");
			return 0;
		}

		memset(&lmp[nlmp], 0, sizeof lmp[nlmp]);
		strncpy(lmp[nlmp].tree_path, devpath,
					 sizeof lmp[nlmp].tree_path);
		lmp[nlmp].tree_path[sizeof(lmp[nlmp].tree_path) - 1] = '\0';
		strncpy(lmp[nlmp].dev_path, devname, sizeof lmp[nlmp].dev_path);
		lmp[nlmp].dev_path[sizeof(lmp[nlmp].dev_path) - 1] = '\0';
		strncpy(lmp[nlmp].serial, serial, sizeof lmp[nlmp].serial);
		lmp[nlmp].serial[sizeof(lmp[nlmp].serial) - 1] = '\0';

		/* open the device path */
		lmp[nlmp].fd = open(devname, O_RDWR, 0);
		if (lmp[nlmp].fd < 0) {
			lwsl_err("Unable to open %s\n", devname);
			return 0;
		}

		/* enforce suitable tty state */

		if (set_tty_for_lmp(lmp[nlmp].fd)) {
			lwsl_err("tcgetattr failed on %s\n", devname);
			close(lmp[nlmp].fd);
			return 0;
		}

		/* add us to the poll array */
		fd_to_type[lmp[nlmp].fd] = LST_TTYACM;
		callback_http(NULL, NULL, LWS_CALLBACK_ADD_POLL_FD,
				      NULL, (void *)(long)lmp[nlmp].fd, POLLIN);
		lwsl_notice("Added %s fd %d serial %s\n",
					devname, lmp[nlmp].fd, serial);

		/* make it official */
		lmpd_cmd(&lmp[nlmp++], "\x02{ \"schema\":\"org.linaro.lmp.info\" }\x04");

		lmpd_sort();

		/* inform when we got the json back */
		return 0;
	}
	if (relevant == 4 && !strcmp(act, "remove")) {
		if (i < 0)
			return 0;
		lwsl_notice("Removed #%d %s serial %s, closing fd %d\n",
			i, lmp[i].dev_path, lmp[i].serial, lmp[i].fd);
		fd_to_type[lmp[i].fd] = LST_UNKNOWN;
		callback_http(NULL, NULL, LWS_CALLBACK_DEL_POLL_FD,
			      NULL, (void *)(long)lmp[i].fd, 0);
		lwsl_err("closing fd %d\n", lmp[i].fd);
		close(lmp[i].fd);
		if (nlmp > 1)
			lmp[i] = lmp[nlmp - 1];
		nlmp--;
		lmpd_sort();
		goto inform;
	}

	return 0;

inform:
	/* inform clients something happened */
	issue++;
	libwebsocket_callback_on_writable_all_protocol(
						&protocols[PROTOCOL_LMPD]);

	return 0;
}
