#ifdef __GNUC__
#define _GNU_SOURCE /* for strsignal() */
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <syslog.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>

/*************************************************************************/

/* global variables and constants */

volatile sig_atomic_t	gGracefulShutdown=0;
volatile sig_atomic_t	gCaughtHupSignal=0;

int                     gLockFileDesc=-1;
int                     gMasterSocket=-1;

const char *const       gLockFilePath = "/var/run/prime-sensors.pid";
const char *const       gLightSensorPath = "/sys/devices/platform/tegra-i2c.2/i2c-2/2-001c/show_lux";
//const char *const       gDisplayPowerPath = "/sys/devices/platform/tegra-i2c.2/i2c-2/2-001c/bl_power";
const char *const       gDisplayRegulatorPath = "/sys/class/backlight/pwm-backlight/brightness";
int const               gDisplayMinBrightness = 4;
bool                    gAutoLightOn = true;
const char *const       gAudioJackPath = "/sys/class/switch/h2w/name";
bool                    gAudioJackPlugged = false;
/*************************************************************************/

/* prototypes */

int BecomeDaemonProcess(const char *const lockFileName,
                                const char *const logPrefix,
                                const int logLevel,
                                int *const lockFileDesc,
                                int *const thisPID);

int ConfigureSignalHandlers(void);
int BindPassiveSocket(const int portNum, int *const boundSocket);
void FatalSigHandler(int sig);
void TermHandler(int sig);
void HupHandler(int sig);
void Usr1Handler(int sig);
void TidyUp(void);

/*************************************************************************/

int main(int argc,char *argv[])
{
    int   result;
    pid_t daemonPID;
    int fd, len;
    if (argc > 1) {
          pid_t pid;
          char pid_buf[16];
          if ((fd = open(gLockFilePath, O_RDONLY)) < 0)
          {
                perror("Lock file not found. May be the service is not running?");
                exit(fd);
          }
          len = read(fd, pid_buf, 16);
          pid_buf[len] = 0;
          pid = atoi(pid_buf);
          if(!strcmp(argv[1], "sensorstate")) {
                kill(pid, SIGUSR2);
                exit(EXIT_SUCCESS);
          }
          if(!strcmp(argv[1], "stop")) {
                kill(pid, SIGUSR1);
                exit(EXIT_SUCCESS);
          }
          if(!strcmp(argv[1], "restart")) {
                kill(pid, SIGHUP);
                exit(EXIT_SUCCESS);
          }
          printf ("usage %s [stop|restart]\n", argv[0]);
          exit (EXIT_FAILURE);
      }

    /*************************************************************/
    /* perhaps at this stage you would read a configuration file */
    /*************************************************************/

    /* the first task is to put ourself into the background (i.e
        become a daemon. */

    if((result=BecomeDaemonProcess(gLockFilePath,"prime-sensors",
                                             LOG_DEBUG,&gLockFileDesc,&daemonPID))<0)
    {
        perror("Failed to become daemon process");
        exit(result);
    }

    /* set up signal processing */

    if((result=ConfigureSignalHandlers())<0)
        {
        syslog(LOG_LOCAL0|LOG_INFO,"ConfigureSignalHandlers failed, errno=%d",errno);
        unlink(gLockFilePath);
        exit(result);
        }


    /* now enter an infinite loop handling connections */
    int lux;
    char data_buf[16];
    int sensorFile = open(gLightSensorPath, O_RDONLY);
//    int displayPowerFile = open(gDisplayPowerPath, O_RDONLY);
    int regulatorFile = open(gDisplayRegulatorPath, O_RDWR);
    int jackStatusFile = open(gAudioJackPath, O_RDONLY);

    syslog(LOG_LOCAL0|LOG_INFO,"fileid: %d and %d", sensorFile, regulatorFile);
    do{
        if(gAutoLightOn && sensorFile > 0 && regulatorFile > 0){
          len = read(sensorFile, data_buf, 16);
          data_buf[len] = 0;
          lux = atoi(data_buf);

          len = read(regulatorFile, data_buf, 16);
          data_buf[len] = 0;
          int curBrightness = atoi(data_buf);

	  if(curBrightness > 0){
            int calcBrightness = gDisplayMinBrightness + lux/1.5;
            if(calcBrightness > 255){
        	 calcBrightness = 255;
            }

            if(abs(curBrightness - calcBrightness) > 15){
        	 sprintf(data_buf, "%d", calcBrightness);
                write(regulatorFile, data_buf, sizeof(data_buf));
            }
	  }
        }

        if(jackStatusFile > 0){
    	    len = read(jackStatusFile, data_buf, 9);
    	    data_buf[len] = 0;

    	    if (strcmp(data_buf, "No Device") == 0){
        	if(gAudioJackPlugged){
            	    gAudioJackPlugged = false;
            	    syslog(LOG_LOCAL0|LOG_INFO, "audio headset unplugged", data_buf);
            	    system("amixer set \"Headphone Jack\" mute");
            	    system("amixer set \"Int Spk\" unmute");
        	}
    	    }else{
        	if(!gAudioJackPlugged){
            	    gAudioJackPlugged = true;
            	    syslog(LOG_LOCAL0|LOG_INFO, "audio %s plugged", data_buf);
            	    system("amixer set \"Int Spk\" mute");
            	    system("amixer set \"Headphone Jack\" unmute");
        	}
    	    }
	}
	
	lseek(sensorFile, SEEK_SET, 0);
//	lseek(displayPowerFile, SEEK_SET, 0);
	lseek(regulatorFile, SEEK_SET, 0);
	lseek(jackStatusFile, SEEK_SET, 0);

        sleep(1);
        /* the next conditional will be true if we caught signal SIGUSR1 */
        if((gGracefulShutdown==1)&&(gCaughtHupSignal==0))
            break;

        /* if we caught SIGHUP, then start handling connections again */
        gGracefulShutdown=gCaughtHupSignal=0;
    }while(1);
    close(sensorFile);
    close(regulatorFile);
    close(jackStatusFile);

    TidyUp(); /* close the socket and kill the lock file */

    return 0;
}

/**************************************************************************/
/***************************************************************************

   BecomeDaemonProcess

    Fork the process into the background, make a lock file, and open the
   system log.

    Inputs:

   lockFileName I					  the path to the lock file

   logPrefix	 I					  the string that will appear at the
                                          start of all log messages

   logLevel		 I					  the logging level for this process

   lockFileDesc O					  the file descriptor of the lock file

   thisPID		 O					  the PID of this process after fork()
                                          has placed it in the background
    Returns:

    status code indicating success - 0 = success

***************************************************************************/
/**************************************************************************/

int BecomeDaemonProcess(const char *const lockFileName,
                            const char *const logPrefix,
                            const int logLevel,
                            int *const lockFileDesc,
                            pid_t *const thisPID)
{
    int						curPID,stdioFD,lockResult,killResult,lockFD,i,numFiles;
    char                    pidBuf[17],*lfs,pidStr[7];
    FILE                    *lfp;
    unsigned long			lockPID;
    struct flock			exclusiveLock;

    /* set our current working directory to root to avoid tying up
        any directories. In a real server, we might later change to
        another directory and call chroot() for security purposes
        (especially if we are writing something that serves files */

    chdir("/");

    /* try to grab the lock file */

    lockFD=open(lockFileName,O_RDWR|O_CREAT|O_EXCL,0644);

    if(lockFD==-1)
        {
        /* Perhaps the lock file already exists. Try to open it */

        lfp=fopen(lockFileName,"r");

        if(lfp==0) /* Game over. Bail out */
            {
            perror("Can't get lockfile");
            return -1;
            }

        /* We opened the lockfile. Our lockfiles store the daemon PID in them.
            Find out what that PID is */

        lfs=fgets(pidBuf,16,lfp);

        if(lfs!=0)
            {
            if(pidBuf[strlen(pidBuf)-1]=='\n') /* strip linefeed */
                pidBuf[strlen(pidBuf)-1]=0;

            lockPID=strtoul(pidBuf,(char**)0,10);

            /* see if that process is running. Signal 0 in kill(2) doesn't
                send a signal, but still performs error checking */

            killResult=kill(lockPID,0);

            if(killResult==0)
                {
                printf("\n\nERROR\n\nA lock file %s has been detected. It appears it is owned\nby the (active) process with PID %ld.\n\n",lockFileName,lockPID);
                }
            else
                {
                if(errno==ESRCH) /* non-existent process */
                    {
                    printf("\n\nERROR\n\nA lock file %s has been detected. It appears it is owned\nby the process with PID %ld, which is now defunct. Delete the lock file\nand try again.\n\n",lockFileName,lockPID);
                    }
                else
                    {
                    perror("Could not acquire exclusive lock on lock file");
                    }
                }
            }
        else
            perror("Could not read lock file");

        fclose(lfp);

        return -1;
        }

    /* we have got this far so we have acquired access to the lock file.
        Set a lock on it */

    exclusiveLock.l_type=F_WRLCK; /* exclusive write lock */
    exclusiveLock.l_whence=SEEK_SET; /* use start and len */
    exclusiveLock.l_len=exclusiveLock.l_start=0; /* whole file */
    exclusiveLock.l_pid=0; /* don't care about this */
    lockResult=fcntl(lockFD,F_SETLK,&exclusiveLock);

    if(lockResult<0) /* can't get a lock */
        {
        close(lockFD);
        perror("Can't get lockfile");
        return -1;
        }

    /* now we move ourselves into the background and become a daemon.
     Remember that fork() inherits open file descriptors among others so
     our lock file is still valid */

    curPID=fork();

    switch(curPID)
        {
        case 0: /* we are the child process */
          break;

        case -1: /* error - bail out (fork failing is very bad) */
          fprintf(stderr,"Error: initial fork failed: %s\n",
                     strerror(errno));
          return -1;
          break;

        default: /* we are the parent, so exit */
          exit(0);
          break;
        }

    /* make the process a session and process group leader. This simplifies
        job control if we are spawning child servers, and starts work on
        detaching us from a controlling TTY	*/

    if(setsid()<0)
        return -1;

    /* Note by A.B.: we skipped another fork here */

    /* log PID to lock file */

    /* truncate just in case file already existed */

    if(ftruncate(lockFD,0)<0)
        return -1;

    /* store our PID. Then we can kill the daemon with
        kill `cat <lockfile>` where <lockfile> is the path to our
        lockfile */

    sprintf(pidStr,"%d\n",(int)getpid());

    write(lockFD,pidStr,strlen(pidStr));

    *lockFileDesc=lockFD; /* return lock file descriptor to caller */

    /* close open file descriptors */
    /* Note by A.B.: sysconf(_SC_OPEN_MAX) does work under Linux.
       No need in ad hoc guessing */

    numFiles = sysconf(_SC_OPEN_MAX); /* how many file descriptors? */


    for(i=numFiles-1;i>=0;--i) /* close all open files except lock */
        {
        if(i!=lockFD) /* don't close the lock file! */
            close(i);
        }

    /* stdin/out/err to /dev/null */

    umask(0); /* set this to whatever is appropriate for you */

    stdioFD=open("/dev/null",O_RDWR); /* fd 0 = stdin */
    dup(stdioFD); /* fd 1 = stdout */
    dup(stdioFD); /* fd 2 = stderr */

    /* open the system log - here we are using the LOCAL0 facility */

    openlog(logPrefix,LOG_PID|LOG_CONS|LOG_NDELAY|LOG_NOWAIT,LOG_LOCAL0);

    (void)setlogmask(LOG_UPTO(logLevel)); /* set logging level */

    /* put server into its own process group. If this process now spawns
        child processes, a signal sent to the parent will be propagated
        to the children */

    setpgrp();

    return 0;
}


void StateHandler(int sig){
    gAutoLightOn = !gAutoLightOn;
}

/**************************************************************************/
/***************************************************************************

   ConfigureSignalHandlers

    Set up the behaviour of the various signal handlers for this process.
   Signals are divided into three groups: those we can ignore; those that
   cause a fatal error but in which we are not particularly interested and
   those that are used to control the server daemon. We don't bother with
   the new real-time signals under Linux since these are blocked by default
   anyway.

    Returns: none

***************************************************************************/
/**************************************************************************/

int ConfigureSignalHandlers(void)
{
    struct sigaction		sighupSA,sigusr1SA,sigtermSA;

    /* ignore several signals because they do not concern us. In a
        production server, SIGPIPE would have to be handled as this
        is raised when attempting to write to a socket that has
        been closed or has gone away (for example if the client has
        crashed). SIGURG is used to handle out-of-band data. SIGIO
        is used to handle asynchronous I/O. SIGCHLD is very important
        if the server has forked any child processes. */

    signal(SIGUSR2, StateHandler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGURG, SIG_IGN);
    signal(SIGXCPU, SIG_IGN);
    signal(SIGXFSZ, SIG_IGN);
    signal(SIGVTALRM, SIG_IGN);
    signal(SIGPROF, SIG_IGN);
    signal(SIGIO, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    /* these signals mainly indicate fault conditions and should be logged.
        Note we catch SIGCONT, which is used for a type of job control that
        is usually inapplicable to a daemon process. We don't do anyting to
        SIGSTOP since this signal can't be caught or ignored. SIGEMT is not
        supported under Linux as of kernel v2.4 */

    signal(SIGQUIT, FatalSigHandler);
    signal(SIGILL, FatalSigHandler);
    signal(SIGTRAP, FatalSigHandler);
    signal(SIGABRT, FatalSigHandler);
    signal(SIGIOT, FatalSigHandler);
    signal(SIGBUS, FatalSigHandler);
#ifdef SIGEMT /* this is not defined under Linux */
    signal(SIGEMT,FatalSigHandler);
#endif
    signal(SIGFPE, FatalSigHandler);
    signal(SIGSEGV, FatalSigHandler);
    signal(SIGSTKFLT, FatalSigHandler);
    signal(SIGCONT, FatalSigHandler);
    signal(SIGPWR, FatalSigHandler);
    signal(SIGSYS, FatalSigHandler);

    /* these handlers are important for control of the daemon process */

    /* TERM  - shut down immediately */

    sigtermSA.sa_handler=TermHandler;
    sigemptyset(&sigtermSA.sa_mask);
    sigtermSA.sa_flags=0;
    sigaction(SIGTERM,&sigtermSA,NULL);

    /* USR1 - finish serving the current connection and then close down
        (graceful shutdown) */

    sigusr1SA.sa_handler=Usr1Handler;
    sigemptyset(&sigusr1SA.sa_mask);
    sigusr1SA.sa_flags=0;
    sigaction(SIGUSR1,&sigusr1SA,NULL);

    /* HUP - finish serving the current connection and then restart
        connection handling. This could be used to force a re-read of
        a configuration file for example */

    sighupSA.sa_handler=HupHandler;
    sigemptyset(&sighupSA.sa_mask);
    sighupSA.sa_flags=0;
    sigaction(SIGHUP,&sighupSA,NULL);

    return 0;
}

/**************************************************************************/
/***************************************************************************

   FatalSigHandler

    General catch-all signal handler to mop up signals that we aren't
    especially interested in. It shouldn't be called (if it is it
    probably indicates an error). It simply dumps a report of the
    signal to the log and dies. Note the strsignal() function may not be
   available on all platform/compiler combinations.

    sig			 I					  the signal number

    Returns: none

***************************************************************************/
/**************************************************************************/

void FatalSigHandler(int sig)
{
#ifdef _GNU_SOURCE
    syslog(LOG_LOCAL0|LOG_INFO,"caught signal: %s - exiting",strsignal(sig));
#else
    syslog(LOG_LOCAL0|LOG_INFO,"caught signal: %d - exiting",sig);
#endif

    closelog();
    TidyUp();
    _exit(0);
}

/**************************************************************************/
/***************************************************************************

   TermHandler

    Handler for the SIGTERM signal. It cleans up the lock file and
   closes the server's master socket, then immediately exits.

    sig			 I					  the signal number (SIGTERM)

    Returns: none

***************************************************************************/
/**************************************************************************/

void TermHandler(int sig)
{
    TidyUp();
    _exit(0);
}

/**************************************************************************/
/***************************************************************************

   HupHandler

    Handler for the SIGHUP signal. It sets the gGracefulShutdown and
   gCaughtHupSignal flags. The latter is used to distinguish this from
   catching SIGUSR1. Typically in real-world servers, SIGHUP is used to
   tell the server that it should re-read its configuration file. Many
   important daemons do this, including syslog and xinetd (under Linux).

    sig			 I					  the signal number (SIGTERM)

    Returns: none

***************************************************************************/
/**************************************************************************/

void HupHandler(int sig)
{
    syslog(LOG_LOCAL0|LOG_INFO,"caught SIGHUP");
    gGracefulShutdown=1;
    gCaughtHupSignal=1;

    /****************************************************************/
    /* perhaps at this point you would re-read a configuration file */
    /****************************************************************/

    return;
}

/**************************************************************************/
/***************************************************************************

   Usr1Handler

    Handler for the SIGUSR1 signal. This sets the gGracefulShutdown flag,
   which permits active connections to run to completion before shutdown.
   It is therefore a more friendly way to shut down the server than
   sending SIGTERM.

    sig			 I					  the signal number (SIGTERM)

    Returns: none

***************************************************************************/
/**************************************************************************/

void Usr1Handler(int sig)
{
    syslog(LOG_LOCAL0|LOG_INFO,"caught SIGUSR1 - soft shutdown");
    gGracefulShutdown=1;

    return;
}

/**************************************************************************/
/***************************************************************************

   TidyUp

    Dispose of system resources. This function is not strictly necessary,
   as UNIX processes clean up after themselves (heap memory is freed,
   file descriptors are closed, etc.) but it is good practice to
   explicitly release that which you have allocated.

    Returns: none

***************************************************************************/
/**************************************************************************/

void TidyUp(void)
{
    if(gLockFileDesc!=-1)
        {
        close(gLockFileDesc);
        unlink(gLockFilePath);
        gLockFileDesc=-1;
        }

    if(gMasterSocket!=-1)
        {
        close(gMasterSocket);
        gMasterSocket=-1;
        }
}
