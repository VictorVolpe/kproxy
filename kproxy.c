#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/event.h>

struct conn_data
{
	int inSock;
	int outSock;
};

char				*argAddr;
int					argPort;
int					argConn;
int					argThrd;
int					sock;
int					kq;
struct conn_data	*connData;

void *AcceptThread();
void *WorkerThread();

int Create()
{
	struct sockaddr_in	sockAddr;
	int					reuseAddr = 1;

	if ((sock = socket(PF_INET, SOCK_STREAM, 0)) == -1)
	{
		fprintf(stderr, "socket(): %s\n", strerror(errno));

		return 0;
	}

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr)) == -1)
	{
		fprintf(stderr, "setsockopt(): %s\n", strerror(errno));
		close(sock);

		return 0;
	}

	memset(&sockAddr, 0, sizeof(sockAddr));

	sockAddr.sin_family = AF_INET;
	sockAddr.sin_port = htons(argPort);
	sockAddr.sin_addr.s_addr = inet_addr("0.0.0.0");

	if (bind(sock, (struct sockaddr *)&sockAddr, sizeof(sockAddr)) == -1)
	{
		fprintf(stderr, "bind(): %s\n", strerror(errno));
		close(sock);

		return 0;
	}

	if (listen(sock, 5) == -1)
	{
		fprintf(stderr, "listen(): %s\n", strerror(errno));
		close(sock);

		return 0;
	}

	return 1;
}

int Init()
{
	struct conn_data	*cd;
	int					res;
	pthread_t			acceptThread;
	pthread_t			workerThread[argThrd];

	kq = kqueue();

	if (kq == -1)
	{
		fprintf(stderr, "kqueue(): %s\n", strerror(errno));
		close(sock);

		return 0;
	}

	int i;

	for (i = 0; i < argConn; i++)
	{
		cd = (connData + i);

		cd->inSock	= -1;
		cd->outSock	= -1;
	}

	if ((res = pthread_create(&acceptThread, NULL, AcceptThread, NULL)) != 0)
	{
		fprintf(stderr, "pthread_create(): %s\n", strerror(res));
		close(sock);

		return 0;
	}

	for (i = 0; i < argThrd; i++)
	{
		if ((res = pthread_create(&workerThread[i], NULL, WorkerThread, NULL)) != 0)
		{
			fprintf(stderr, "pthread_create(): %s\n", strerror(res));
			close(sock);

			return 0;
		}
	}

	pthread_join(acceptThread, NULL);

	for (i = 0; i < argThrd; i++)
	{
		pthread_join(workerThread[i], NULL);
	}

	close(sock);

	fprintf(stdout, "All connections closed!\n");

	return 1;
}

void *AcceptThread()
{
	int					sockLen;
	struct sockaddr_in	sockAddr;
	int					sockDesc;
	int					connId;
	struct conn_data	*cd;
	struct sockaddr_in	sockAddrOut;
	struct kevent		kEvents[2];
	int					res;

	fprintf(stdout, "Accepting new connections...\n");

	while (1)
	{
		sockLen = sizeof(sockAddr);

		if ((sockDesc = accept(sock, (struct sockaddr *)&sockAddr, (socklen_t *)&sockLen)) == -1)
		{
			fprintf(stderr, "accept(): %s\n", strerror(errno));

			break;
		}

		if (fcntl(sockDesc, F_SETFL, O_NONBLOCK) == -1)
		{
			fprintf(stderr, "fcntl(): %s\n", strerror(errno));
			close(sockDesc);

			continue;
		}

		connId = -1;
		int i;

		for (i = 0; i < argConn; i++)
		{
			cd = (connData + i);

			if (cd->inSock == -1)
			{
				cd->inSock	= sockDesc;
				connId		= i;

				break;
			}
		}

		if (connId == -1)
		{
			close(sockDesc);

			continue;
		}

		cd = (connData + connId);

		if ((cd->outSock = socket(PF_INET, SOCK_STREAM, 0)) == -1)
		{
			fprintf(stderr, "socket(): %s\n", strerror(errno));

			close(cd->inSock);

			cd->inSock	= -1;
			cd->outSock	= -1;

			continue;
		}

		memset(&sockAddrOut, 0, sizeof(sockAddrOut));

		sockAddrOut.sin_family = AF_INET;
		sockAddrOut.sin_port = htons(argPort);
		sockAddrOut.sin_addr.s_addr = inet_addr(argAddr);

		if (connect(cd->outSock, (struct sockaddr *)&sockAddrOut, sizeof(sockAddrOut)) == -1)
		{
			fprintf(stderr, "connect(): %s\n", strerror(errno));

			close(cd->inSock);
			close(cd->outSock);

			cd->inSock	= -1;
			cd->outSock	= -1;

			continue;
		}

		EV_SET(&kEvents[0], cd->inSock, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, cd);
		EV_SET(&kEvents[1], cd->outSock, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, cd);

		if (kevent(kq, kEvents, 2, NULL, 0, NULL) == -1)
		{
			fprintf(stderr, "kevent(): %s\n", strerror(errno));

			break;
		}
	}

	pthread_exit(NULL);
}

void *WorkerThread()
{
	struct kevent		kEvent;
	int					kRet;
	int					res;
	struct conn_data	*cd;
	int					fd;
	char				buf[8192];
	size_t				bytesRead;

	while (1)
	{
		memset(&kEvent, 0, sizeof(kEvent));

		kRet = kevent(kq, NULL, 0, &kEvent, 1, NULL);

		if (kRet == -1)
		{
			fprintf(stderr, "kevent(): %s\n", strerror(errno));

			break;
		}

		if (kRet == 0)
		{
			continue;
		}

		cd = kEvent.udata;
		fd = (int)kEvent.ident;

		if (kEvent.flags & EV_EOF)
		{
			close(cd->inSock);
			close(cd->outSock);

			cd->inSock	= -1;
			cd->outSock	= -1;
		}
		else if (kEvent.filter == EVFILT_READ)
		{
			if (fd == cd->inSock)
			{
				bytesRead = recv(fd, buf, sizeof(buf), 0);
				send(cd->outSock, buf, bytesRead, 0);
			}
			else if (fd == cd->outSock)
			{
				bytesRead = recv(fd, buf, sizeof(buf), 0);
				send(cd->inSock, buf, bytesRead, 0);
			}
		}
	}

	pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
	if (argc < 5)
	{
		fprintf(stderr, "Usage: %s <remote ip> <port> <connection limit> <worker threads>\n", argv[0]);

		exit(0);
	}

	argAddr = argv[1];
	argPort = atoi(argv[2]);
	argConn = atoi(argv[3]);
	argThrd = atoi(argv[4]);

	if (Create() == 0)
	{
		exit(1);
	}

	struct conn_data cd[argConn];
	connData = &cd[0];

	if (Init())
	{
		exit(1);
	}

	return 0;
}