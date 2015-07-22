/*****************************************
 * Read QSee/Zmodo cameras               *
 * Forward stream to FIFO pipe           *
 * Author: Daniel Osborne                *
 * Based on IP Cam Viewer by Robert Chou *
 * License: Public Domain                *
 *****************************************/

/* Version history
 * 0.43 - 2015-06-12
 *       Added support for mEye compatible.
 * 0.42 - 2015-04-22
 *       Added support for Swann DVR8-4000 and compatible.
 * 0.41 - 2013-05-03
 *       Fixed -h option not displaying help.
 * 0.4 - 2013-04-21
 *       Add support for some Swann models
 *       Added Visionari patch by jasonblack
 *       Fix incorrect use of select timeout struct.
 * 0.3 - 2012-01-26
 *       Add support for DVR-8104UV/8114HV and CnM Classic 4 Cam DVR
 * 0.2 - 2011-11-12
 *       Got media port support working (for some models at least).
 *       Changed fork behavior to fix a bug, now parent only spawns children, it doesn't stream.
 * 0.1 - 2011-08-26
 *       Initial version, working mobile port support, but buggy
 */

// Compile: gcc -Wall zmodopipe.c -o zmodopipe

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdarg.h>

//typedef enum bool {false=0, true=1,} bool;

typedef enum CameraModel
{
	mobile = 1,	// Q-See/Swann/Zmodo DVR w/mobile port
	media,		// Q-See/Zmodo w/media port
	media_header,	// Q-See/Zmodo w/media port and header packet
	qt504,		// Q-See QT-504 compatible model
	dvr8104_mobile,	// Zmodo DVR-8104/8114
	cnmclassic,	// CnM Classic 4 Cam
	visionari,	// Visionari 4/8 channel DVR
	swannmedia,	// Swann media
	swanndvr8,  // Swann dvr8-4000
	meye,	// mEye compatible DVR
} CameraModel;

// Structure for logging into QSee/Zmodo DVR's mobile port
// Must be in network byte order
struct QSeeLoginMobile
{
	int	val1;		// 4 Set to 64
	int	val2;		// 4 Set to 0;
	short	val3;		// 2 Set to 41
	short	val4;		// 2 Set to 56
	char	user[32];	// 32 username field (len might be 24, not sure)
	char	pass[20];	// 20 password field
	short	ch;		// 2 Camera channel (0 index)
	short	val5;		// 2 unknown (reserved?)
};	// total size: 		   68 bytes

// Structure for logging into QSee/Zmodo DVR's media port
// Must be in network byte order
struct QSeeLoginMedia
{
	char valc[47];		// 47 bytes, special values
	char user[8];		//  8 username field
	char vals[26];		// 26 unknown values
	char pass[6];		//  6 password field
	char filler[420];	//420 Filler (more unknown)
};	// Total size:		  507 bytes

// Structure for logging into QSee QT-504
// There are two other structures, but simple byte arrays are used for those
struct QSee504Login
{
	char vala[32];		// 44 bytes, special values
	char user[8];		//  8 username field
	char valb[28];		// 28 unknown values
	char pass[6];		//  6 password field
	char valc[30];		// 30 more unknown
	char host[8];		//  8 Hostname, apparently (how big??)
	char filler[32];	// 32 Filler (more unknown)
};	// Total size:		  144 bytes

// Structure for logging into Zmodo DVR-8104
struct DVR8104MobileLogin
{
	char vala[60];		// 60 bytes
	char user[4];		//  4 username field(really 4?)
	char valb[28];		// 26 unknown values
	char pass[6];		//  6 password field
	char filler[18];	// 18 Filler (more unknown)
};	// Total size:		  116 bytes

// Structure for logging into CnM 4 Cam Classic CCTV
struct CnMClassicLogin
{
	char vala[40];		// 40 bytes
	char user[8];		//  8 username field
	char valb[24];		// 24 unknown values
	char pass[6];		//  6 password field
	char filler[422];	//422 Filler (more unknown)
};	// Total size:		  500 bytes

struct VisionariLogin
{
	char vala[60];		// 60 bytes
	char user[8];		//  4 username field
	char valb[24];		// 24 unknown values
	char pass[6];		//  6 password field
	char filler[18];	// 18 Filler (more unknown)
};	// Total size:		  112 bytes 

// Structure for logging into QSee/Zmodo DVR's media port
// Must be in network byte order
struct SwannLoginMedia
{
	char valc[47];		// 47 bytes, special values
	char user[8];		//  8 username field
	char vals[24];		// 24 unknown values
	char pass[6];		//  6 password field
	char filler[422];	//422 Filler (more unknown)
};	// Total size:		  507 bytes

// Structure for logging into Swann DVR8-4000
struct SwannDVR8
{
	char valc[20];		// 20 bytes, special values
	char user[32];		//  32 username field
	char pass[32];		//  32 password field
	char filler[4];	    // 4 Filler (more unknown)
};	// Total size:		  88 bytes

// Structure for logging into mEye compatible
struct mEye
{
	char valc[18];		// 18 bytes, special values
	char user[20];		//  20 username field
	char pass[20];		//  20 password field
};	// Total size:		  58 bytes

#define MAX_CHANNELS 16		// maximum channels to support (I've only seen max of 16).

struct globalArgs_t {
	bool verbose;			// -v duh
	char *pipeName;			// -n name to use for filename (ch # will be appended)
	bool channel[MAX_CHANNELS];     // -c (support up to 16)
	char *hostname;			// -s hostname to connect to
	unsigned short port;		// -p port number
	CameraModel model;		// -m model to use
	char *username;			// -u login username
	char *password;			// -a login password
	int timer;			// -t alarm timer
} globalArgs = {0};

extern char *optarg;
const char *optString = "vn:c:p:s:m:u:a:t:h?";
int g_childPids[MAX_CHANNELS] = {0};
int g_cleanUp = false;
char g_errBuf[256];	// This will contain the error message for perror calls
int g_processCh = -1;	// Channel this process will be in charge of (-1 means parent)

void sigHandler(int sig);
void display_usage(char *name);
int printMessage(bool verbose, const char *message, ...);
int ConnectViaMobile(int sockFd, int channel);
int ConnectViaMedia(int sockFd, int channel);
int ConnectQT504(int sockFd, int channel);
int ConnectDVR8104ViaMobile(int sockFd, int channel);
int ConnectCnMClassic(int sockFd, int channel);
int ConnectSwannViaMedia(int sockFd, int channel);
int ConnectSwannDVR8(int sockFd, int channel);
int ConnectMEYE(int sockFd, int channel);
int ConnectVisionari(int sockFd, int channel);

// Function pointer list
int (*pConnectFunc[])(int, int) = {
	NULL,
	ConnectViaMobile,
	ConnectViaMedia,
	ConnectViaMedia,
	ConnectQT504,
	ConnectDVR8104ViaMobile,
	ConnectCnMClassic,
	ConnectVisionari,
	ConnectSwannViaMedia,
	ConnectSwannDVR8,
	ConnectMEYE,
};

void printBuffer(char *pbuf, size_t len)
{
	int n;

	printMessage(false, "Length: %lu\n", (unsigned long)len);
	for(n=0; n < len ; n++)
	{
		printf( "%02x",(unsigned char)(pbuf[n]));
		
		if( ((n + 1) % 8) == 0 )
			printf(" ");		// Make hex string slightly more readable
	}

	printf("\n");
}

int main(int argc, char**argv)
{
	char pipename[256];
	struct addrinfo hints, *server;
	struct sockaddr_in serverAddr;
	int retval = 0;
	char recvBuf[2048];
	struct sigaction sapipe, oldsapipe, saterm, oldsaterm, saint, oldsaint, sahup, oldsahup;
	char opt;
	int loopIdx;
	int outPipe = -1;
#ifdef DOMAIN_SOCKETS
	struct sockaddr_un addr;
#endif
	int sockFd = -1;
	struct timeval tv;
#ifdef NON_BLOCK_READ
	struct timeval tv, tv_sel;
#endif
	struct linger lngr;
	int status = 0;
	int pid = 0;

	lngr.l_onoff = false;
	lngr.l_linger = 0;

	// Process arguments
	// Clear and set defaults
	memset(&globalArgs, 0, sizeof(globalArgs));
	
	globalArgs.hostname =
		globalArgs.pipeName = "zmodo";
	globalArgs.model = media;
	globalArgs.username = 
		globalArgs.password = "admin";

	// Read command-line
	while( ((opt = getopt(argc, argv, optString)) != -1) && (opt != 255))
	{
		switch( opt )
		{
		case 'v':
			globalArgs.verbose = true;
			break;
		case 'c':
			globalArgs.channel[atoi(optarg) - 1] = true;
			break;
		case 'n':
			globalArgs.pipeName = optarg;
			break;
		case 's':
			globalArgs.hostname = optarg;
			break;
		case 'p':
			globalArgs.port = atoi(optarg);
			break;
		case 'm':
			globalArgs.model = atoi(optarg);
			break;
		case 'u':
			globalArgs.username = optarg;
			break;
		case 'a':
			globalArgs.password = optarg;
			break;
		case 't':
			globalArgs.timer = atoi(optarg);
			break;
		case 'h':
			// Fall through
		case '?':
			// Fall through
		default:
			display_usage(argv[0]);
			return 0;
		}
	}

	// Set up default values based on provided values (if any)
	if( !globalArgs.port )
	{   
	    //printMessage(true, "%s\n", globalArgs.model);
		switch( globalArgs.model )
		{
		case mobile:
			globalArgs.port = 18600;
			break;
		case media:
		case media_header:
		case cnmclassic:
		case swannmedia:
			globalArgs.port = 9000;
			break;
		case swanndvr8:
			globalArgs.port = 9000;
			break;
		case meye:
			globalArgs.port = 80;
			break;	
		case qt504:
			globalArgs.port = 6036;
			break;
		case dvr8104_mobile:
			globalArgs.port = 8888;
			break;
		case visionari:
			globalArgs.port = 1115;
			break;
		}
	}

	memset(&saint, 0, sizeof(saint));
	memset(&saterm, 0, sizeof(saterm));
	memset(&sahup, 0, sizeof(sahup));
	
	// Ignore SIGPIPE
	sapipe.sa_handler = sigHandler;
	sigaction(SIGPIPE, &sapipe, &oldsapipe);

	// Handle SIGTERM & SIGING
	saterm.sa_handler = sigHandler;
	sigaction(SIGTERM, &saterm, &oldsaterm);
	saint.sa_handler = sigHandler;
	sigaction(SIGINT, &saint, &oldsaint);
	
	signal( SIGUSR1, SIG_IGN );		// Ignore SIGUSR1 in parent process

	// SIGUSR2 is used to reset the pipe and connection
	sahup.sa_handler = sigHandler;
	sigaction(SIGUSR2, &sahup, &oldsahup);

	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_protocol = IPPROTO_TCP;
	retval = getaddrinfo(globalArgs.hostname, NULL, &hints, &server);
	if( retval != 0 )
	{
		printMessage(false, "getaddrinfo failed: %s\n", gai_strerror(retval));
		return 1;
	}

	serverAddr.sin_addr = ((struct sockaddr_in*)server->ai_addr)->sin_addr;
	serverAddr.sin_port = htons(globalArgs.port);


	do
	{
		if( pid )
		{
			printMessage(true, "Child %i returned: %i\n", pid, status);

			if( g_cleanUp == 2 )
				g_cleanUp = false;	// Ignore SIGHUP
		}

		// Create a fork for each camera channel to stream
		for( loopIdx=0;loopIdx<MAX_CHANNELS;loopIdx++ )
		{
			//static bool hitFirst = false;
			if( globalArgs.channel[loopIdx] == true )
			{
				// Always fork if we're starting up, or if the pid of the dead child process matches
				if( pid == 0 || g_childPids[loopIdx] == pid )
					g_childPids[loopIdx] = fork();

				// Child Process
				if( g_childPids[loopIdx] == 0 )
				{
					// SIGUSR1 is used to reset the pipe and connection
					sahup.sa_handler = sigHandler;
					sigaction(SIGUSR1, &sahup, &oldsahup);

					memset(g_childPids, 0, sizeof(g_childPids));
					g_processCh = loopIdx;
					break;
				}
				// Error
				else if( g_childPids[loopIdx] == -1 )
				{
					printMessage(false, "fork failed\n");
					return 1;
				}
			}
		}
	}
	while( (pid = wait(&status)) > 0  && g_cleanUp != true );

	if( g_processCh != -1 )
	{
		// At this point, g_processCh contains the camera number to use
		sprintf(pipename, "/tmp/%s%i", globalArgs.pipeName, g_processCh);

		tv.tv_sec = 5;		// Wait 5 seconds for socket data
		tv.tv_usec = 0;
		
#ifndef DOMAIN_SOCKETS
		retval = mkfifo(pipename, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);

		if( retval != 0 )
		{
			sprintf(g_errBuf, "Ch %i: Failed to create pipe", g_processCh+1);
			perror(g_errBuf);
		}
#endif	

		while( !g_cleanUp )
		{
			int flag = true;
#ifdef NON_BLOCK_READ
			fd_set readfds;
#endif
			// Initialize the socket and connect
			sockFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

			if( setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv)))
			{
				sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to set socket timeout");
				perror(g_errBuf);
			}

			if( setsockopt(sockFd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag)))
			{
				sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to set TCP_NODELAY");
				perror(g_errBuf);
			}
			if( setsockopt(sockFd, SOL_SOCKET, SO_LINGER, (char*)&lngr, sizeof(lngr)))
			{
				sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to set SO_LINGER");
				perror(g_errBuf);
			}

			retval = connect(sockFd, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
			
			if( globalArgs.verbose )
				printMessage(true, "Ch %i: Connect result: %i\n", g_processCh+1, retval);
			
			if( retval == -1 && errno != EINPROGRESS )
			{
				int sleeptime = 10;

				if( globalArgs.verbose )
				{
					sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to connect");
					perror(g_errBuf);
					printMessage(true, "Waiting %i seconds.\n", sleeptime);
				}
				close(sockFd);
				sockFd = -1;
				sleep(sleeptime);
				continue;
			}
		
			if( retval == -1 && errno != EINPROGRESS )
			{
				
				sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to connect");
				perror(g_errBuf);
				return 1;
			}
			
			retval = 0;

			if( pConnectFunc[globalArgs.model](sockFd, g_processCh) != 0 )
			{
				printMessage(true, "Login failed, bailing.\nDid you select the right model?\n");
				close(sockFd);
				sockFd = -1;
				return 1;
			}
#ifdef NON_BLOCK_READ
			if( fcntl(sockFd, F_SETFL, O_NONBLOCK) == -1 )	// non-blocking sockets
			{
				sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to set O_NONBLOCK");
				perror(g_errBuf);
			}
			FD_ZERO(&readfds);
			FD_SET(sockFd, &readfds	);
#endif
			// the stream sometimes goes grey,
			// this alarm should periodically reset the stream
			if(globalArgs.timer)
				alarm(globalArgs.timer);

			// Now we are connected and awaiting stream
			do
			{
				int read;
#ifdef NON_BLOCK_READ
				tv_sel.tvsec = 0;
				tv_sel.tv_usec = 10000;	// Wait 10 ms

				if( select( sockFd+1, &readfds, NULL, NULL, &tv_sel) == -1)
				{
					sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Select failed");
					perror(g_errBuf);
					
					close(sockFd);
					sockFd = -1;
					break;			// Connection may have died, reset
				}

				if (!FD_ISSET(sockFd, &readfds))
				{
					if( globalArgs.verbose )
						printMessage(true, "\nCh %i: %s", g_processCh+1, "Read would block\n");
					continue;		// Not ready yet
				}
#endif
				// Read actual h264 data from camera
				read  = recv(sockFd, recvBuf, sizeof(recvBuf), 0);
				
				// Server disconnected, close the socket so we can try to reconnect
				if( read <= 0 )
				{
#ifdef NON_BLOCK_READ
					if( errno == EAGAIN || errno == EWOULDBLOCK )
					{
						if( globalArgs.verbose )
							printMessage(true, "\nCh %i: %s", g_processCh+1, "Read would block\n");
						continue;
					}
#endif
					if( globalArgs.verbose )
						printMessage(true, "Ch %i: Socket closed. Receive result: %i\n", g_processCh+1, read);
					
					close(sockFd);
					sockFd = -1;
					break;
				}

				if( globalArgs.verbose )
				{
					printf(".");
					fflush(stdout);
				}

#ifdef DOMAIN_SOCKETS
				if( outPipe == -1 )
				{
					outPipe = socket(AF_UNIX, SOCK_STREAM, 0);
					if( outPipe == -1 )
						perror("Error creating socket");

					memset(&addr, 0, sizeof(addr));
					addr.sun_family = AF_UNIX;
					strncpy(addr.sun_path, pipename, sizeof(addr.sun_path) - 1);

					if( bind(outPipe, (struct sockaddr*)&addr, sizeof(addr)) == -1 )
						perror("Error binding socket");
				}
				
#else
				// Open the pipe if it wasn't previously opened
				if( outPipe == -1 )
					outPipe = open(pipename, O_WRONLY | O_NONBLOCK);
#endif

				// send to pipe
				if( outPipe != -1 )
				{

					if( (retval = write(outPipe, recvBuf, read)) == -1)
					{
						if( errno == EAGAIN || errno == EWOULDBLOCK )
						{
							if( globalArgs.verbose )
								printMessage(true, "\nCh %i: %s", g_processCh+1, "Reader isn't reading fast enough, discarding data. Not enough processing power?\n");

							// Right now we discard data, should be a way to buffer maybe?
							continue;
						}
						// reader closed the pipe, wait for it to be opened again.
						else if( globalArgs.verbose )
						{
							sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Pipe closed");
							perror(g_errBuf);
						}
#ifndef DOMAIN_SOCKETS
						close(outPipe);
						outPipe = -1;
#endif
						close(sockFd);
						sockFd = -1;
						continue;
					}
					else
					{
						if( globalArgs.verbose )
						{
							printf("\b \b");
							fflush(stdout);
						}
					}
				}
			}
			while( sockFd != -1 && !g_cleanUp );
			
			// If we receive a SIGUSR1, close and reset everything
			// Then start the loop over again.
			if( g_cleanUp >= 2 )
			{
				g_cleanUp = false;

				if( sockFd != -1 )
					close(sockFd);

				sockFd = -1;

				if( g_cleanUp != 3 )
				{
					if( outPipe != -1 )
						close(outPipe);
					outPipe = -1;
				}

			}

		}
		if( globalArgs.verbose )
			printMessage(true, "Exiting loop: %i\n", g_cleanUp);
		// Received signal to exit, cleanup
		close(outPipe);
		close(sockFd);

		unlink(pipename);
	}

	// Restore old signal handler
	sigaction(SIGPIPE, &oldsapipe, NULL);
	sigaction(SIGTERM, &oldsaterm, NULL);
	sigaction(SIGINT, &oldsaint, NULL);
	sigaction(SIGUSR1, &oldsahup, NULL);
	freeaddrinfo(server);

	// Kill all children (if any)
	for( loopIdx=0;loopIdx<MAX_CHANNELS;loopIdx++ )
	{
		if( globalArgs.channel[loopIdx] > 0 )
			kill( globalArgs.channel[loopIdx], SIGTERM );
	}
	return 0;
}

void display_usage(char *name)
{
	printf("Usage: %s [options]\n\n", name);
	printf("Where [options] is one of:\n\n"
		"    -s <string>\tIP to connect to\n"
		"    -t <int>\tSend a timer interrupt every x seconds.\n"
		"    -p <int>\tPort number to connect to\n"
		"    -c <int>\tChannels to stream (can be specified multiple times)\n"
		"    -n <string>\tBase filename of pipe (ch# will be appended)\n"
		"    -v\t\tVerbose output\n"
		"    -u <string>\tUsername\n"
		"    -a <string>\tPassword\n"
		"    -m <int>\tMode to use (ie. mobile/media)\n"
		"    \t\t1 - Use mobile port (safest, default)\n"
		"    \t\t2 - Use media port (Works for some models, ie. Zmodo 9104)\n"
		"    \t\t3 - Use media port w/header (Other models, please test)\n"
		"    \t\t4 - Use QT5 family (ie. QT504, QT528)\n"
		"    \t\t5 - Zmodo DVR-8104UV compatible (also DVR-8114HV)\n"
		"    \t\t6 - CnM Classic 4 Cam DVR\n"
		"    \t\t7 - Visionari 4/8 Channel DVR\n"
		"    \t\t8 - Swann DM-70D and compatible\n"
		"    \t\t9 - Swann DVR8-4000 and compatible\n"
		"    \t\t10 - mEye compatible\n"
	"\n");
}

void sigHandler(int sig)
{
	printMessage(true, "Received signal: %i\n", sig);
	
	switch( sig )
	{
	case SIGTERM:
	case SIGINT:
		// Kill the main loop
		g_cleanUp = true;
		break;
	case SIGUSR1:
	case SIGALRM:
	case SIGPIPE:
		g_cleanUp = 2;
		break;
	case SIGUSR2:
		g_cleanUp = 3;
		break;
	}
}

int printMessage(bool verbose, const char *message, ...)
{
	char msgBuf[2048];
	int ret=0;
	va_list argptr;
	va_start(argptr, message);

	if( g_processCh == -1 )
		sprintf(msgBuf, "Main: %s", message);
	else
		sprintf(msgBuf, "Ch %i: %s", g_processCh, message);

	if( !( verbose && !globalArgs.verbose) )
		ret = vprintf(msgBuf, argptr);

	va_end(argptr);
	return ret;
}
// This is more compatible, but less reliable than the Media mode.
// h264 decoder shows "This stream was generated by a broken encoder, invalid 8x8 inference"
// Output is 320x240@25fps ~160kbit/s VBR
int ConnectViaMobile(int sockFd, int channel)
{
	struct QSeeLoginMobile loginBuf = {0};
	int retval;
	int header;
	char recvBuf[128];

	// do writing
	// Setup login buffer
	loginBuf.val1 = htonl(64);
	loginBuf.val3 = htons(10496);
	loginBuf.val4 = htons(14336);
	loginBuf.ch   = htons(channel);
	strcpy(loginBuf.user, globalArgs.username);
	strcpy(loginBuf.pass, globalArgs.password);

	retval = send(sockFd, (char*)(&loginBuf), sizeof(loginBuf), 0);

	if( globalArgs.verbose )
	{
		printMessage(true, "Ch %i: Send result: %i\n", channel+1, retval);
	}

	// do reading
	// Get header length (4 bytes)
	retval = recv(sockFd, &header, sizeof(header), 0);
	if( retval != 4 )
	{
		printMessage(true, "Ch %i: Receive 1 failed.\n", channel+1);
		return 1;
	}

	header = ntohl(header);
	
	// Get next section (20 bytes)
	retval = recv(sockFd, recvBuf, header, 0);
	if( retval != header )
	{
		printMessage(true, "Ch %i: Receive 2 failed.\n", channel+1);
		return 1;
	}

	// Check login status
	if( recvBuf[16] != 1 )
	{
		printMessage(true, "Ch %i: Login failed: ", channel+1);
		int x;
		for( x=0;x<header;x++)
			printMessage(true, "%02x", recvBuf[x]);
		return 1;
	}

	// Read next sections
	retval = recv(sockFd, recvBuf, sizeof(header), 0);
	if( retval != sizeof(header) && recvBuf[3] != 0 )
	{
		printMessage(true, "Ch %i: Problem length (4): %i, recvBuf[3]: %i\n", channel+1, retval, (int)recvBuf[3]);
		return 1;
	}

	header = recvBuf[3] & 0xFF;
	
	retval = recv(sockFd, recvBuf, header, 0);
	if( retval != header )
	{
		printMessage(true, "Ch %i: Receive 3 failed.\n", channel+1);
		return 1;
	}

	// Read another 27 bytes
	header = 27;
	
	retval = recv(sockFd, recvBuf, header, 0);
	if( retval != header )
	{
		printMessage(true, "Ch %i: Receive 4 failed.\n", channel+1);
		return 1;
	}

	// If we got here, the stream will be waiting for us to recv.
	return 0;
}

// This is less compatible, but more reliable than the mobile mode
// Output is 704x480@25fps 1200kbit/s VBR
int ConnectViaMedia(int sockFd, int channel)
{
	struct QSeeLoginMedia loginBuf;
	int retval;
	static bool beenHere = false;

	memset(&loginBuf, 0, sizeof(loginBuf));

	// Some models take a special header first
	// Mine doesn't, so this is untested
	if( globalArgs.model == media_header )
	{
		retval = send(sockFd, "0123456", 7, 0);

		if( globalArgs.verbose )
			printMessage(true, "Ch %i: Send result: %i\n", channel+1, retval);
	}

	// Setup login buffer
	loginBuf.valc[10] = 0x01;
	*(short*)&loginBuf.valc[14] = htons(0x035f + (1 << channel));
	loginBuf.valc[30] = 0x01;
	loginBuf.valc[26] = 0x68;
	loginBuf.valc[34] = 0x10;
	*(short*)&loginBuf.valc[37] = htons(1 << channel);
	loginBuf.valc[42] = 1;
	loginBuf.valc[46] = 1;

	strcpy(loginBuf.user, globalArgs.username);
	strcpy(loginBuf.pass, globalArgs.password);

	if( globalArgs.verbose && beenHere == false )
	{
		printBuffer((char*)&loginBuf, sizeof(loginBuf));
		beenHere = true;
	}

	retval = send(sockFd, (char*)(&loginBuf), sizeof(loginBuf), 0);

	if( globalArgs.verbose )
		printMessage(true, "Ch %i: Send result: %i\n", channel+1, retval);

/*	// Not sure when this is used, possibly some other model
	// do reading
	// Get header length (8 bytes)
	retval = recv(sockFd, &recvBuf, 8, 0);
	if( retval != 8 )
	{
		if( retval != 16 )
		{
			printMessage(true, "Receive 1 failed.\n");
			return 1;
		}
	}
*/
	// If we got here, the stream will be waiting for us to recv.
	return 0;
}

// QT5 Family (ie. QT-504)
// the QT-504 is a bit different, it sends 3 packets for login.
int ConnectQT504(int sockFd, int channel)
{
	char suppLoginBuf[88] = {0};
	struct QSee504Login loginBuf;
	int retval;
	char recvBuf[532];
	static bool beenHere = false;

	memset(&loginBuf, 0, sizeof(loginBuf));

	loginBuf.vala[0] = 0x31;
	loginBuf.vala[1] = 0x31;
	loginBuf.vala[2] = 0x31;
	loginBuf.vala[3] = 0x31;
	loginBuf.vala[4] = 0x88;
	loginBuf.vala[8] = 0x01;
	loginBuf.vala[9] = 0x01;
	loginBuf.vala[12] = 0xff;
	loginBuf.vala[13] = 0xff;
	loginBuf.vala[14] = 0xff;
	loginBuf.vala[15] = 0xff;
	loginBuf.vala[16] = 0x04;
	loginBuf.vala[20] = 0x78;
	loginBuf.vala[24] = 0x03;

	strcpy(loginBuf.user, globalArgs.username);
	strcpy(loginBuf.pass, globalArgs.password);
	gethostname(loginBuf.host, sizeof(loginBuf.host));
	
	loginBuf.filler[22] = 0x50;
	loginBuf.filler[23] = 0x56;
	loginBuf.filler[24] = 0xc0;
	loginBuf.filler[25] = 0x08;
	loginBuf.filler[28] = 0x04;
	
	if( globalArgs.verbose && beenHere == false )
	{
		printBuffer((char*)&loginBuf, sizeof(loginBuf));
	}

	// Send the login packet (1 of 4)
	retval = send(sockFd, (char*)(&loginBuf), sizeof(loginBuf), 0);

	if( globalArgs.verbose )
	{
		printMessage(true, "Ch %i: Send 1 result: %i\n", channel+1, retval);
	}

	// do reading
	// Get header length (4 bytes)
	retval = recv(sockFd, &recvBuf, 532, 0);

	// Verify send was successful
	if( retval != 532 )
	{
		printMessage(true, "Ch %i: Receive 1 failed: %i\n", channel+1, retval);
	//	return 1;
	}
	
	/* Inbetween the 2 and last packets there is another packet.
	 * It seems to be optional.
	 */
	memset(suppLoginBuf, 0, 88);

	suppLoginBuf[0] = 0x31;
	suppLoginBuf[1] = 0x31;
	suppLoginBuf[2] = 0x31;
	suppLoginBuf[3] = 0x31;
	suppLoginBuf[4] = 0x50;
	suppLoginBuf[8] = 0x03;
	suppLoginBuf[9] = 0x04;
	suppLoginBuf[12] = 0xf0;
	suppLoginBuf[13] = 0xb7;
	suppLoginBuf[14] = 0x3d;
	suppLoginBuf[15] = 0x08;
	suppLoginBuf[16] = 0x03;
	suppLoginBuf[20] = 0x40;
	suppLoginBuf[25] = 0xf8;
	suppLoginBuf[32] = 0x01;
	suppLoginBuf[33] = 0xf8;
	suppLoginBuf[40] = 0x02;
	suppLoginBuf[41] = 0xf8;
	suppLoginBuf[48] = 0x03;
	suppLoginBuf[49] = 0xf8;
	suppLoginBuf[56] = 0x40;
	suppLoginBuf[57] = 0xf8;
	suppLoginBuf[60] = 0x97;
	suppLoginBuf[61] = 0xf0;
	suppLoginBuf[64] = 0x41;
	suppLoginBuf[65] = 0xf8;
	

	// Send the next packet (2 of 4)
	retval = send(sockFd, suppLoginBuf, 88, 0);

	if( globalArgs.verbose )
	{
		printMessage(true, "Ch %i: Send 2 result: %i\n", channel+1, retval);
	}

	retval = 0;
	{
	int ret = 0;
	while( (ret = recv(sockFd, recvBuf, sizeof(recvBuf), 0)) > 0 )
		retval += ret;
	}
	
	suppLoginBuf[0] = 0x31;
	suppLoginBuf[1] = 0x31;
	suppLoginBuf[2] = 0x31;
	suppLoginBuf[3] = 0x31;
	suppLoginBuf[4] = 0x34;
	suppLoginBuf[8] = 0x01;
	suppLoginBuf[9] = 0x02;
	
	suppLoginBuf[20] = 0x24;
	*(short*)&suppLoginBuf[36] = htons(1 << channel);
	*(short*)&suppLoginBuf[52] = htons(1 << channel);

	if( globalArgs.verbose && beenHere == false )
	{
		printBuffer((char*)&suppLoginBuf, 60);
		beenHere = true;
	}

	// Send the next packet (3 of 4)
	retval = send(sockFd, suppLoginBuf, 60, 0);

	if( globalArgs.verbose )
	{
		printMessage(true, "Ch %i: Send 3 result: %i\n", channel+1, retval);
	}

	// do reading
	retval = recv(sockFd, recvBuf, 124, 0);

	// Verify send was successful
	if( retval <= 0 )
	{
		printMessage(true, "Ch %i: Receive 3 failed.\n", channel+1);
		return 1;
	}
	else
		printMessage(true, "Ch %i: Receive 3 result: %i bytes.\n", channel+1, retval);
	

	// Reuse the old buffer, last three bytes should still be 0
	//suppLoginBuf[5] = 0;
	
	// Send the last packet (4 of 4)
	//retval = send(sockFd, suppLoginBuf, 8, 0);

	//if( globalArgs.verbose )
	//{
	//	printMessage(true, "Ch %i: Send 4 result: %i\n", channel+1, retval);
	//}

	// If we got here, the stream will be waiting for us to recv.
	return 0;
}

// Output is 352x240@25fps VBR
int ConnectDVR8104ViaMobile(int sockFd, int channel)
{
	struct DVR8104MobileLogin loginBuf;
	int retval;
	static bool beenHere = false;

	memset(&loginBuf, 0, sizeof(loginBuf));

	// Setup login buffer
	loginBuf.vala[3] = 0x70;
	loginBuf.vala[4] = 0x01;
	loginBuf.vala[8] = 0x28;
	loginBuf.vala[10] = 0x04;
	loginBuf.vala[12] = 0x03;
	loginBuf.vala[14] = 0x07;
	loginBuf.vala[16] = 0x48;
	loginBuf.vala[18] = 0x24;
	loginBuf.vala[20] = 0x20;
	loginBuf.vala[21] = 0x20;
	loginBuf.vala[22] = 0x20;
	loginBuf.vala[23] = 0x21;
	loginBuf.vala[24] = 0x20;
	loginBuf.vala[25] = 0x20;
	loginBuf.vala[26] = 0x20;
	loginBuf.vala[36] = 0x4d;
	loginBuf.vala[37] = 0x4f;
	loginBuf.vala[38] = 0x42;
	loginBuf.vala[39] = 0x49;
	loginBuf.vala[40] = 0x4c;
	loginBuf.vala[41] = 0x45;
	loginBuf.vala[56] = 0x29;
	loginBuf.vala[58] = 0x38;
	loginBuf.valb[0] = 0x6e;
	loginBuf.valb[27] = 0x6e;
	loginBuf.filler[10] = 0x01;
	loginBuf.filler[15] = channel;

	strcpy(loginBuf.user, globalArgs.username);
	strcpy(loginBuf.pass, globalArgs.password);

	if( globalArgs.verbose && beenHere == false )
	{
		printBuffer((char*)&loginBuf, sizeof(loginBuf));
		beenHere = true;
	}

	retval = send(sockFd, (char*)(&loginBuf), sizeof(loginBuf), 0);

	if( retval != sizeof(loginBuf) )
	{
		printMessage(true, "Ch %i: Send failed, was: %i, should be: %i\n", channel+1, retval, (int)sizeof(loginBuf));	
		return 1;
	}
	if( globalArgs.verbose )
		printMessage(true, "Ch %i: Send result: %i\n", channel+1, retval);

	// If we got here, the stream will be waiting for us to recv.
	return 0;
}

// CnM Classic 4 Cam
// http://194.150.201.35/cnmsecure/support/4CamClassicKit.htm
int ConnectCnMClassic(int sockFd, int channel)
{
	struct CnMClassicLogin loginBuf;
	int retval;
	char recvBuf[532];
	static bool beenHere = false;

	memset(&loginBuf, 0, sizeof(loginBuf));

	loginBuf.vala[3] = 0x01;
	loginBuf.vala[7] = 0x03;
	loginBuf.vala[8] = 0x0b;
	loginBuf.vala[19] = 0x68;
	loginBuf.vala[23] = 0x01;
	loginBuf.vala[27] = 0x54;
	*(short*)&loginBuf.vala[30] = htons(1 << channel);

	strcpy(loginBuf.user, globalArgs.username);
	strcpy(loginBuf.pass, globalArgs.password);
	
	if( globalArgs.verbose && beenHere == false )
	{
		printBuffer((char*)&loginBuf, sizeof(loginBuf));
	}

	// Send the login packet
	retval = send(sockFd, (char*)(&loginBuf), sizeof(loginBuf), 0);

	if( globalArgs.verbose )
	{
		printMessage(true, "Ch %i: Send 1 result: %i\n", channel+1, retval);
	}

	// do reading (1 of 2)
	retval = recv(sockFd, &recvBuf, 8, 0);

	// Verify send was successful
	if( retval != 8 && recvBuf[0] != 1 )
	{
		printMessage(true, "Ch %i: Receive 1 failed: %i\n", channel+1, retval);
		printBuffer((char*)&recvBuf, sizeof(recvBuf));
		return 1;
	}
	
	// do reading (1 of 2)
	retval = recv(sockFd, &recvBuf, 520, 0);

	// Verify send was successful
	if( retval != 520 )
	{
		printMessage(true, "Ch %i: Receive 2 failed: %i\n", channel+1, retval);
		printBuffer((char*)&recvBuf, sizeof(recvBuf));
		return 1;
	}

	// If we got here, the stream will be waiting for us to recv.
	return 0;
}

// Visionari 4/8 Channel DVR
int ConnectVisionari(int sockFd, int channel)
{
        struct VisionariLogin loginBuf;
        int retval;
        static bool beenHere = false;

        memset(&loginBuf, 0, sizeof(loginBuf));

	// Setup login buffer
        loginBuf.vala[3] = 0x70;
        loginBuf.vala[4] = 0x01;
        loginBuf.vala[8] = 0x28;
        loginBuf.vala[10] = 0x04;
        loginBuf.vala[12] = 0x03;
        loginBuf.vala[14] = 0x07;
        loginBuf.vala[16] = 0x48;
        loginBuf.vala[18] = 0x24;
        loginBuf.vala[20] = 0x30;
        loginBuf.vala[21] = 0x30;
        loginBuf.vala[22] = 0x30;
        loginBuf.vala[23] = 0x31;
        loginBuf.vala[24] = 0x30;
        loginBuf.vala[25] = 0x30;
        loginBuf.vala[26] = 0x30;
        loginBuf.vala[36] = 0x4d;
        loginBuf.vala[37] = 0x4f;
        loginBuf.vala[38] = 0x42;
        loginBuf.vala[39] = 0x49;
        loginBuf.vala[40] = 0x4c;
        loginBuf.vala[41] = 0x45;
        loginBuf.vala[56] = 0x29;
        loginBuf.vala[58] = 0x38;
        loginBuf.filler[10] = 0x01;
        loginBuf.filler[15] = channel;

	strcpy(loginBuf.user, globalArgs.username);
        strcpy(loginBuf.pass, globalArgs.password);
	

        if( globalArgs.verbose && beenHere == false )
        {
                printBuffer((char*)&loginBuf, sizeof(loginBuf));
                beenHere = true;
        }

        retval = send(sockFd, (char*)(&loginBuf), sizeof(loginBuf), 0);

        if( retval != sizeof(loginBuf) )
        {
                printf("Ch %i: Send failed, was: %i, should be: %i\n", channel+1, retval, (int)sizeof(loginBuf));
                return 1;
        }
        if( globalArgs.verbose )
                printf("Ch %i: Send result: %i\n", channel+1, retval);

        // If we got here, the stream will be waiting for us to recv.
        return 0;
}

// For some Swann models (Hardware version DM-70D, Device type DVR04B)
int ConnectSwannViaMedia(int sockFd, int channel)
{
	struct SwannLoginMedia loginBuf;
	int retval;
	static bool beenHere = false;
	char recvBuf[16];
	short *shrtval;
	memset(&loginBuf, 0, sizeof(loginBuf));

	// Setup login buffer
	loginBuf.valc[10] = 0x01;
	shrtval = (short*)&loginBuf.valc[14];
	if( channel == 1 )
		*shrtval = htons(0x0324);
	else
		*shrtval = htons(0x0324 + channel);

	loginBuf.valc[26] = 0x68;
	loginBuf.valc[30] = 0x01;
	loginBuf.valc[34] = 0x10;
	*(short*)&loginBuf.valc[37] = htons(1 << channel);
	loginBuf.valc[42] = 1;
	loginBuf.valc[46] = 1;

	strcpy(loginBuf.user, globalArgs.username);
	strcpy(loginBuf.pass, globalArgs.password);

	if( globalArgs.verbose && beenHere == false )
	{
		printBuffer((char*)&loginBuf, sizeof(loginBuf));
		beenHere = true;
	}

	retval = send(sockFd, (char*)(&loginBuf), sizeof(loginBuf), 0);
	printMessage(true, "Ch %i: Send result: %i\n", channel+1, retval);

	// before video stream, a small packet is sent
	// Get header length (8 bytes)
	retval = recv(sockFd, &recvBuf, 8, 0);
	if( retval != 8 )
	{
		if( retval != 16 )
		{
			printMessage(false, "Receive 1 failed.\n");
			return 1;
		}
	}
	// If we got here, the stream will be waiting for us to recv.
	return 0;
}

int ConnectSwannDVR8(int sockFd, int channel)
{
	char channelBuf[32] = {0};
	struct SwannDVR8 loginBuf;
	int retval;
	static bool beenHere = false;
	char recvBuf[9686];
	memset(&loginBuf, 0, sizeof(loginBuf));

	// Setup login buffer
	loginBuf.valc[0] = 0xf0;
	loginBuf.valc[1] = 0xde;
	loginBuf.valc[2] = 0xbc;
	loginBuf.valc[3] = 0x0a;
	loginBuf.valc[4] = 0x01;
	loginBuf.valc[8] = 0x44;
	loginBuf.valc[12] = 0xff;
	loginBuf.valc[13] = 0xff;
	loginBuf.valc[14] = 0xff;
	loginBuf.valc[15] = 0xff;	

	strcpy(loginBuf.user, globalArgs.username);
	strcpy(loginBuf.pass, globalArgs.password);
	
	memset(channelBuf, 0, 32);

	// select channel to stream
	channelBuf[0] = 0xf0;
	channelBuf[1] = 0xde;
	channelBuf[2] = 0xbc;
	channelBuf[3] = 0x0a;
	channelBuf[4] = 0x03;       //0x03 request video stream
	                            //0x04 logoff
	channelBuf[8] = 0x0c;
	*(short*)&channelBuf[11] = htons(channel);     //channel number
	*(short*)&channelBuf[19] = htons(channel);     //channel number
	*(short*)&channelBuf[23] = htons(channel);     //channel number
	
	channelBuf[28] = 0x01;     // Streaming Quality
	                           //seems to be 0x01 for Video: h264 (High), yuv420p, 352x240, 8.83 fps, 4 tbr, 1200k tbn, 8 tbc
	                           //and 0x00 for Video: h264 (High), yuv420p, 704x480, 30 fps, 30 tbr, 1200k tbn, 60 tbc
	//total length 32 bytes

	if( globalArgs.verbose && beenHere == false )
	{
		printBuffer((char*)&loginBuf, sizeof(loginBuf));
		printBuffer((char*)&channelBuf, sizeof(channelBuf));
		beenHere = true;
	}

	retval = send(sockFd, (char*)(&loginBuf), sizeof(loginBuf), 0);
	printMessage(true, "Ch %i: Send Login result: %i\n", channel+1, retval);
	
	retval = recv(sockFd, recvBuf, sizeof(recvBuf), 0);
	
	//printBuffer((char*)&recvBuf, 32);
	printMessage(true, "Received Login Result: %i\n", retval);
	//printMessage(true, "X: %02x\n", (unsigned char)(recvBuf[8])); Value should be 0x50
	
	
	// Send packet to open channel
	retval = send(sockFd, channelBuf, sizeof(channelBuf), 0);
	
	// Verify send was successful
	if( retval != 32 )
	{
		printMessage(true, "Ch %i: Could not open channel, Streaming failed.\n", channel+1);
		return 1;
	}
	else
		printMessage(true, "Ch %i: Send Channel result: %i bytes.\n", channel+1, retval);
	
	// If we got here, the stream will be waiting for us to recv.
	return 0;
}


int ConnectMEYE(int sockFd, int channel)
{
	
//00000000  00 00 00 48 00 00 00 00  28 00 04 00 05 00 00 00 ...H.... (.......
//00000010  29 00 38 00 61 64 6d 69  6e 00 00 00 00 00 00 00 ).8.admi n.......
//00000020  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00 ........ ........
//00000030  00 00 00 00 6e 69 76 6c  32 30 30 35 00 00 00 00 ....nivl 2005....
//00000040  00 00 00 00 00 00 00 00  00 05 00 00             ........ ....
//76 bytes total

	struct mEye loginBuf;
	int retval;
	static bool beenHere = false;
	memset(&loginBuf, 0, sizeof(loginBuf));
	char initBuf[43] = {0};
	char configBuf[18] = {0};
	char channelBuf[26] = {0};
	char recvBuf[1024];

    // Setup the init buffer
    strcpy(initBuf, "GET /bubble/live?ch=0&stream=0 HTTP/1.1");
    initBuf[39] = 0x0d;
	initBuf[40] = 0x0a;
	initBuf[41] = 0x0d;
	initBuf[42] = 0x0a;
	    //total length 43 bytes

	// Setup login buffer
	loginBuf.valc[0] = 0xaa;
	loginBuf.valc[4] = 0x35;
	loginBuf.valc[13] = 0x2c;	

	strcpy(loginBuf.user, globalArgs.username);
	strcpy(loginBuf.pass, globalArgs.password);
	//*(short*)&loginBuf.channel[0] = htons(channel);     //channel number
	
	// Now setup configBuf
	memset(configBuf, 0, 18);

	// setup streaming
	configBuf[0] = 0xaa;
	configBuf[4] = 0x0d;
	configBuf[13] = 0x04;
	configBuf[14] = 0x01;
	    //total length 18 bytes
	
	
	// Now setup channelBuf
	memset(channelBuf, 0, 26);

	// select channel to stream
	channelBuf[0] = 0xaa;
	channelBuf[4] = 0x15;
	channelBuf[5] = 0x0a;
	*(short*)&channelBuf[9] = htons(channel);     //channel number	
	channelBuf[14] = 0x01;                         // Quality? 0=High, 1=Low
	channelBuf[18] = 0x01;
		//total length 26 bytes
	

	if( globalArgs.verbose && beenHere == false )
	{
		printBuffer((char*)&initBuf, sizeof(initBuf));
		printBuffer((char*)&loginBuf, sizeof(loginBuf));
		printBuffer((char*)&configBuf, sizeof(configBuf));
		printBuffer((char*)&channelBuf, sizeof(channelBuf));
		beenHere = true;
	}
	
	// send init packet
	retval = send(sockFd, (char*)(&initBuf), sizeof(initBuf), 0);
	printMessage(true, "Ch %i: Send Init result: %i\n", channel+1, retval);
	
	retval = recv(sockFd, recvBuf, sizeof(recvBuf), 0);  // expect 1024 byte response after login request
	
	//printBuffer((char*)&recvBuf, 32);
	printMessage(true, "Received Init Result(expect 1024): %i\n", retval);
	
	// send login packet
	retval = send(sockFd, (char*)(&loginBuf), sizeof(loginBuf), 0);
	printMessage(true, "Ch %i: Send Login result: %i\n", channel+1, retval);
	
	retval = recv(sockFd, recvBuf, 54, 0);  // expect 54 byte response after login request
	
	//printBuffer((char*)&recvBuf, 32);
	printMessage(true, "Received Login Result(expect 54): %i\n", retval);
	
	// send configure packet
	retval = send(sockFd, (char*)(&configBuf), sizeof(configBuf), 0);
	printMessage(true, "Ch %i: Send config result: %i\n", channel+1, retval);
	
	retval = recv(sockFd, recvBuf, 22, 0);  // expect 22 byte response after config request
	
	//printBuffer((char*)&recvBuf, 32);
	printMessage(true, "Received Login Result(expect 22): %i\n", retval);
	
	// Send packet to open channel
	retval = send(sockFd, channelBuf, sizeof(channelBuf), 0);
	
	
	// Verify send was successful, all 26 bytes
	if( retval != 26 )
	{
		printMessage(true, "Ch %i: Could not open channel, Streaming failed.\n", channel+1);
		return 1;
	}
	else
		printMessage(true, "Ch %i: Send Channel result: %i bytes.\n", channel+1, retval);

	// If we got here, the stream will be waiting for us to recv.
	return 0;
}