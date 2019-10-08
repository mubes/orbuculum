/*
 * SWO Splitter for Blackmagic Probe and TTL Serial Interfaces
 * ===========================================================
 *
 * Copyright (C) 2017, 2019  Dave Marples  <dave@marples.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the names Orbtrace, Orbuculum nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <semaphore.h>
#if defined OSX
    #include <sys/ioctl.h>
    #include <libusb.h>
    #include <termios.h>
#else
    #if defined LINUX
        #include <libusb-1.0/libusb.h>
        #include <asm/ioctls.h>
        #if defined TCGETS2
            #include <asm/termios.h>
            /* Manual declaration to avoid conflict. */
            extern int ioctl ( int __fd, unsigned long int __request, ... ) __THROW;
        #else
            #include <sys/ioctl.h>
            #include <termios.h>
        #endif
    #else
    #include <sys/ioctl.h>
    #include <libusb.h>
    #include <termios.h>
//        #error "Unknown OS"
    #endif
#endif
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include "git_version_info.h"
#include "generics.h"
#include "tpiuDecoder.h"
#include "itmDecoder.h"
#include "fileWriter.h"

#ifdef INCLUDE_FPGA_SUPPORT
    #include <libftdi1/ftdi.h>
    #include "ftdispi.h"
    #define FTDI_VID  (0x0403)
    #define FTDI_PID  (0x6010)
    #define FTDI_INTERFACE (INTERFACE_A)
    #define FTDI_UART_INTERFACE (INTERFACE_B)
    #define FTDI_INTERFACE_SPEED CLOCK_MAX_SPEEDX5
    #define FTDI_PACKET_SIZE  (17)
    #define FTDI_NUM_FRAMES   (900)  // If you make this too large the driver drops frames
    #define FTDI_HS_TRANSFER_SIZE (FTDI_PACKET_SIZE*FTDI_NUM_FRAMES)
    #define FPGA_AWAKE (0x80)
    #define FPGA_ASLEEP (0x90)
    // #define DUMP_FTDI_BYTES // Uncomment to get data dump of bytes from FTDI transfer
#endif

#define SERVER_PORT 3443                      /* Server port definition */
#define SEGGER_HOST "localhost"               /* Address to connect to SEGGER */
#define SEGGER_PORT (2332)

/* Descriptor information for BMP */
#define VID           (0x1d50)
#define PID           (0x6018)
#define SWO_INTERFACE (5)
#define ENDPOINT      (0x85)

#define TRANSFER_SIZE (4096)
#define NUM_CHANNELS  32
#define HW_CHANNEL    (NUM_CHANNELS)         /* Make the hardware fifo on the end of the software ones */

#define MAX_STRING_LENGTH (100)              /* Maximum length that will be output from a fifo for a single event */

/* Information for an individual channel */
struct Channel
{
    char *chanName;                          /* Filename to be used for the fifo */
    char *presFormat;                        /* Format of data presentation to be used */
};

#define HWFIFO_NAME "hwevent"

/* Record for options, either defaults or from command line */
struct
{
    /* Config information */
    bool useTPIU;
    bool segger;
    bool forceITMSync;
    char *fwbasedir;

#ifdef INCLUDE_FPGA_SUPPORT
    bool orbtrace;
    uint32_t orbtraceWidth;
#endif
    char *seggerHost;
    int32_t seggerPort;

    uint32_t tpiuITMChannel;

    /* Sink information */
    struct Channel channel[NUM_CHANNELS + 1];
    char *chanPath;

    /* Source information */
    char *port;
    char *file;
    int speed;
    int listenPort;
} options =
{
    .forceITMSync = true,
    .chanPath = "",
    .speed = 115200,
    .useTPIU = false,
    .tpiuITMChannel = 1,
    .listenPort = SERVER_PORT,
    .seggerHost = SEGGER_HOST
#ifdef INCLUDE_FPGA_SUPPORT
    ,
    .orbtraceWidth = 4
#endif
};


/* Runtime state */
struct channelRuntime
{
    int handle;
    pthread_t thread;
    char *fifoName;
};

/* List of any connected network clients */
struct nwClient

{
    int handle;
    pthread_t thread;
    struct nwClient *nextClient;
    struct nwClient *prevClient;
};

/* Informtation about each individual network client */
struct nwClientParams

{
    struct nwClient *client;
    int portNo;
    int listenHandle;
};

struct
{
    struct channelRuntime c[NUM_CHANNELS + 1];   /* Output for each channel */
    struct nwClient *firstClient;                /* Head of linked list of network clients */
    sem_t clientList;                              /* Locking semaphore for list of network clients */

    pthread_t ipThread;                          /* The listening thread for n/w clients */

    /* Timestamp info */
    uint64_t lastHWExceptionTS;

    /* The decoders and the packets from them */
    struct ITMDecoder i;
    struct ITMPacket h;
    struct TPIUDecoder t;
    struct TPIUPacket p;

#ifdef INCLUDE_FPGA_SUPPORT
    bool feederExit;
    struct ftdi_context *ftdi;
    struct ftdispi_context ftdifsc;
#endif
} _r;

/* Structure for parameters passed to a software task thread */
struct _runThreadParams

{
    int portNo;
    int listenHandle;
};
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Mechanism for handling the set of Fifos
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
static void *_runFifo( void *arg )

/* This is the control loop for the channel fifos (for each software port) */

{
    struct _runThreadParams *params = ( struct _runThreadParams * )arg;
    struct ITMPacket p;
    uint32_t w;

    char constructString[MAX_STRING_LENGTH];
    int fifo;
    int readDataLen, writeDataLen;

    if ( mkfifo( _r.c[params->portNo].fifoName, 0666 ) < 0 )
    {
        return NULL;
    }

    while ( 1 )
    {
        /* This is the child */
        fifo = open( _r.c[params->portNo].fifoName, O_WRONLY );

        while ( 1 )
        {
            /* ....get the packet */
            readDataLen = read( params->listenHandle, &p, sizeof( struct ITMPacket ) );

            if ( readDataLen != sizeof( struct ITMPacket ) )
            {
                return NULL;
            }

            /* Build 32 value the long way around to avoid type-punning issues */
            w = ( p.d[3] << 24 ) | ( p.d[2] << 16 ) | ( p.d[1] << 8 ) | ( p.d[0] );
            if (options.channel[params->portNo].presFormat)
            {
                // formatted output.
                writeDataLen = snprintf( constructString, MAX_STRING_LENGTH, options.channel[params->portNo].presFormat, w );
                if ( write( fifo, constructString, ( writeDataLen < MAX_STRING_LENGTH ) ? writeDataLen : MAX_STRING_LENGTH ) <= 0 )
                {
                    break;
                }
            }
            else
            {
                // raw output.
                if ( write( fifo, &w, sizeof (w) ) <= 0 )
                {
                    break;
                }
            }
        }

        close( fifo );
    }

    return NULL;
}
// ====================================================================================================
static void *_runHWFifo( void *arg )

/* This is the control loop for the hardware fifo */

{
    struct _runThreadParams *params = ( struct _runThreadParams * )arg;
    int fifo;
    int readDataLen;
    uint8_t p[MAX_STRING_LENGTH];

    if ( mkfifo( _r.c[params->portNo].fifoName, 0666 ) < 0 )
    {
        return NULL;
    }

    while ( 1 )
    {
        /* This is the child */
        fifo = open( _r.c[params->portNo].fifoName, O_WRONLY );

        while ( 1 )
        {
            /* ....get the packet */
            readDataLen = read( params->listenHandle, p, MAX_STRING_LENGTH );

            if ( write( fifo, p, readDataLen ) <= 0 )
            {
                break;
            }
        }

        close( fifo );
    }

    return NULL;
}
// ====================================================================================================
static bool _makeFifoTasks( void )

/* Create each sub-process that will handle a port */

{
    struct _runThreadParams *params;
    int f[2];

    /* Cycle through channels and create a fifo for each one that is enabled */
    for ( int t = 0; t < ( NUM_CHANNELS + 1 ); t++ )
    {
        if ( t < NUM_CHANNELS )
        {
            if ( options.channel[t].chanName )
            {
                /* This is a live software channel fifo */
                if ( pipe( f ) < 0 )
                {
                    return false;
                }

                fcntl( f[1], F_SETFL, O_NONBLOCK );
                _r.c[t].handle = f[1];

                params = ( struct _runThreadParams * )malloc( sizeof( struct _runThreadParams ) );
                params->listenHandle = f[0];
                params->portNo = t;

                _r.c[t].fifoName = ( char * )malloc( strlen( options.channel[t].chanName ) + strlen( options.chanPath ) + 2 );
                strcpy( _r.c[t].fifoName, options.chanPath );
                strcat( _r.c[t].fifoName, options.channel[t].chanName );

                if ( pthread_create( &( _r.c[t].thread ), NULL, &_runFifo, params ) )
                {
                    return false;
                }
            }
        }
        else
        {
            /* This is the hardware fifo channel */
            if ( pipe( f ) < 0 )
            {
                return false;
            }

            fcntl( f[1], F_SETFL, O_NONBLOCK );
            _r.c[t].handle = f[1];

            params = ( struct _runThreadParams * )malloc( sizeof( struct _runThreadParams ) );
            params->listenHandle = f[0];
            params->portNo = t;

            _r.c[t].fifoName = ( char * )malloc( strlen( HWFIFO_NAME ) + strlen( options.chanPath ) + 2 );
            strcpy( _r.c[t].fifoName, options.chanPath );
            strcat( _r.c[t].fifoName, HWFIFO_NAME );

            if ( pthread_create( &( _r.c[t].thread ), NULL, &_runHWFifo, params ) )
            {
                return false;
            }
        }
    }

    return true;
}
// ====================================================================================================
static void _removeFifoTasks( void )

/* Destroy the per-port sub-processes */

{
    for ( int t = 0; t < NUM_CHANNELS + 1; t++ )
    {
        if ( _r.c[t].handle > 0 )
        {
            close( _r.c[t].handle );
            unlink( _r.c[t].fifoName );
        }
    }
}
// ====================================================================================================
uint64_t _timestampuS( void )

{
    struct timeval te;
    gettimeofday( &te, NULL ); // get current time
    return ( te.tv_sec * 1000000LL + te.tv_usec ); // caculate microseconds
}
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Network server implementation for raw SWO feed
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
static void *_client( void *args )

/* Handle an individual network client account */

{
    struct nwClientParams *params = ( struct nwClientParams * )args;
    int readDataLen;
    uint8_t maxTransitPacket[TRANSFER_SIZE];

    while ( 1 )
    {
        readDataLen = read( params->listenHandle, maxTransitPacket, TRANSFER_SIZE );

        if ( ( readDataLen <= 0 ) || ( write( params->portNo, maxTransitPacket, readDataLen ) < 0 ) )
        {
            /* This port went away, so remove it */
            genericsReport( V_INFO, "Connection dropped" EOL );

            close( params->portNo );
            close( params->listenHandle );

            /* First of all, make sure we can get access to the client list */
            sem_wait( &_r.clientList );

            if ( params->client->prevClient )
            {
                params->client->prevClient->nextClient = params->client->nextClient;
            }
            else
            {
                _r.firstClient = params->client->nextClient;
            }

            if ( params->client->nextClient )
            {
                params->client->nextClient->prevClient = params->client->prevClient;
            }

            /* OK, we made our modifications */
            sem_post( &_r.clientList );

            return NULL;
        }
    }
}

// ====================================================================================================
static void *_listenTask( void *arg )

{
    int sockfd = *( ( int * )arg );
    int newsockfd;
    socklen_t clilen;
    struct sockaddr_in cli_addr;
    int f[2];                               /* File descriptor set for pipe */
    struct nwClientParams *params;
    char s[100];

    while ( 1 )
    {
        listen( sockfd, 5 );
        clilen = sizeof( cli_addr );
        newsockfd = accept( sockfd, ( struct sockaddr * ) &cli_addr, &clilen );

        inet_ntop( AF_INET, &cli_addr.sin_addr, s, 99 );
        genericsReport( V_INFO, "New connection from %s" EOL, s );

        /* We got a new connection - spawn a thread to handle it */
        if ( !pipe( f ) )
        {
            params = ( struct nwClientParams * )malloc( sizeof( struct nwClientParams ) );

            params->client = ( struct nwClient * )malloc( sizeof( struct nwClient ) );
            params->client->handle = f[1];
            params->listenHandle = f[0];
            params->portNo = newsockfd;

            if ( !pthread_create( &( params->client->thread ), NULL, &_client, params ) )
            {
                /* Auto-cleanup for this thread */
                pthread_detach( params->client->thread );

                /* Hook into linked list */
                sem_wait( &_r.clientList );

                params->client->nextClient = _r.firstClient;
                params->client->prevClient = NULL;

                if ( params->client->nextClient )
                {
                    params->client->nextClient->prevClient = params->client;
                }

                _r.firstClient = params->client;

                sem_post( &_r.clientList );
            }
        }
    }

    return NULL;
}
// ====================================================================================================
static bool _makeServerTask( int port )

/* Creating the listening server thread */

{
    static int sockfd;  /* This needs to be static to keep it available for the inferior */
    struct sockaddr_in serv_addr;
    int flag = 1;

    sockfd = socket( AF_INET, SOCK_STREAM, 0 );
    setsockopt( sockfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof( flag ) );

    if ( sockfd < 0 )
    {
        genericsReport( V_ERROR, "Error opening socket" EOL );
        return false;
    }

    bzero( ( char * ) &serv_addr, sizeof( serv_addr ) );
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons( port );

    if ( setsockopt( sockfd, SOL_SOCKET, SO_REUSEADDR, &( int )
{
    1
}, sizeof( int ) ) < 0 )
    {
        genericsReport( V_ERROR, "setsockopt(SO_REUSEADDR) failed" );
        return false;
    }

    if ( bind( sockfd, ( struct sockaddr * ) &serv_addr, sizeof( serv_addr ) ) < 0 )
    {
        genericsReport( V_ERROR, "Error on binding" EOL );
        return false;
    }

    /* We have the listening socket - spawn a thread to handle it */
    if ( pthread_create( &( _r.ipThread ), NULL, &_listenTask, &sockfd ) )
    {
        genericsReport( V_ERROR, "Failed to create listening thread" EOL );
        return false;
    }

    return true;
}
// ====================================================================================================
static void _sendToClients( uint32_t len, uint8_t *buffer )

{
    struct nwClient *n = _r.firstClient;

    sem_wait( &_r.clientList );

    while ( n )
    {
        write( n->handle, buffer, len );
        n = n->nextClient;
    }

    sem_post( &_r.clientList );
}
// ====================================================================================================
void _handleException( struct ITMDecoder *i, struct ITMPacket *p )

{
    uint64_t ts = _timestampuS(); /* Stamp as early as possible */
    char outputString[MAX_STRING_LENGTH];
    int opLen;
    uint32_t exceptionNumber = ( ( p->d[1] & 0x01 ) << 8 ) | p->d[0];
    uint32_t eventType = p->d[1] >> 4;
    uint64_t eventdifftS = ts - _r.lastHWExceptionTS;


    const char *exNames[] = {"Thread", "Reset", "NMI", "HardFault", "MemManage", "BusFault", "UsageFault", "UNKNOWN_7",
                             "UNKNOWN_8", "UNKNOWN_9", "UNKNOWN_10", "SVCall", "Debug Monitor", "UNKNOWN_13", "PendSV", "SysTick"
                            };
    const char *exEvent[] = {"Unknown", "Enter", "Exit", "Resume"};

    _r.lastHWExceptionTS = ts;

    if ( exceptionNumber < 16 )
    {
        /* This is a system based exception */
        opLen = snprintf( outputString, MAX_STRING_LENGTH, "%d,%ld,%s,%s" EOL, HWEVENT_EXCEPTION, eventdifftS, exEvent[eventType & 0x03], exNames[exceptionNumber & 0x0F] );
    }
    else
    {
        /* This is a CPU defined exception */
        opLen = snprintf( outputString, MAX_STRING_LENGTH, "%d,%ld,%s,External,%d" EOL, HWEVENT_EXCEPTION, eventdifftS, exEvent[eventType & 0x03], exceptionNumber - 16 );
    }

    write( _r.c[HW_CHANNEL].handle, outputString, opLen );
}
// ====================================================================================================
void _handleDWTEvent( struct ITMDecoder *i, struct ITMPacket *p )

{
    uint64_t ts = _timestampuS(); /* Stamp as early as possible */
    char outputString[MAX_STRING_LENGTH];
    int opLen;
    uint32_t event = p->d[1] & 0x2F;
    const char *evName[] = {"CPI", "Exc", "Sleep", "LSU", "Fold", "Cyc"};
    uint64_t eventdifftS = ts - _r.lastHWExceptionTS;

    _r.lastHWExceptionTS = ts;

    for ( uint32_t i = 0; i < 6; i++ )
    {
        if ( event & ( 1 << i ) )
        {
            opLen = snprintf( outputString, MAX_STRING_LENGTH, "%d,%ld,%s" EOL, HWEVENT_DWT, eventdifftS, evName[event] );
            write( _r.c[HW_CHANNEL].handle, outputString, opLen );
        }
    }
}
// ====================================================================================================
void _handlePCSample( struct ITMDecoder *i, struct ITMPacket *p )

/* We got a sample of the PC */

{
    uint64_t ts = _timestampuS(); /* Stamp as early as possible */
    char outputString[MAX_STRING_LENGTH];
    int opLen;
    uint64_t eventdifftS = ts - _r.lastHWExceptionTS;

    _r.lastHWExceptionTS = ts;

    if ( p->len == 1 )
    {
        /* This is a sleep packet */
        opLen = snprintf( outputString, ( MAX_STRING_LENGTH - 1 ), "%d,%ld,**SLEEP**" EOL, HWEVENT_PCSample, eventdifftS );
    }
    else
    {
        uint32_t pc = ( p->d[3] << 24 ) | ( p->d[2] << 16 ) | ( p->d[1] << 8 ) | ( p->d[0] );
        opLen = snprintf( outputString, ( MAX_STRING_LENGTH - 1 ), "%d,%ld,0x%08x" EOL, HWEVENT_PCSample, eventdifftS, pc );
    }

    write( _r.c[HW_CHANNEL].handle, outputString, opLen );
}
// ====================================================================================================
void _handleDataRWWP( struct ITMDecoder *i, struct ITMPacket *p )

/* We got an alert due to a watch pointer */

{
    uint64_t ts = _timestampuS(); /* Stamp as early as possible */
    uint8_t comp = ( p->srcAddr >> 1) & 0x3;
    bool isWrite = ( ( p->srcAddr & 0x01 ) != 0 );
    uint32_t data;
    char outputString[MAX_STRING_LENGTH];
    int opLen;
    uint64_t eventdifftS = ts - _r.lastHWExceptionTS;

    _r.lastHWExceptionTS = ts;

    switch ( p->len )
    {
        case 1:
            data = p->d[0];
            break;

        case 2:
            data = ( p->d[0] ) | ( ( p->d[1] ) << 8 );
            break;

        default:
            data = ( p->d[0] ) | ( ( p->d[1] ) << 8 ) | ( ( p->d[2] ) << 16 ) | ( ( p->d[3] ) << 24 );
            break;
    }

    opLen = snprintf( outputString, MAX_STRING_LENGTH, "%d,%ld,%d,%s,0x%x" EOL, HWEVENT_RWWT, eventdifftS, comp, isWrite ? "Write" : "Read", data );
    write( _r.c[HW_CHANNEL].handle, outputString, opLen );
}
// ====================================================================================================
void _handleDataAccessWP( struct ITMDecoder *i, struct ITMPacket *p )

/* We got an alert due to a watchpoint */

{
    uint64_t ts = _timestampuS(); /* Stamp as early as possible */
    uint8_t comp = ( p->srcAddr >> 1) & 0x3;
    uint32_t data = ( p->d[0] ) | ( ( p->d[1] ) << 8 ) | ( ( p->d[2] ) << 16 ) | ( ( p->d[3] ) << 24 );
    char outputString[MAX_STRING_LENGTH];
    int opLen;

    uint64_t eventdifftS = ts - _r.lastHWExceptionTS;

    _r.lastHWExceptionTS = ts;
    opLen = snprintf( outputString, MAX_STRING_LENGTH, "%d,%ld,%d,0x%08x" EOL, HWEVENT_AWP, eventdifftS, comp, data );
    write( _r.c[HW_CHANNEL].handle, outputString, opLen );
}
// ====================================================================================================
void _handleDataOffsetWP( struct ITMDecoder *i, struct ITMPacket *p )

/* We got an alert due to an offset write event */

{
    uint64_t ts = _timestampuS(); /* Stamp as early as possible */
    uint8_t comp = ( p->srcAddr >> 1) & 0x3;
    uint32_t offset = ( p->d[0] ) | ( ( p->d[1] ) << 8 );
    char outputString[MAX_STRING_LENGTH];
    int opLen;
    uint64_t eventdifftS = ts - _r.lastHWExceptionTS;

    _r.lastHWExceptionTS = ts;
    opLen = snprintf( outputString, MAX_STRING_LENGTH, "%d,%ld,%d,0x%04x" EOL, HWEVENT_OFS, eventdifftS, comp, offset );
    write( _r.c[HW_CHANNEL].handle, outputString, opLen );
}
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Handlers for each message type
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
void _handleSW( struct ITMDecoder *i )

{
    struct ITMPacket p;

    if ( ITMGetPacket( i, &p ) )
    {
        /* Filter off filewriter packets and let the filewriter module deal with those */
        if ( p.srcAddr == FW_CHANNEL )
        {
            filewriterProcess( &p );
        }
        else
        {
            if ( ( p.srcAddr < NUM_CHANNELS ) && ( _r.c[p.srcAddr].handle ) )
            {
                write( _r.c[p.srcAddr].handle, &p, sizeof( struct ITMPacket ) );
            }
        }
    }
}
// ====================================================================================================
void _handleHW( struct ITMDecoder *i )

/* ... a hardware event has been received, dispatch it */

{
    struct ITMPacket p;
    ITMGetPacket( i, &p );

    switch ( p.srcAddr )
    {
        // --------------
        case 0: /* DWT Event */
            break;

        // --------------
        case 1: /* Exception */
            _handleException( i, &p );
            break;

        // --------------
        case 2: /* PC Counter Sample */
            _handlePCSample( i, &p );
            break;

        // --------------
        default:
            if ( ( p.srcAddr & 0x19 ) == 0x11)
            {
                _handleDataRWWP( i, &p );
            }
            else if ( ( p.srcAddr & 0x19 ) == 0x08 )
            {
                _handleDataAccessWP( i, &p );
            }
            else if ( ( p.srcAddr & 0x19 ) == 0x09 )
            {
                _handleDataOffsetWP( i, &p );
            }

            break;
            // --------------
    }
}
// ====================================================================================================
void _handleTS( struct ITMDecoder *i )

/* ... a timestamp */

{
    char outputString[MAX_STRING_LENGTH];
    int opLen;
    struct ITMPacket p;
    uint32_t stamp = 0;

    if ( ITMGetPacket( i, &p ) )
    {
        if ( !( p.d[0] & 0x80 ) )
        {
            /* This is packet format 2 ... just a simple increment */
            stamp = p.d[0] >> 4;
        }
        else
        {
            /* This is packet format 1 ... full decode needed */
            i->timeStatus = ( p.d[0] & 0x30 ) >> 4;
            stamp = ( p.d[1] ) & 0x7f;

            if ( p.len > 2 )
            {
                stamp |= ( p.d[2] ) << 7;

                if ( p.len > 3 )
                {
                    stamp |= ( p.d[3] & 0x7F ) << 14;

                    if ( p.len > 4 )
                    {
                        stamp |= ( p.d[4] & 0x7f ) << 21;
                    }
                }
            }
        }

        i->timeStamp += stamp;
    }

    opLen = snprintf( outputString, MAX_STRING_LENGTH, "%d,%d,%" PRIu64 EOL, HWEVENT_TS, i->timeStatus, i->timeStamp );
    write( _r.c[HW_CHANNEL].handle, outputString, opLen );
}
// ====================================================================================================
void _itmPumpProcess( char c )

/* Handle individual characters into the itm decoder */

{
    switch ( ITMPump( &_r.i, c ) )
    {
        // ------------------------------------
        case ITM_EV_NONE:
            break;

        // ------------------------------------
        case ITM_EV_UNSYNCED:
            genericsReport( V_WARN, "ITM Lost Sync (%d)" EOL, ITMDecoderGetStats( &_r.i )->lostSyncCount );
            break;

        // ------------------------------------
        case ITM_EV_SYNCED:
            genericsReport( V_INFO, "ITM In Sync (%d)" EOL, ITMDecoderGetStats( &_r.i )->syncCount );
            break;

        // ------------------------------------
        case ITM_EV_OVERFLOW:
            genericsReport( V_WARN, "ITM Overflow (%d)" EOL, ITMDecoderGetStats( &_r.i )->overflow );
            break;

        // ------------------------------------
        case ITM_EV_ERROR:
            genericsReport( V_WARN, "ITM Error" EOL );
            break;

        // ------------------------------------
        case ITM_EV_TS_PACKET_RXED:
            _handleTS( &_r.i );
            break;

        // ------------------------------------
        case ITM_EV_SW_PACKET_RXED:
            _handleSW( &_r.i );
            break;

        // ------------------------------------
        case ITM_EV_HW_PACKET_RXED:
            _handleHW( &_r.i );
            break;

        // ------------------------------------
        case ITM_EV_RESERVED_PACKET_RXED:
            genericsReport( V_INFO, "Reserved Packet Received" EOL );
            break;

        // ------------------------------------
        case ITM_EV_XTN_PACKET_RXED:
            genericsReport( V_INFO, "Unknown Extension Packet Received" EOL );
            break;

            // ------------------------------------
    }
}
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Protocol pump for decoding messages
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
void _protocolPump( uint8_t c )

/* Top level protocol pump */

{
    if ( options.useTPIU )
    {
        switch ( TPIUPump( &_r.t, c ) )
        {
            // ------------------------------------
            case TPIU_EV_NEWSYNC:
                genericsReport( V_INFO, "TPIU In Sync (%d)" EOL, TPIUDecoderGetStats( &_r.t )->syncCount );

            // This fall-through is deliberate
            case TPIU_EV_SYNCED:

                ITMDecoderForceSync( &_r.i, true );
                break;

            // ------------------------------------
            case TPIU_EV_RXING:
            case TPIU_EV_NONE:
                break;

            // ------------------------------------
            case TPIU_EV_UNSYNCED:
                genericsReport( V_INFO, "TPIU Lost Sync (%d)" EOL, TPIUDecoderGetStats( &_r.t )->lostSync );
                ITMDecoderForceSync( &_r.i, false );
                break;

            // ------------------------------------
            case TPIU_EV_RXEDPACKET:
                if ( !TPIUGetPacket( &_r.t, &_r.p ) )
                {
                    genericsReport( V_WARN, "TPIUGetPacket fell over" EOL );
                }

                for ( uint32_t g = 0; g < _r.p.len; g++ )
                {
                    if ( _r.p.packet[g].s == options.tpiuITMChannel )
                    {
                        _itmPumpProcess( _r.p.packet[g].d );
                        continue;
                    }

                    if ( ( _r.p.packet[g].s != 0 ) && ( _r.p.packet[g].s != 0x7f ) )
                    {
                        genericsReport( V_INFO, "Unknown TPIU channel %02x" EOL, _r.p.packet[g].s );
                    }
                }

                break;

            // ------------------------------------
            case TPIU_EV_ERROR:
                genericsReport( V_ERROR, "****ERROR****" EOL );
                break;
                // ------------------------------------
        }
    }
    else
    {
        /* There's no TPIU in use, so this goes straight to the ITM layer */
        _itmPumpProcess( c );
    }
}
// ====================================================================================================
static void _intHandler( int sig, siginfo_t *si, void *unused )

{
    /* CTRL-C exit is not an error... */
    exit( 0 );
}
// ====================================================================================================
void _printHelp( char *progName )

{
    fprintf( stdout, "Usage: %s <hntv> <s name:number> <b basedir> <f filename>  <i channel> <p port> <a speed>" EOL, progName );
    fprintf( stdout, "        a: <serialSpeed> to use" EOL );
    fprintf( stdout, "        b: <basedir> for channels" EOL );
    fprintf( stdout, "        c: <Number>,<Name>,<Format> of channel to populate (repeat per channel)" EOL );
    fprintf( stdout, "        f: <filename> Take input from specified file" EOL );
    fprintf( stdout, "        h: This help" EOL );
    fprintf( stdout, "        i: <channel> Set ITM Channel in TPIU decode (defaults to 1)" EOL );
    fprintf( stdout, "        l: <port> Listen port for the incoming connections (defaults to %d)" EOL, SERVER_PORT );
    fprintf( stdout, "        n: Enforce sync requirement for ITM (i.e. ITM needs to issue syncs)" EOL );
#ifdef INCLUDE_FPGA_SUPPORT
    fprintf( stdout, "        o: <num> Use traceport FPGA custom interface with 1, 2 or 4 bits width" EOL );
#endif
    fprintf( stdout, "        p: <serialPort> to use" EOL );
    fprintf( stdout, "        s: <address>:<port> Set address for SEGGER JLink connection (default none:%d)" EOL, SEGGER_PORT );
    fprintf( stdout, "        t: Use TPIU decoder" EOL );
    fprintf( stdout, "        v: <level> Verbose mode 0(errors)..3(debug)" EOL );
}
// ====================================================================================================
int _processOptions( int argc, char *argv[] )

{
    int c;
    char *chanConfig;
    uint chan;
    char *chanIndex;
#define DELIMITER ','

    while ( ( c = getopt ( argc, argv, "a:b:c:f:hi:l:no:p:s:tv:" ) ) != -1 )
        switch ( c )
        {
            // ------------------------------------
            case 'a':
                options.speed = atoi( optarg );
                break;

            // ------------------------------------
            case 'b':
                options.chanPath = optarg;
                break;

            // ------------------------------------
            case 'f':
                options.file = optarg;
                break;

            // ------------------------------------
            case 'h':
                _printHelp( argv[0] );
                return false;

            // ------------------------------------
            case 'i':
                options.tpiuITMChannel = atoi( optarg );
                break;

            case 'l':
                options.listenPort = atoi( optarg );
                break;

            // ------------------------------------
            case 'n':
                options.forceITMSync = false;
                break;
                // ------------------------------------
#ifdef INCLUDE_FPGA_SUPPORT

            case 'o':
                // Generally you need TPIU for orbtrace
                options.useTPIU = true;
                options.orbtrace = true;
                options.orbtraceWidth = atoi( optarg );
                break;
#endif

            // ------------------------------------
            case 'p':
                options.port = optarg;
                break;

            // ------------------------------------
            case 's':
                options.forceITMSync = true;
                options.seggerHost = optarg;

                // See if we have an optional port number too
                char *a = optarg;

                while ( ( *a ) && ( *a != ':' ) )
                {
                    a++;
                }

                if ( *a == ':' )
                {
                    *a = 0;
                    options.seggerPort = atoi( ++a );
                }

                if ( !options.seggerPort )
                {
                    options.seggerPort = SEGGER_PORT;
                }

                break;

            // ------------------------------------
            case 't':
                options.useTPIU = true;
                break;

            // ------------------------------------
            case 'v':
                genericsSetReportLevel( atoi( optarg ) );
                break;

            // ------------------------------------
            /* Individual channel setup */
            case 'c':
                chanIndex = chanConfig = strdup( optarg );
                chan = atoi( optarg );

                if ( chan >= NUM_CHANNELS )
                {
                    genericsReport( V_ERROR, "Channel index out of range" EOL );
                    return false;
                }

                /* Scan for start of filename */
                while ( ( *chanIndex ) && ( *chanIndex != DELIMITER ) )
                {
                    chanIndex++;
                }

                if ( !*chanIndex )
                {
                    genericsReport( V_ERROR, "No filename for channel %d" EOL, chan );
                    return false;
                }

                options.channel[chan].chanName = ++chanIndex;

                /* Scan for format */
                while ( ( *chanIndex ) && ( *chanIndex != DELIMITER ) )
                {
                    chanIndex++;
                }

                if ( !*chanIndex )
                {
                    genericsReport( V_WARN, "No output format for channel %d, output raw!" EOL, chan );
                    options.channel[chan].presFormat = NULL;
                    break;
                }

                *chanIndex++ = 0;
                options.channel[chan].presFormat = strdup( GenericsUnescape( chanIndex ) );
                break;

            // ------------------------------------


            case '?':
                if ( optopt == 'b' )
                {
                    genericsReport( V_ERROR, "Option '%c' requires an argument." EOL, optopt );
                }
                else if ( !isprint ( optopt ) )
                {
                    genericsReport( V_ERROR, "Unknown option character `\\x%x'." EOL, optopt );
                }

                return false;

            // ------------------------------------
            default:
                genericsReport( V_ERROR, "%c" EOL, c );
                return false;
                // ------------------------------------
        }

    /* Now perform sanity checks.... */
    if ( ( options.useTPIU ) && ( !options.tpiuITMChannel ) )
    {
        genericsReport( V_ERROR, "TPIU set for use but no channel set for ITM output" EOL );
        return false;
    }

#ifdef INCLUDE_FPGA_SUPPORT

    if ( ( options.orbtrace ) && !( ( options.orbtraceWidth == 1 ) || ( options.orbtraceWidth == 2 ) || ( options.orbtraceWidth == 4 ) ) )
    {
        genericsReport( V_ERROR, "Orbtrace interface illegal port width" EOL );
        return false;
    }

#endif

    /* ... and dump the config if we're being verbose */
    genericsReport( V_INFO, "Orbuculum V" VERSION " (Git %08X %s, Built " BUILD_DATE ")" EOL, GIT_HASH, ( GIT_DIRTY ? "Dirty" : "Clean" ) );

    genericsReport( V_INFO, "BasePath   : %s" EOL, options.chanPath );
    genericsReport( V_INFO, "ForceSync  : %s" EOL, options.forceITMSync ? "true" : "false" );

    if ( options.port )
    {
        genericsReport( V_INFO, "Serial Port : %s" EOL "Serial Speed: %d" EOL, options.port, options.speed );
    }

    if ( options.seggerPort )
    {
        genericsReport( V_INFO, "SEGGER H&P : %s:%d" EOL, options.seggerHost, options.seggerPort );
    }

#ifdef INCLUDE_FPGA_SUPPORT

    if ( options.orbtrace )
    {
        genericsReport( V_INFO, "Orbtrace   : %d bits width" EOL, options.orbtraceWidth );
    }

#endif

    if ( options.useTPIU )
    {
        genericsReport( V_INFO, "Using TPIU : true (ITM on channel %d)" EOL, options.tpiuITMChannel );
    }

    if ( options.file )
    {
        genericsReport( V_INFO, "Input File : %s" EOL, options.file );
    }

    genericsReport( V_INFO, "Channels   :" EOL );

    for ( int g = 0; g < NUM_CHANNELS; g++ )
    {
        if ( options.channel[g].chanName )
        {
            genericsReport( V_INFO, "         %02d [%s] [%s]" EOL, g, GenericsEscape( options.channel[g].presFormat ?: "RAW" ), options.channel[g].chanName );
        }
    }

    genericsReport( V_INFO, "         HW [Predefined] [" HWFIFO_NAME "]" EOL );

    if ( ( options.file ) && ( ( options.port ) || ( options.seggerPort ) ) )
    {
        genericsReport( V_ERROR, "Cannot specify file and port or Segger at same time" EOL );
        return false;
    }

    if ( ( options.port ) && ( options.seggerPort ) )
    {
        genericsReport( V_ERROR, "Cannot specify port and Segger at same time" EOL );
        return false;
    }

    return true;
}
// ====================================================================================================
int usbFeeder( void )

{

    unsigned char cbw[TRANSFER_SIZE];
    libusb_device_handle *handle;
    libusb_device *dev;
    int size;
    int32_t err;

    while ( 1 )
    {
        if ( libusb_init( NULL ) < 0 )
        {
            genericsReport( V_ERROR, "Failed to initalise USB interface" EOL );
            return ( -1 );
        }

        genericsReport( V_INFO, "Opening USB Device" EOL );

        /* Snooze waiting for the device to appear .... this is useful for when they come and go */
        while ( !( handle = libusb_open_device_with_vid_pid( NULL, VID, PID ) ) )
        {
            usleep( 500000 );
        }

        genericsReport( V_INFO, "USB Device opened" EOL );

        if ( !( dev = libusb_get_device( handle ) ) )
        {
            /* We didn't get the device, so try again in a while */
            continue;
        }

        if ( ( err = libusb_claim_interface ( handle, SWO_INTERFACE ) ) < 0 )
        {
            genericsReport( V_ERROR, "Failed to claim interface (%d)" EOL, err );
            return 0;
        }

        int32_t r;

        genericsReport( V_INFO, "USB Interface claimed, ready for data" EOL );

        while ( true )
        {
            r = libusb_bulk_transfer( handle, ENDPOINT, cbw, TRANSFER_SIZE, &size, 10 );

            if ( ( r < 0 ) && ( r != LIBUSB_ERROR_TIMEOUT ) )
            {
                genericsReport( V_INFO, "USB data collection failed with error %d" EOL, r );
                break;
            }

            _sendToClients( size, cbw );
            unsigned char *c = cbw;

            genericsReport( V_DEBUG, "RXED Packet of %d bytes" EOL, size );

            while ( size-- )
            {
                _protocolPump( *c++ );
            }
        }

        libusb_close( handle );
        genericsReport( V_INFO, "USB Interface closed" EOL );
    }

    return 0;
}
// ====================================================================================================
int seggerFeeder( void )

{
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    uint8_t cbw[TRANSFER_SIZE];

    ssize_t t;
    int flag = 1;

    bzero( ( char * ) &serv_addr, sizeof( serv_addr ) );
    server = gethostbyname( options.seggerHost );

    if ( !server )
    {
        genericsReport( V_ERROR, "Cannot find host" EOL );
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    bcopy( ( char * )server->h_addr,
           ( char * )&serv_addr.sin_addr.s_addr,
           server->h_length );
    serv_addr.sin_port = htons( options.seggerPort );

    while ( 1 )
    {
        sockfd = socket( AF_INET, SOCK_STREAM, 0 );
        setsockopt( sockfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof( flag ) );

        if ( sockfd < 0 )
        {
            genericsReport( V_ERROR, "Error creating socket" EOL );
            return -1;
        }

        while ( connect( sockfd, ( struct sockaddr * ) &serv_addr, sizeof( serv_addr ) ) < 0 )
        {
            usleep( 500000 );
        }

        genericsReport( V_INFO, "Established Segger Link" EOL );

        TPIUDecoderForceSync( &_r.t, 0 );
        ITMDecoderForceSync( &_r.i, true );

        while ( ( t = read( sockfd, cbw, TRANSFER_SIZE ) ) > 0 )
        {
            _sendToClients( t, cbw );
            uint8_t *c = cbw;

            while ( t-- )
            {
                _protocolPump( *c++ );
            }
        }

        close( sockfd );

        genericsReport( V_INFO, "Lost Segger Link" EOL );
    }

    return -2;
}
// ====================================================================================================
#if defined(LINUX) && defined (TCGETS2)
int setSerialConfig ( int f, speed_t speed )
{
    // Use Linux specific termios2.
    struct termios2 settings;
    int ret = ioctl( f, TCGETS2, &settings );

    if ( ret < 0 )
    {
        return ( -3 );
    }

    settings.c_iflag &= ~( ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF );
    settings.c_lflag &= ~( ICANON | ECHO | ECHOE | ISIG );
    settings.c_cflag &= ~PARENB; /* no parity */
    settings.c_cflag &= ~CSTOPB; /* 1 stop bit */
    settings.c_cflag &= ~CSIZE;
    settings.c_cflag &= ~( CBAUD | CIBAUD );
    settings.c_cflag |= CS8 | CLOCAL; /* 8 bits */
    settings.c_oflag &= ~OPOST; /* raw output */

    const unsigned int speed1[] =
    {
        B115200, B230400, 0, B460800, B576000,
        0, 0, B921600, 0, B1152000
    };
    const unsigned int speed2[] =
    {
        B500000,  B1000000, B1500000, B2000000,
        B2500000, B3000000, B3500000, B4000000
    };
    int speed_ok = 0;

    if ( ( speed % 500000 ) == 0 )
    {
        // speed is multiple of 500000, use speed2 table.
        int i = speed / 500000;

        if ( i <= 8 )
        {
            speed_ok = speed2[i - 1];
        }
    }
    else if ( ( speed % 115200 ) == 0 )
    {
        int i = speed / 115200;

        if ( i <= 10 && speed1[i - 1] )
        {
            speed_ok = speed2[i - 1];
        }
    }

    if ( speed_ok )
    {
        settings.c_cflag |= speed_ok;
    }
    else
    {
        settings.c_cflag |= BOTHER;
        settings.c_ispeed = speed;
        settings.c_ospeed = speed;
    }

    // Ensure input baud is same than output.
    settings.c_cflag |= ( settings.c_cflag & CBAUD ) << IBSHIFT;
    // Now configure port.
    ret = ioctl( f, TCSETS2, &settings );

    if ( ret < 0 )
    {
        genericsReport( V_ERROR, "Unsupported baudrate" EOL );
        return ( -3 );
    }

    // Check configuration is ok.
    ret = ioctl( f, TCGETS2, &settings );

    if ( ret < 0 )
    {
        return ( -3 );
    }

    if ( speed_ok )
    {
        if ( ( settings.c_cflag & CBAUD ) != speed_ok )
        {
            genericsReport( V_WARN, "Fail to set baudrate" EOL );
        }
    }
    else
    {
        if ( ( settings.c_ispeed != speed ) || ( settings.c_ospeed != speed ) )
        {
            genericsReport( V_WARN, "Fail to set baudrate" EOL );
        }
    }

    // Flush port.
    ioctl( f, TCFLSH, TCIOFLUSH );
    return 0;
}
#else
int setSerialConfig ( int f, speed_t speed )
{
    struct termios settings;

    if ( tcgetattr( f, &settings ) < 0 )
    {
        perror( "tcgetattr" );
        return ( -3 );
    }

    if ( cfsetspeed( &settings, speed ) < 0 )
    {
        genericsReport( V_ERROR, "Error Setting input speed" EOL );
        return ( -3 );
    }

    settings.c_iflag &= ~( ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF );
    settings.c_lflag &= ~( ICANON | ECHO | ECHOE | ISIG );
    settings.c_cflag &= ~PARENB; /* no parity */
    settings.c_cflag &= ~CSTOPB; /* 1 stop bit */
    settings.c_cflag &= ~CSIZE;
    settings.c_cflag |= CS8 | CLOCAL; /* 8 bits */
    settings.c_oflag &= ~OPOST; /* raw output */

    if ( tcsetattr( f, TCSANOW, &settings ) < 0 )
    {
        genericsReport( V_ERROR, "Unsupported baudrate" EOL );
        return ( -3 );
    }

    tcflush( f, TCOFLUSH );
    return 0;
}
#endif
// ====================================================================================================
int serialFeeder( void )
{
    int f, ret;
    unsigned char cbw[TRANSFER_SIZE];
    ssize_t t;

    while ( 1 )
    {
#ifdef OSX

        while ( ( f = open( options.port, O_RDONLY | O_NONBLOCK ) ) < 0 )
#else
        while ( ( f = open( options.port, O_RDONLY ) ) < 0 )
#endif
        {
            genericsReport( V_WARN, "Can't open serial port" EOL );
            usleep( 500000 );
        }

        genericsReport( V_INFO, "Port opened" EOL );

#ifdef OSX
        /* Remove the O_NONBLOCK flag now the port is open (OSX Only) */

        if ( ( flags = fcntl( f, F_GETFL, NULL ) ) < 0 )
        {
            genericsExit( -3, "F_GETFL failed" EOL );
        }

        flags &= ~O_NONBLOCK;

        if ( ( flags = fcntl( f, F_SETFL, flags ) ) < 0 )
        {
            genericsExit( -3, "F_SETFL failed" EOL );
        }

#endif

        if ( ( ret = setSerialConfig ( f, options.speed ) ) < 0 )
        {
            genericsExit( ret, "setSerialConfig failed" EOL );
        }

        while ( ( t = read( f, cbw, TRANSFER_SIZE ) ) > 0 )
        {
            _sendToClients( t, cbw );
            unsigned char *c = cbw;

            while ( t-- )
            {
                _protocolPump( *c++ );
            }
        }

        genericsReport( V_INFO, "Read failed" EOL );

        close( f );
    }
}
// ====================================================================================================
#ifdef INCLUDE_FPGA_SUPPORT

int fpgaFeeder( void )

{
    int f, t;
    uint8_t cbw[FTDI_HS_TRANSFER_SIZE];
    uint8_t scratchBuffer[FTDI_HS_TRANSFER_SIZE - FTDI_NUM_FRAMES];
    uint8_t *c;
    uint8_t *d;
    uint8_t initSequence[] = {0xA5, 0xAC};

    // Insert appropriate control byte to set port width for trace
    initSequence[1] = ( char[] )
    {
        0xA0, 0xA4, 0xAC, 0xAC
    }[options.orbtraceWidth - 1];

    // FTDI Chip takes a little while to reset itself
    // usleep( 400000 );

    _r.feederExit = false;

    while ( !_r.feederExit )
    {
        _r.ftdi = ftdi_new();
        ftdi_set_interface( _r.ftdi, FTDI_INTERFACE );


        do
        {
            f = ftdi_usb_open( _r.ftdi, FTDI_VID, FTDI_PID );

            if ( f < 0 )
            {
                genericsReport( V_WARN, "Cannot open device (%s)" EOL, ftdi_get_error_string( _r.ftdi ) );
                usleep( 50000 );
            }
        }
        while ( ( f < 0 ) && ( !_r.feederExit ) );

        genericsReport( V_INFO, "Port opened" EOL );
        f = ftdispi_open( &_r.ftdifsc, _r.ftdi, FTDI_INTERFACE );

        if ( f < 0 )
        {
            genericsReport( V_ERROR, "Cannot open spi %d (%s)" EOL, f, ftdi_get_error_string( _r.ftdi ) );
            return -2;
        }

        ftdispi_setmode( &_r.ftdifsc, 1, 0, 1, 0, 0, FPGA_AWAKE );

        ftdispi_setloopback( &_r.ftdifsc, 0 );

        f = ftdispi_setclock( &_r.ftdifsc, FTDI_INTERFACE_SPEED );

        if ( f < 0 )
        {
            genericsReport( V_ERROR, "Cannot set clockrate %d %d (%s)" EOL, f, FTDI_INTERFACE_SPEED, ftdi_get_error_string( _r.ftdi ) );
            return -2;
        }

        genericsReport( V_INFO, "All parameters configured" EOL );


        TPIUDecoderForceSync( &_r.t, 0 );

        while ( ( !_r.feederExit ) && ( ( t = ftdispi_write_read( &_r.ftdifsc, initSequence, 2, cbw, FTDI_HS_TRANSFER_SIZE, FPGA_AWAKE ) ) >= 0 ) )
        {
            c = cbw;
            d = scratchBuffer;

            for ( f = 0; f < FTDI_NUM_FRAMES; f++ )
            {
                if ( ( *c ) & 0x80 )
                {
                    // This frame has no useful data
                    c += FTDI_PACKET_SIZE;
                    continue;
                }
                else
                {
                    // This frame contains something - copy and feed it, excluding header byte
                    memcpy( d, &c[1], FTDI_PACKET_SIZE - 1 );
                    d += FTDI_PACKET_SIZE - 1;

                    for ( uint32_t e = 1; e < FTDI_PACKET_SIZE; e++ )
                    {
#ifdef DUMP_FTDI_BYTES
                        printf( "%02X ", c[e] );
#endif
                        _protocolPump( c[e] );
                    }

                    c += FTDI_PACKET_SIZE;
#ifdef DUMP_FTDI_BYTES
                    printf( "\n" );
#endif
                }
            }

            genericsReport( V_WARN, "RXED frame of %d/%d full packets (%3d%%)    \r",
                            ( d - scratchBuffer ) / ( FTDI_PACKET_SIZE - 1 ), FTDI_NUM_FRAMES, ( ( d - scratchBuffer ) * 100 ) / ( FTDI_HS_TRANSFER_SIZE - FTDI_NUM_FRAMES ) );

            _sendToClients( ( d - scratchBuffer ), scratchBuffer );
        }

        genericsReport( V_WARN, "Exit Requested (%d, %s)" EOL, t, ftdi_get_error_string( _r.ftdi ) );

        ftdispi_setgpo( &_r.ftdifsc, FPGA_ASLEEP );
        ftdispi_close( &_r.ftdifsc, 1 );
    }

    return 0;
}

// ====================================================================================================
void fpgaFeederClose( int dummy )

{
    ( void )dummy;
    _r.feederExit = true;
}
#endif
// ====================================================================================================
int fileFeeder( void )

{
    int f;
    unsigned char cbw[TRANSFER_SIZE];
    ssize_t t;

    if ( ( f = open( options.file, O_RDONLY ) ) < 0 )
    {
        genericsExit( -4, "Can't open file %s" EOL, options.file );
    }

    genericsReport( V_INFO, "Reading from file" EOL );

    while ( ( t = read( f, cbw, TRANSFER_SIZE ) ) >= 0 )
    {

        if ( !t )
        {
            // Just spin for a while to avoid clogging the CPU
            usleep( 100000 );
            continue;
        }

        _sendToClients( t, cbw );
        unsigned char *c = cbw;

        while ( t-- )
        {
            _protocolPump( *c++ );
        }
    }

    genericsReport( V_INFO, "File read error" EOL );

    close( f );
    return true;
}
// ====================================================================================================
int main( int argc, char *argv[] )

{
    sigset_t set;
    struct sigaction sa;

    if ( !_processOptions( argc, argv ) )
    {
        genericsExit( -1, "processOptions failed" EOL );
    }

    atexit( _removeFifoTasks );

    sem_init( &_r.clientList, 0, 1 );

    /* Reset the TPIU handler before we start */
    TPIUDecoderInit( &_r.t );
    ITMDecoderInit( &_r.i, options.forceITMSync );

    /* This ensures the atexit gets called */
    sa.sa_flags = SA_SIGINFO;
    sigemptyset( &sa.sa_mask );
    sa.sa_sigaction = _intHandler;

    if ( sigaction( SIGINT, &sa, NULL ) == -1 )
    {
        genericsExit( -1, "Failed to establish Int handler" EOL );
    }

    /* Don't kill a sub-process when any reader or writer evaporates */
    sigemptyset( &set );
    sigaddset( &set, SIGPIPE );

    if ( pthread_sigmask( SIG_BLOCK, &set, NULL ) == -1 )
    {
        genericsExit( -1, "Failed to establish Int handler" EOL );
    }

    if ( !_makeFifoTasks() )
    {
        genericsExit( -1, "Failed to make channel devices" EOL );
    }

    if ( !_makeServerTask( options.listenPort ) )
    {
        genericsExit( -1, "Failed to make network server" EOL );
    }

    /* Make sure there's an initial timestamp to work with */

    _r.lastHWExceptionTS = _timestampuS();

    /* Start the filewriter */
    filewriterInit( options.fwbasedir );

    /* Using the exit construct rather than return ensures any atexit gets called */
#ifdef INCLUDE_FPGA_SUPPORT

    if ( options.orbtrace )
    {
        signal( SIGINT, fpgaFeederClose );
        exit( fpgaFeeder() );
    }

#endif

    if ( options.seggerPort )
    {
        exit( seggerFeeder() );
    }

    if ( options.port )
    {
        exit( serialFeeder() );
    }

    if ( options.file )
    {
        exit( fileFeeder() );
    }

    exit( usbFeeder() );
}
// ====================================================================================================
