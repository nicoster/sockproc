#if __APPLE__
/* In Mac OS X 10.5 and later trying to use the daemon function gives a “‘daemon’ is deprecated”
** error, which prevents compilation because we build with "-Werror".
** Since this is supposed to be portable cross-platform code, we don't care that daemon is
** deprecated on Mac OS X 10.5, so we use this preprocessor trick to eliminate the error message.
*/
#define daemon yes_we_know_that_daemon_is_deprecated_in_os_x_10_5_thankyou
#endif

#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#if __APPLE__
#undef daemon
extern int daemon(int, int);
#endif

#define PIPE_READ 0
#define PIPE_WRITE 1

#define SHELL_BIN "/bin/sh"
#define SHELL_ARG "-c"

struct sockaddr_un addr;
struct sockaddr_in addr_in;

char *socket_path;
char *pid_file;

void proc_exit() {}

int forward(int from, int to){
	int size;
	char buf[BUFSIZ];

	if ((size = read(from, buf, BUFSIZ)) == -1) {
		printf("read err:~d\n", errno);
		exit(-1);
	}

	if (size == 0) {
		printf("EOF\n");
		exit(0);
	}

	return write(to, buf, size);
}


int create_worker(const char* cmd, char* const argv[], char* const env[], int sock)
{
	int stdin_pipe[2];
	int stdout_pipe[2];
	int fork_result;
	int result;
	pid_t child_pid;

	if (pipe(stdin_pipe) < 0) {
		printf("allocating pipe for child input redirect");
		return -1;
	}
	if (pipe(stdout_pipe) < 0) {
		close(stdin_pipe[PIPE_READ]);
		close(stdin_pipe[PIPE_WRITE]);
		printf("allocating pipe for child output redirect");
		return -1;
	}

	signal(SIGCHLD, proc_exit);

	fork_result = fork();

	if (fork_result == 0) {
		/* child continues here */
		if (dup2(stdin_pipe[PIPE_READ], STDIN_FILENO) == -1) {
			printf("dup2: stdin");
			return -1;
		}
		if (dup2(stdout_pipe[PIPE_WRITE], STDOUT_FILENO) == -1) {
			printf("dup2: stdout");
			return -1;
		}
		if (dup2(stdout_pipe[PIPE_WRITE], STDERR_FILENO) == -1) {
			printf("dup2: stderr");
			return -1;
		}

		/* all these are for use by parent only */
		close(stdin_pipe[PIPE_READ]);
		close(stdin_pipe[PIPE_WRITE]);
		close(stdout_pipe[PIPE_READ]);
		close(stdout_pipe[PIPE_WRITE]);

		/* run child process image */
		result = execve(cmd, argv, env);

		/* if we got here, then an error had occurred */
		perror("should not get here");
		exit(result);

	} else if (fork_result > 0) {
		/* parent continues here */
		child_pid = fork_result;

		/* close unused file descriptors, these are for child only */
		close(stdin_pipe[PIPE_READ]);
		close(stdout_pipe[PIPE_WRITE]);

		fd_set readfds;
		#define MAX(a, b) (a > b ? a : b)
		int fdset_width = MAX(sock, stdout_pipe[PIPE_READ]) + 1;
		int selret = 0;

		// printf("stdout-pipe-read:%d socket-read:%d fdset-width:%d\n", stdout_pipe[PIPE_READ], sock, fdset_width);

		while (1) {

			FD_ZERO(&readfds);
			FD_SET(stdout_pipe[PIPE_READ], &readfds);
			FD_SET(sock, &readfds);

			do {
				selret = select(fdset_width, &readfds, 0, 0, 0);
				if (selret < 0 && errno != EINTR) {
					printf("select:%d\n", selret);
					exit(1);
				}
			} while(selret <= 0);

			if (FD_ISSET(sock, &readfds)) {
				forward(sock, stdin_pipe[PIPE_WRITE]);
			}

			if (FD_ISSET(stdout_pipe[PIPE_READ], &readfds)) {
				forward(stdout_pipe[PIPE_READ], sock);
			}

		}
		
		/* close file descriptors */
		close(stdin_pipe[PIPE_WRITE]);
		close(stdout_pipe[PIPE_READ]);
		close(sock);

	} else {
		/* failed to create child. Not much to do at this point
		 * since we don't log anything */
		close(stdin_pipe[PIPE_READ]);
		close(stdin_pipe[PIPE_WRITE]);
		close(stdout_pipe[PIPE_READ]);
		close(stdout_pipe[PIPE_WRITE]);
	}

	return fork_result;
}

void terminate(int sig)
{
	/* remove unix-socket-path */
	if (socket_path != NULL) {
		unlink(socket_path);
	}

	/* remove pid_file */
	if (pid_file != NULL) {
		unlink(pid_file);
	}

	/* restore and raise signals */
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(int argc, char *argv[], char *envp[])
{
	int i, fd, cl, rc;
	char buf[2048];
	char *p, *end, *bc;
	int count;
	char *child_argv[4];
	int port;
	FILE* f;
	int daemonize = 1;
	int reuseaddr = 1;

	if (argc < 2 || (argc >= 2 && argv[1][0] == '-')) {
		printf("Usage: %s (<unix-socket-path>|<tcp-port>) {pidfile} [--foreground]\n", argv[0]);
		return 2;
	}

	pid_file = NULL;
	socket_path = strdup(argv[1]);

	if (sscanf(socket_path, "%d", &port) == 1) {
		socket_path = NULL;
		/* tcp socket on localhost interface */
		fd = socket(AF_INET, SOCK_STREAM, 0);
		memset(&addr_in, 0, sizeof(addr_in));
		addr_in.sin_family = AF_INET;
		addr_in.sin_port = htons(port);
		addr_in.sin_addr.s_addr = inet_addr("127.0.0.1");

		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int));

		if (bind(fd, (struct sockaddr*)&addr_in, sizeof(addr_in)) == -1) {
			perror("bind error");
			return errno;
		}
	}
	else {
		if (access(socket_path, X_OK) != -1) {
			errno = EEXIST;
			perror("socket_path error");
			return errno;
		}
		/* unix domain socket */
		unlink(socket_path);
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);

		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int));

		if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
			perror("bind error");
			return errno;
		}
	}

	if (listen(fd, 32) == -1) {
		perror("listen error");
		return errno;
	}

	/* check foreground flag */
	for (i=2; i<=argc-1; i++) {
		if (strcmp(argv[i], "--foreground")==0) {
			daemonize = 0;
			break;
		}
	}

	if (daemonize) {
	   daemon(0, 0);
	}

	if (argc > 2) {
		if (strcmp(argv[2], "--foreground")==0) {
			if (argc > 3) {
				pid_file = strdup(argv[3]);
			}
		}
		else {
			pid_file = strdup(argv[2]);
		}

		/* write pid to a file, if asked to do so */
		f = fopen(pid_file, "w");
		if (f) {
			fprintf(f, "%d", getpid());
			fclose(f);
		}
	}

	signal(SIGCHLD, SIG_IGN);
	signal(SIGTERM, terminate);
	signal(SIGINT, terminate);

	while (1) {
		if ( (cl = accept(fd, NULL, NULL)) == -1) {
			perror("accept error");
			continue;
		}

		if (fork()==0) {
			/* child, this is the supervisor process */
			memset(buf, 0, sizeof(buf));
			p = bc = buf; count = sizeof(buf)-1;
			while (count > 0) {
				rc = read(cl, p, 1);
				if (rc == 0) {
					break;
				}
				if ((end = strstr(buf, "\r\n"))) {
					end[0] = '\0';
					bc = end + 2;
					break;
				}
				p += rc;
				count -= rc;
			}

			/* execute command */
			child_argv[0] = SHELL_BIN;
			child_argv[1] = SHELL_ARG;
			child_argv[2] = buf;
			child_argv[3] = 0;
			create_worker(child_argv[0], child_argv, envp, cl);
			close(cl);

			exit(0);
		}
		else {
			/* parent */
			close(cl);
		}
	}
}

