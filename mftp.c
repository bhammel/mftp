/*
 * File: mftp.c
 * Author: Brandon Hammel
 * Class: CS 176B, Winter 2016
 * Assignment: Homework 2
 */

#define _MULTI_THREADED

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>

enum {
	BUFSIZE = 1024
};

typedef struct _GlobalArgs {
	char *filename;
	char *file_to_open;
	char *hostname;
	int port;
	char *username;
	char *password;
	int active;
	char *mode;
	char *logfile;
	FILE *log;
	FILE *file;
	char *swarmfile;
	int num_bytes;
	int num_threads;

	int done;
	int downloaded_so_far;

	pthread_mutex_t fileLock;
	pthread_mutex_t logLock;
	pthread_mutex_t dsfLock;
} GlobalArgs;

typedef struct _FtpArgs {
	char *filename;
	char *hostname;
	char *username;
	char *password;
	int port;
	int thread_id;
} FtpArgs;

GlobalArgs *globalArgs;

void PrintUsage(FILE *fd) {
	fprintf(fd, "mftp - Swarming FTP client\n\n");

	fprintf(fd, "Usage:\n");
	fprintf(fd, "  mftp (-s | --server) hostname (-f | --file) file [options]\n");
	fprintf(fd, "  mftp (-w | --swarm) swarm_config_file [-b num_bytes] [options]\n");
	fprintf(fd, "  mftp -h | --help\n");
	fprintf(fd, "  mftp -v | --version\n\n");

	fprintf(fd, "Options:\n");
	fprintf(fd, "  -a, --active               Forces active behavior [default: passive]\n");
	fprintf(fd, "  -l, --log <logfile>        Logs all the FTP commands exchanged with server to file\n");
	fprintf(fd, "  -m, --mode <mode>          Specifies the mode to be used for the transfer [default: binary]\n");
	fprintf(fd, "  -n, --username <username>  Specifies the username to use when logging into the FTP server [default: anonymous]\n");
	fprintf(fd, "  -p, --port <port>          Specifies the port to be used when contacting the server [default: 21]\n");
	fprintf(fd, "  -P, --password <password>  Specifies the password to use when logging into the FTP server [default: user@localhost.localnet]\n\n");

	exit(0);
}

void PrintVersion() {
	printf("mftp 0.1\n");
	printf("Copyright (C) 2016 Brandon Hammel\n");
	printf("This is free software; you are free to change and redistribute it.\n");
	printf("There is NO WARRANTY, to the extent permitted by law.\n\n");

	exit(0);
}

void PrintAndExit(int status, char *message) {
	char exitMessage[64];

	if (message != NULL) {
		fprintf(stderr, message);
	}

	if (globalArgs->log != NULL) {
		fclose(globalArgs->log);
	}

	switch (status) {
		case 0:
			strcpy(exitMessage, "Operation successfully completed.");
			break;
		case 1:
			sprintf(exitMessage, "Error (%d): Can't connect to server", status);
			break;
		case 2:
			sprintf(exitMessage, "Error (%d): Authentication failed", status);
			break;
		case 3:
			sprintf(exitMessage, "Error (%d): File not found", status);
			break;
		case 4:
			sprintf(exitMessage, "Error (%d): Syntax error in client request", status);
			break;
		case 5:
			sprintf(exitMessage, "Error (%d): Command not implemented by server", status);
			break;
		case 6:
			sprintf(exitMessage, "Error (%d): Operation not allowed by server", status);
			break;
		case 7:
			sprintf(exitMessage, "Error (%d): Generic error", status);
			break;
		default:
			strcpy(exitMessage, "Unknown error");
			break;
	}

	if (status != 0) {
		unlink(globalArgs->file_to_open);
	}

	fprintf(stderr, "%s\n", exitMessage);
	exit(status);
}

int SubstringAfter(char *out, char str[], char token, int count) {
	int i;
	int token_count = 0;

	for (i = 0; i < strlen(str); i++) {
		if (str[i] == token) {
			token_count++;
		}

		if (i == strlen(str) - 1) {
			out = "";
			return 1;
		}

		if (token_count == count) {
			strcpy(out, &str[i + 1]);
			return 0;
		}
	}

	out = "";
	return -1;
}

void CheckForErrorResponse(char response[]) {
	long int response_code = strtol(response, NULL, 10);
	char *resp = (char *) malloc((strlen(response) + 1) * sizeof(char));
	strcpy(resp, response);

	int i;
	for (i = 0; i < strlen(resp); i++) {
		if (resp[i] == '\n') {
			resp[i + 1] = '\0';
			break;
		}
	}

	switch (response_code) {
		case 202:
			PrintAndExit(5, resp);
			break;
		case 332:
			PrintAndExit(2, resp);
			break;
		case 421:
			PrintAndExit(1, resp);
			break;
		case 425:
			PrintAndExit(1, resp);
			break;
		case 450:
			PrintAndExit(3, resp);
			break;
		case 451:
			PrintAndExit(7, resp);
			break;
		case 500:
			PrintAndExit(4, resp);
			break;
		case 501:
			PrintAndExit(4, resp);
			break;
		case 502:
			PrintAndExit(5, resp);
			break;
		case 503:
			PrintAndExit(4, resp);
			break;
		case 504:
			PrintAndExit(5, resp);
			break;
		case 530:
			PrintAndExit(2, resp);
			break;
		case 550:
			PrintAndExit(6, resp);
			break;
		case 551:
			PrintAndExit(3, resp);
			break;
		case 552:
			PrintAndExit(3, resp);
			break;
		case 553:
			PrintAndExit(3, resp);
			break;
	}

	free(resp);
}

void LogRead(int socket, char recvBuf[], int thread_id) {
	int n = read(socket, recvBuf, BUFSIZE - 1);

	if (n < 0) {
		PrintAndExit(7, "Error: Failed to read from socket\n");
	}

	CheckForErrorResponse(recvBuf);

	recvBuf[n] = '\0';

	if (globalArgs->log != NULL) {
		if (globalArgs->num_threads > 1) {
			pthread_mutex_lock(&globalArgs->logLock);
			fprintf(globalArgs->log, "Thread %d -- S->C: %s", thread_id, recvBuf);
			pthread_mutex_unlock(&globalArgs->logLock);
		} else {
			pthread_mutex_lock(&globalArgs->logLock);
			fprintf(globalArgs->log, "S->C: %s", recvBuf);
			pthread_mutex_unlock(&globalArgs->logLock);
		}
	}
}

void LogWrite(int socket, char sendBuf[], int thread_id) {
	int n = write(socket, sendBuf, strlen(sendBuf));

	if (n < 0) {
		PrintAndExit(7, "Error: Failed to write to socket\n");
	}

	if (globalArgs->log != NULL) {
		if (globalArgs->num_threads > 1) {
			pthread_mutex_lock(&globalArgs->logLock);
			fprintf(globalArgs->log, "Thread %d -- C->S: %s", thread_id, sendBuf);
			pthread_mutex_unlock(&globalArgs->logLock);
		} else {
			pthread_mutex_lock(&globalArgs->logLock);
			fprintf(globalArgs->log, "C->S: %s", sendBuf);
			pthread_mutex_unlock(&globalArgs->logLock);
		}
	}
}

int FindPassivePort(char str[]) {
	char b[128];
	char e[128];
	int offset;
	int second_last, last;

	memset(b, 0, sizeof(b));
	memset(e, 0, sizeof(e));

	SubstringAfter(b, str, ',', 4);
	SubstringAfter(e, str, ',', 5);

	offset = strlen(b) - strlen(e) - 1;

	b[offset] = '\0';
	second_last = atoi(b);

	SubstringAfter(b, str, ',', 5);
	SubstringAfter(e, str, ')', 1);

	offset = strlen(b) - strlen(e) - 1;

	b[offset] = '\0';
	last = atoi(b);

	return (second_last * 256 + last);
}

int ConnectSocket(char hostname[], int port) {
	int socket_fd;
	struct hostent *host_ent;
	struct sockaddr_in server_address;

	memset(&server_address, 0, sizeof(server_address));

	socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_fd < 0) {
		PrintAndExit(1, "Error: Failed to create socket\n");
	}

	host_ent = gethostbyname(hostname);
	if (host_ent == NULL) {
		char message[64];
		sprintf(message, "Error: Failed to find host %s\n", hostname);
		PrintAndExit(1, message);
	}

	memcpy(&server_address.sin_addr, host_ent->h_addr_list[0], host_ent->h_length);

	server_address.sin_family = AF_INET;
	server_address.sin_port = htons(port);

	if (connect(socket_fd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
		PrintAndExit(1, "Error: Failed to connect socket\n");
	}

	return socket_fd;
}

int ConnectToMessageSocket(int conn_socket) {
	int message_socket;
	struct sockaddr_in client_address;
	socklen_t client_length;

	listen(conn_socket, 5);
	client_length = sizeof(client_address);

	message_socket = accept(conn_socket, (struct sockaddr *)&client_address, &client_length);
	if (message_socket < 0) {
		PrintAndExit(1, "Error: Failed to accept socket connection\n");
	}

	return message_socket;
}

int OpenServerSocket() {
	int socket_fd;
	struct sockaddr_in server_address;

	memset(&server_address, 0, sizeof(server_address));

	socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_fd < 0) {
		PrintAndExit(1, "Error: Failed to create socket\n");
	}

	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = INADDR_ANY;
	server_address.sin_port = htons(0);

	if (bind(socket_fd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
		PrintAndExit(1, "Error: Failed to bind socket\n");
	}

	return socket_fd;
}

void GetPortString(char out[], int conn_socket) {
	char hostname[512];
	struct hostent *host_ent;
	struct sockaddr_in tmp_socket;
	int i, port;
	socklen_t tmp_socket_length;

	tmp_socket_length = sizeof(tmp_socket);
	getsockname(conn_socket, (struct sockaddr *)&tmp_socket, &tmp_socket_length);
	port = ntohs(tmp_socket.sin_port);

	if (gethostname(hostname, sizeof(hostname)) == 0) {
		host_ent = gethostbyname(hostname);
		if (host_ent == NULL) {
			PrintAndExit(1, "Error: Failed to find localhost\n");
		}
		sprintf(hostname, "%s", inet_ntoa(*(struct in_addr *)host_ent->h_addr));

		for (i = 0; i < strlen(hostname); i++) {
			if (hostname[i] == '.') {
				hostname[i] = ',';
			}
		}

		sprintf(out, "PORT %s,%d,%d\r\n", hostname, port / 256, port % 256);
	} else {
		PrintAndExit(2, "Error: Failed to get local hostname\n");
	}
}

void SetASCIIMode(int control_socket, char sendBuf[], char recvBuf[], int thread_id) {
	strcpy(sendBuf, "TYPE A\r\n");

	LogWrite(control_socket, sendBuf, thread_id);
	LogRead(control_socket, recvBuf, thread_id);
}

void RetrieveFile(FtpArgs *ftpArgs, char sendBuf[], char recvBuf[], int control_socket) {
	int file_socket, conn_socket;
	int filesize = 0;
	int bytes_to_download = 0;
	int received = 0;
	int starting_position = 0;
	int n;
	unsigned char fileRecv[BUFSIZE];

	memset(fileRecv, 0, sizeof(fileRecv));

	sprintf(sendBuf, "SIZE %s\r\n", ftpArgs->filename);
	LogWrite(control_socket, sendBuf, ftpArgs->thread_id);
	LogRead(control_socket, recvBuf, ftpArgs->thread_id);

	SubstringAfter(recvBuf, recvBuf, ' ', 1);

	filesize = atoi(recvBuf);
	starting_position = ceil(filesize / globalArgs->num_threads) * ftpArgs->thread_id;

	if (ftpArgs->thread_id != globalArgs->num_threads - 1) {
		bytes_to_download = ceil(filesize / globalArgs->num_threads);
	} else {
		bytes_to_download = filesize - ceil(filesize / globalArgs->num_threads) * (globalArgs->num_threads - 1);
	}

	if (strcmp(globalArgs->mode, "ASCII") == 0) {
		SetASCIIMode(control_socket, sendBuf, recvBuf, ftpArgs->thread_id);
	}

	if (globalArgs->active) {
		char portStr[64];
		conn_socket = OpenServerSocket();

		GetPortString(portStr, conn_socket);
		LogWrite(control_socket, portStr, ftpArgs->thread_id);
		LogRead(control_socket, recvBuf, ftpArgs->thread_id);

		sprintf(sendBuf, "REST %d\r\n", starting_position);
		LogWrite(control_socket, sendBuf, ftpArgs->thread_id);
		LogRead(control_socket, recvBuf, ftpArgs->thread_id);

		sprintf(sendBuf, "RETR %s\r\n", ftpArgs->filename);
		LogWrite(control_socket, sendBuf, ftpArgs->thread_id);

		file_socket = ConnectToMessageSocket(conn_socket);
		LogRead(control_socket, recvBuf, ftpArgs->thread_id);
	} else {
		int port;

		strcpy(sendBuf, "PASV\r\n");
		LogWrite(control_socket, sendBuf, ftpArgs->thread_id);
		LogRead(control_socket, recvBuf, ftpArgs->thread_id);

		port = FindPassivePort(recvBuf);
		file_socket = ConnectSocket(ftpArgs->hostname, port);

		sprintf(sendBuf, "REST %d\r\n", starting_position);
		LogWrite(control_socket, sendBuf, ftpArgs->thread_id);
		LogRead(control_socket, recvBuf, ftpArgs->thread_id);

		sprintf(sendBuf, "RETR %s\r\n", ftpArgs->filename);
		LogWrite(control_socket, sendBuf, ftpArgs->thread_id);
		LogRead(control_socket, recvBuf, ftpArgs->thread_id);
	}

	while (received < bytes_to_download) {
		n = read(file_socket, fileRecv, BUFSIZE - 1);

		pthread_mutex_lock(&globalArgs->fileLock);
		fseek(globalArgs->file, starting_position + received, SEEK_SET);
		fwrite(fileRecv, sizeof(unsigned char), n, globalArgs->file);
		pthread_mutex_unlock(&globalArgs->fileLock);

		received += n;
	}

	close(file_socket);
	if (globalArgs->active) {
		close(conn_socket);
	}

	LogRead(control_socket, recvBuf, ftpArgs->thread_id);
}

void RetrieveFileSegmented(FtpArgs *ftpArgs, char sendBuf[], char recvBuf[], int control_socket) {
	int file_socket, conn_socket;
	int filesize = 0;
	int bytes_to_download = 0;
	int received = 0;
	int starting_position = 0;
	int n;
	unsigned char fileRecv[BUFSIZE];

	memset(fileRecv, 0, sizeof(fileRecv));

	sprintf(sendBuf, "SIZE %s\r\n", ftpArgs->filename);
	LogWrite(control_socket, sendBuf, ftpArgs->thread_id);
	LogRead(control_socket, recvBuf, ftpArgs->thread_id);

	SubstringAfter(recvBuf, recvBuf, ' ', 1);
	filesize = atoi(recvBuf);

	while (globalArgs->done == 0) {
		pthread_mutex_lock(&globalArgs->dsfLock);

		if (globalArgs->downloaded_so_far >= filesize) {
			pthread_mutex_unlock(&globalArgs->dsfLock);
			pthread_exit(NULL);
		}

		if (globalArgs->downloaded_so_far + globalArgs->num_bytes > filesize) {
			bytes_to_download = filesize - globalArgs->downloaded_so_far;
		} else {
			bytes_to_download = globalArgs->num_bytes;
		}

		starting_position = globalArgs->downloaded_so_far;
		globalArgs->downloaded_so_far += bytes_to_download;

		if (globalArgs->downloaded_so_far >= filesize) {
			globalArgs->done = 1;
		}

		pthread_mutex_unlock(&globalArgs->dsfLock);

		if (globalArgs->active) {
			char portStr[64];
			conn_socket = OpenServerSocket();

			GetPortString(portStr, conn_socket);
			LogWrite(control_socket, portStr, ftpArgs->thread_id);
			LogRead(control_socket, recvBuf, ftpArgs->thread_id);

			sprintf(sendBuf, "REST %d\r\n", starting_position);
			LogWrite(control_socket, sendBuf, ftpArgs->thread_id);
			LogRead(control_socket, recvBuf, ftpArgs->thread_id);

			sprintf(sendBuf, "RETR %s\r\n", ftpArgs->filename);
			LogWrite(control_socket, sendBuf, ftpArgs->thread_id);

			file_socket = ConnectToMessageSocket(conn_socket);
			LogRead(control_socket, recvBuf, ftpArgs->thread_id);
		} else {
			int port;

			strcpy(sendBuf, "PASV\r\n");
			LogWrite(control_socket, sendBuf, ftpArgs->thread_id);
			LogRead(control_socket, recvBuf, ftpArgs->thread_id);

			port = FindPassivePort(recvBuf);
			file_socket = ConnectSocket(ftpArgs->hostname, port);

			sprintf(sendBuf, "REST %d\r\n", starting_position);
			LogWrite(control_socket, sendBuf, ftpArgs->thread_id);
			LogRead(control_socket, recvBuf, ftpArgs->thread_id);

			sprintf(sendBuf, "RETR %s\r\n", ftpArgs->filename);
			LogWrite(control_socket, sendBuf, ftpArgs->thread_id);
			LogRead(control_socket, recvBuf, ftpArgs->thread_id);
		}

		received = 0;
		while (received < bytes_to_download) {
			n = read(file_socket, fileRecv, BUFSIZE - 1);

			pthread_mutex_lock(&globalArgs->fileLock);
			fseek(globalArgs->file, starting_position + received, SEEK_SET);
			fwrite(fileRecv, sizeof(unsigned char), n, globalArgs->file);
			pthread_mutex_unlock(&globalArgs->fileLock);

			received += n;
		}

		close(file_socket);
		if (globalArgs->active) {
			close(conn_socket);
		}

		LogRead(control_socket, recvBuf, ftpArgs->thread_id);
	}
}

void Authenticate(FtpArgs *ftpArgs, int socket, char recvBuf[], char sendBuf[]) {
	LogRead(socket, recvBuf, ftpArgs->thread_id);

	if (strncmp(recvBuf, "220", 3) == 0) {
		sprintf(sendBuf, "USER %s\r\n", ftpArgs->username);
		LogWrite(socket, sendBuf, ftpArgs->thread_id);
	}

	LogRead(socket, recvBuf, ftpArgs->thread_id);

	if (strncmp(recvBuf, "331", 3) == 0) {
		sprintf(sendBuf, "PASS %s\r\n", ftpArgs->password);
		LogWrite(socket, sendBuf, ftpArgs->thread_id);
	}

	LogRead(socket, recvBuf, ftpArgs->thread_id);

	while (strncmp(recvBuf, "230-", 4) == 0) {
		int s;
		int end = 0;

		while ((s = SubstringAfter(recvBuf, recvBuf, '\n', 1)) == 0) {
			if (strncmp(recvBuf, "230 ", 4) == 0) {
				end = 1;
				break;
			}
		}

		if (end) {
			break;
		}

		LogRead(socket, recvBuf, ftpArgs->thread_id);
	}

	if (strncmp(recvBuf, "230", 3) != 0) {
		PrintAndExit(2, NULL);
	}
}

void SetBinaryMode(int control_socket, char sendBuf[], char recvBuf[], int thread_id) {
	strcpy(sendBuf, "TYPE I\r\n");

	LogWrite(control_socket, sendBuf, thread_id);
	LogRead(control_socket, recvBuf, thread_id);
}

void *DownloadThread(void *arg) {
	int control_socket;
	char recvBuf[BUFSIZE], sendBuf[BUFSIZE];

	memset(recvBuf, 0, sizeof(recvBuf));
	memset(sendBuf, 0, sizeof(sendBuf));

	FtpArgs *ftpArgs = (FtpArgs *)arg;

	control_socket = ConnectSocket(ftpArgs->hostname, ftpArgs->port);

	Authenticate(ftpArgs, control_socket, recvBuf, sendBuf);
	SetBinaryMode(control_socket, sendBuf, recvBuf, ftpArgs->thread_id);

	if (globalArgs->num_bytes > 0) {
		RetrieveFileSegmented(ftpArgs, sendBuf, recvBuf, control_socket);
	} else {
		RetrieveFile(ftpArgs, sendBuf, recvBuf, control_socket);
	}

	close(control_socket);

	return NULL;
}

int IsNumber(char *str) {
	int i;
	for (i = 0; i < strlen(str); i++) {
		if (!isdigit((int)str[i])) {
			return 0;
		}
	}

	return 1;
}

int ValidatePort(char *port) {
	if (IsNumber(port)) {
		return atoi(port);
	} else {
		char message[64];
		sprintf(message, "Error: Invalid port - %s\n", port);
		PrintAndExit(7, message);
	}

	return -1;
}

int ValidateNumBytes(char *num_bytes) {
	if (IsNumber(num_bytes) && atoi(num_bytes) > 0) {
		return atoi(num_bytes);
	} else {
		char message[64];
		sprintf(message, "Error: Invalid number of bytes - %s\n", num_bytes);
		PrintAndExit(7, message);
	}

	return -1;
}

char *ValidateMode(char *mode) {
	if (strcmp(mode, "binary") == 0) {
		return mode;
	} else if (strcmp(mode, "ASCII") == 0) {
		return mode;
	} else {
		char message[128];
		sprintf(message, "Error: Invalid mode - %s\n", mode);
		PrintAndExit(7, message);
	}

	return "binary";
}

char peek(FILE *fp) {
	char c = fgetc(fp);
	ungetc(c, fp);

	return c;
}

int GetNumLinesInFile(FILE *fp) {
	char c;
	int lines = 0;

	while ((c = fgetc(fp)) != EOF) {
		if (c == '\n') {
			lines++;
		}

		if ((peek(fp) == EOF) && (c != '\n')) {
			lines++;
		}
	}

	rewind(fp);
	return lines;
}

void ParseSwarmfile(char out[], char opt, char line[]) {
	char b[512];
	char e[512];

	memset(b, 0, sizeof(b));
	memset(e, 0, sizeof(e));

	switch (opt) {
		case 'u':
			SubstringAfter(b, line, '/', 2);
			SubstringAfter(e, b, ':', 1);
			break;
		case 'p':
			SubstringAfter(b, line, ':', 2);
			SubstringAfter(e, b, '@', 1);
			break;
		case 'h':
			SubstringAfter(b, line, '@', 1);
			SubstringAfter(e, b, '/', 1);
			break;
		case 'f':
			SubstringAfter(b, line, '/', 3);
			SubstringAfter(e, b, '\n', 1);
			break;
	}

	b[strlen(b) - strlen(e) - 1] = '\0';

	if (b[strlen(b) - 1] == '\n') {
		b[strlen(b) - 1] = '\0';
	}

	sprintf(out, "%s", b);
}

void InitGlobalArgs() {
	globalArgs = (GlobalArgs *) malloc(sizeof(GlobalArgs));
	if (globalArgs == NULL) {
		PrintAndExit(7, NULL);
	}

	globalArgs->filename = NULL;
	globalArgs->file_to_open = NULL;
	globalArgs->hostname = NULL;
	globalArgs->port = 21;
	globalArgs->username = "anonymous";
	globalArgs->password = "user@localhost.localnet";
	globalArgs->active = 0;
	globalArgs->mode = "binary";
	globalArgs->logfile = NULL;
	globalArgs->swarmfile = NULL;
	globalArgs->num_bytes = 0;
	globalArgs->num_threads = 1;

	globalArgs->done = 0;
	globalArgs->downloaded_so_far = 0;

	pthread_mutex_init(&globalArgs->fileLock, NULL);
	pthread_mutex_init(&globalArgs->logLock, NULL);
	pthread_mutex_init(&globalArgs->dsfLock, NULL);
}

void FreeGlobalArgs() {
	fclose(globalArgs->file);
	free(globalArgs->file_to_open);
	free(globalArgs);
}

void PrintGlobalArgs() {
	if (globalArgs != NULL) {
		printf("filename: %s\n", globalArgs->filename);
		printf("file_to_open: %s\n", globalArgs->file_to_open);
		printf("hostname: %s\n", globalArgs->hostname);
		printf("port: %d\n", globalArgs->port);
		printf("username: %s\n", globalArgs->username);
		printf("password: %s\n", globalArgs->password);
		printf("active: %d\n", globalArgs->active);
		printf("mode: %s\n", globalArgs->mode);
		printf("logfile: %s\n", globalArgs->logfile);
		printf("swarmfile: %s\n", globalArgs->swarmfile);
		printf("num_bytes: %d\n", globalArgs->num_bytes);
		printf("num_threads: %d\n", globalArgs->num_threads);
	}
}

#define ARGS "hvf:s:p:n:P:am:l:w:b:"

int main(int argc, char **argv) {
	int opt, i;
	char tmp[128];

	if (argc == 1) {
		PrintUsage(stdout);
	}

	InitGlobalArgs();

	static struct option long_options[] = {
		{"help",		no_argument,		NULL,	'h'},
		{"version",		no_argument,		NULL,	'v'},
		{"file",		required_argument,	NULL,	'f'},
		{"server",		required_argument,	NULL,	's'},
		{"port",		required_argument,	NULL,	'p'},
		{"username",	required_argument,	NULL,	'n'},
		{"password",	required_argument,	NULL,	'P'},
		{"active",		no_argument,		NULL,	'a'},
		{"mode",		required_argument,	NULL,	'm'},
		{"log",			required_argument,	NULL,	'l'},
		{"swarm",		required_argument,	NULL,	'w'},
		{NULL,			no_argument,		NULL,	0}
	};

	while ((opt = getopt_long(argc, argv, ARGS, long_options, NULL)) != EOF) {
		switch (opt) {
			case 'h':
				PrintUsage(stdout);
				break;
			case 'v':
				PrintVersion();
				break;
			case 'f':
				globalArgs->filename = optarg;
				break;
			case 's':
				globalArgs->hostname = optarg;
				break;
			case 'p':
				globalArgs->port = ValidatePort(optarg);
				break;
			case 'n':
				globalArgs->username = optarg;
				break;
			case 'P':
				globalArgs->password = optarg;
				break;
			case 'a':
				globalArgs->active = 1;
				break;
			case 'm':
				globalArgs->mode = ValidateMode(optarg);
				break;
			case 'l':
				if (strcmp(optarg, "-") == 0) {
					globalArgs->log = stdout;
					break;
				}

				globalArgs->logfile = optarg;
				globalArgs->log = fopen(globalArgs->logfile, "w");
				if (globalArgs->log == NULL) {
					PrintAndExit(7, "Error: Failed to create logfile\n");
				}
				break;
			case 'w':
				globalArgs->swarmfile = optarg;
				break;
			case 'b':
				globalArgs->num_bytes = ValidateNumBytes(optarg);
				break;
			case '?':
				PrintUsage(stderr);
				break;
			default:
				break;
		}
	}

	if ((globalArgs->filename == NULL || globalArgs->hostname == NULL) && globalArgs->swarmfile == NULL) {
		PrintUsage(stderr);
	}

	FtpArgs *thread_args;
	if (globalArgs->swarmfile == NULL) {
		thread_args = (FtpArgs *) malloc(sizeof(FtpArgs));
		if (thread_args == NULL) {
			PrintAndExit(7, NULL);
		}

		thread_args[0].filename = globalArgs->filename;
		thread_args[0].hostname = globalArgs->hostname;
		thread_args[0].username = globalArgs->username;
		thread_args[0].password = globalArgs->password;
		thread_args[0].port = globalArgs->port;
		thread_args[0].thread_id = 0;
	} else {
		FILE *fp = fopen(globalArgs->swarmfile, "r");
		if (fp == NULL) {
			PrintAndExit(3, "Error: Failed to open swarm config file\n");
		}

		int lines = GetNumLinesInFile(fp);
		globalArgs->num_threads = lines;

		thread_args = (FtpArgs *) malloc(globalArgs->num_threads * sizeof(FtpArgs));
		if (thread_args == NULL) {
			PrintAndExit(7, NULL);
		}

		for (i = 0; i < globalArgs->num_threads; i++) {
			char tmpAttr[256];
			char line[256];

			fgets(line, sizeof(line), fp);

			if ((i == lines - 1) && (line[strlen(line) - 1] != '\n')) {
				strcat(line, "\n");
			}

			if (strlen(line) < 5) {
				continue;
			}

			ParseSwarmfile(tmpAttr, 'u', line);
			thread_args[i].username = (char *) malloc((strlen(tmpAttr) + 1) * sizeof(char));
			strcpy(thread_args[i].username, tmpAttr);

			ParseSwarmfile(tmpAttr, 'p', line);
			thread_args[i].password = (char *) malloc((strlen(tmpAttr) + 1) * sizeof(char));
			strcpy(thread_args[i].password, tmpAttr);

			ParseSwarmfile(tmpAttr, 'h', line);
			thread_args[i].hostname = (char *) malloc((strlen(tmpAttr) + 1) * sizeof(char));
			strcpy(thread_args[i].hostname, tmpAttr);

			ParseSwarmfile(tmpAttr, 'f', line);
			thread_args[i].filename = (char *) malloc((strlen(tmpAttr) + 1) * sizeof(char));
			strcpy(thread_args[i].filename, tmpAttr);

			if (strncmp(thread_args[i].filename, "/", 1) != 0) {
				strcpy(tmpAttr, "/");
				strcat(tmpAttr, thread_args[i].filename);
				strcpy(thread_args[i].filename, tmpAttr);
			}

			thread_args[i].port = globalArgs->port;
			thread_args[i].thread_id = i;
		}

		globalArgs->filename = thread_args[0].filename;
		fclose(fp);
	}

	if ((globalArgs->num_threads > 1 || globalArgs->num_bytes > 0) && strcmp(globalArgs->mode, "ASCII") == 0) {
		PrintAndExit(6, "Error: Swarming only available in binary mode\n");
	}

	if (strncmp(globalArgs->filename, "/", 1) != 0) {
		strcpy(tmp, "/");
		strcat(tmp, globalArgs->filename);
		strcpy(globalArgs->filename, tmp);
	}

	globalArgs->file_to_open = (char *) malloc((strlen(globalArgs->filename) + 1) * sizeof(char));
	strcpy(globalArgs->file_to_open, globalArgs->filename);

	while (SubstringAfter(tmp, globalArgs->file_to_open, '/', 1) == 0) {
		strcpy(globalArgs->file_to_open, tmp);
	}

	globalArgs->file = fopen(globalArgs->file_to_open, "w");
	if (globalArgs->file == NULL) {
		PrintAndExit(7, "Error: Failed to create target file\n");
	}

	int err;
	pthread_t *thread_ids = (pthread_t *) malloc(globalArgs->num_threads * sizeof(pthread_t));
	if (thread_ids == NULL) {
		PrintAndExit(7, NULL);
	}

	for (i = 0; i < globalArgs->num_threads; i++) {
		err = pthread_create(&thread_ids[i], NULL, DownloadThread, (void *)&thread_args[i]);

		if (err) {
			char message[64];
			sprintf(message, "Error: Failed to create thread %d\n", i);
			PrintAndExit(7, message);
		}
	}

	for (i = 0; i < globalArgs->num_threads; i++) {
		err = pthread_join(thread_ids[i], NULL);

		if (err) {
			char message[64];
			sprintf(message, "Error: Failed to join thread %d\n", i);
			PrintAndExit(7, message);
		}
	}

	free(thread_args);
	free(thread_ids);
	FreeGlobalArgs();
	PrintAndExit(0, NULL);

	return 0;
}
