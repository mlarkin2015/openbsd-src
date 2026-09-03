/*	$OpenBSD$	*/
/*
 * Copyright (c) 2026 Mike Larkin <mlarkin@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <dev/vmm/vmm.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vmd.h"
#include "display.h"

#define CHECK(_cond) do {						\
	if (!(_cond)) {						\
		fprintf(stderr, "%s:%d: CHECK(%s) failed: %s\n",	\
		    __func__, __LINE__, #_cond, strerror(errno));	\
		exit(1);						\
	}							\
} while (0)

static void
make_path(char *path, size_t path_len, const char *dir, const char *name)
{
	CHECK(snprintf(path, path_len, "%s/%s", dir, name) > 0);
	CHECK(strnlen(path, path_len) < path_len);
}

static void
test_paths(void)
{
	struct vmop_create_params vmc;
	char path[PATH_MAX], longpath[SUN_PATH_LEN + 1];

	memset(&vmc, 0, sizeof(vmc));
	strlcpy(vmc.vmc_name, "test-vm", sizeof(vmc.vmc_name));
	CHECK(display_path_default(&vmc, path, sizeof(path)) == 0);
	CHECK(strcmp(path, VM_DISPLAY_DIR "/test-vm.vnc") == 0);
	CHECK(display_path_validate(path) == 0);

	errno = 0;
	CHECK(display_path_validate("relative.vnc") == -1);
	CHECK(errno == EINVAL);
	errno = 0;
	CHECK(display_path_validate("/") == -1);
	CHECK(errno == EINVAL);
	memset(longpath, 'a', sizeof(longpath));
	longpath[0] = '/';
	longpath[sizeof(longpath) - 1] = '\0';
	errno = 0;
	CHECK(display_path_validate(longpath) == -1);
	CHECK(errno == ENAMETOOLONG);
}

static void
test_listener(const char *dir)
{
	struct sockaddr_un sun;
	struct stat st;
	char path[PATH_MAX];
	dev_t dev;
	ino_t ino;
	int client, fd;

	make_path(path, sizeof(path), dir, "listener.sock");
	CHECK(display_socket_open(path, getuid(), &fd, &dev, &ino) == 0);
	CHECK(lstat(path, &st) == 0);
	CHECK(S_ISSOCK(st.st_mode));
	CHECK((st.st_mode & ACCESSPERMS) == 0600);
	CHECK(st.st_uid == getuid());
	CHECK(st.st_dev == dev && st.st_ino == ino);

	CHECK((client = socket(AF_UNIX, SOCK_STREAM, 0)) != -1);
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	CHECK(strlcpy(sun.sun_path, path, sizeof(sun.sun_path)) <
	    sizeof(sun.sun_path));
	CHECK(connect(client, (struct sockaddr *)&sun, sizeof(sun)) == 0);
	close(client);

	display_socket_close(path, &fd, dev, ino, 1);
	CHECK(fd == -1);
	CHECK(lstat(path, &st) == -1 && errno == ENOENT);
}

static void
test_existing_paths(const char *dir)
{
	struct stat st;
	char path[PATH_MAX], target[PATH_MAX];
	dev_t dev;
	ino_t ino;
	int fd, filefd;

	make_path(path, sizeof(path), dir, "existing");
	CHECK((filefd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600)) != -1);
	close(filefd);
	errno = 0;
	CHECK(display_socket_open(path, getuid(), &fd, &dev, &ino) == -1);
	CHECK(errno == EEXIST);
	CHECK(lstat(path, &st) == 0 && S_ISREG(st.st_mode));
	CHECK(unlink(path) == 0);

	make_path(target, sizeof(target), dir, "target");
	CHECK(symlink(target, path) == 0);
	errno = 0;
	CHECK(display_socket_open(path, getuid(), &fd, &dev, &ino) == -1);
	CHECK(errno == EEXIST);
	CHECK(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
	CHECK(unlink(path) == 0);
}

static void
test_identity_cleanup(const char *dir)
{
	struct stat st;
	char oldpath[PATH_MAX], path[PATH_MAX];
	dev_t dev;
	ino_t ino;
	int fd, filefd;

	make_path(path, sizeof(path), dir, "identity.sock");
	make_path(oldpath, sizeof(oldpath), dir, "identity.old");
	CHECK(display_socket_open(path, getuid(), &fd, &dev, &ino) == 0);
	CHECK(rename(path, oldpath) == 0);
	CHECK((filefd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600)) != -1);
	close(filefd);

	display_socket_close(path, &fd, dev, ino, 1);
	CHECK(lstat(path, &st) == 0 && S_ISREG(st.st_mode));
	CHECK(unlink(path) == 0);
	CHECK(unlink(oldpath) == 0);
}

static void
test_unsafe_parent(const char *dir)
{
	char path[PATH_MAX];
	dev_t dev;
	ino_t ino;
	int fd;

	make_path(path, sizeof(path), dir, "unsafe.sock");
	CHECK(chmod(dir, 0777) == 0);
	errno = 0;
	CHECK(display_socket_open(path, getuid(), &fd, &dev, &ino) == -1);
	CHECK(errno == EPERM);
	CHECK(chmod(dir, 0700) == 0);
}

int
main(void)
{
	char dir[] = "/tmp/vmd-display.XXXXXXXX";

	test_paths();
	CHECK(mkdtemp(dir) != NULL);
	test_listener(dir);
	test_existing_paths(dir);
	test_identity_cleanup(dir);
	test_unsafe_parent(dir);
	CHECK(rmdir(dir) == 0);
	return (0);
}
