#!/usr/bin/env python
##
##  DVR Alarm ...
##
##  Begun                 2015-04-22
##  Last modified         2015-06-19
##
##  Copyright (c) 2014 Paul Beyleveld. Distribution and modification permitted.
##	GNU General Public License v2.0
##

# THE PROGRAM IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY WARRANTY. 
# IT IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING, 
# BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR 
# PURPOSE. THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE PROGRAM IS WITH YOU. SHOULD
# THE PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF ALL NECESSARY SERVICING, REPAIR OR CORRECTION.
# 
# IN NO EVENT UNLESS REQUIRED BY APPLICABLE LAW THE AUTHOR WILL BE LIABLE TO YOU FOR DAMAGES, 
# INCLUDING ANY GENERAL, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OR 
# INABILITY TO USE THE PROGRAM (INCLUDING BUT NOT LIMITED TO LOSS OF DATA OR DATA BEING RENDERED 
# INACCURATE OR LOSSES SUSTAINED BY YOU OR THIRD PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE 
# WITH ANY OTHER PROGRAMS), EVEN IF THE AUTHOR HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.

## Version history
# 0.2   2015-07-18
#   Implemented external configuration file, streamlined installation
# 0.1   2015-06-19
#   Initial version, buffering 10 sec video and send email with mp4 files on GPIO trigger
##

## Todo List
# 
# Natively implement DVR Streaming, remove zmodopipe
# Use alternative mp4 encapsulation technique, remove ffmpeg requirement
#
##



import os                                   # using os filesystem
import time                                 # time related operations
import sys                                  # for accessing the list of arguments from the commandline
import glob                                 # unix file pattern matching library
import subprocess                           # to spawn new processes
import shutil                               # utility to join movie clips together
import signal
import shlex                                # split strings
import RPi.GPIO as GPIO                     # library for handling Rpi GPIO
import json                                 # for json configuration file
import threading                            # handle multiple threads, used for each channel
import io
import logging                              # library to log to log file
import getopt                               # for parsing command-line options
import termios, tty
from collections import deque

import smtplib                              # libraries required for sending email
from os.path import basename
from email.mime.base import MIMEBase
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText
from email.utils import COMMASPACE, formatdate
from email import encoders

# General Variables
LEVEL = logging.INFO                        # Default Log Level, debug, info, warning, error and critical
DAEMONIZE = False                           # Do not suppress CLI output by default override with -d arg
INIT_C = False                              # Configuration Initialisation flag
PIDS = []                                   # List to keep track of all subprocesses
GPIO.setmode(GPIO.BCM)                      # Rpi GPIO PIN Layout settings
CONFIG = {}                                 # Config variables array

''' Have been implemented in the configuration file
# DVR Config for Zmodopipe
DVR_IP = '192.168.1.4'
DVR_USER = 'admin'
DVR_PASS = 'admin'
DVR_MODEL = '9'
CH_LIST = {7, 1, 2, 3}                       # list of DVR channels to capture and alert on

#SMTP Variables
MAIL_FROM = 'alarm@raspberry.pi'
MAIL_TO = 'paul.beyleveld@gmail.com'
MAIL_BODY = 'CCTV Footage capture seconds before Alarm Trigger occurred'
MAIL_SERVER = '127.0.0.1'
'''

# Other potentially configurable variables
SEG_TIME = 8                                # length in sec of each video segment created
TMP_PATH = '/tmp/dvralert'                  # path to tmp folders
LOGFILE = '/var/log/dvralarm.log'           # Path to logfile
FFMPEG_PATH = '/usr/bin/ffmpeg'             # path to ffmpeg bin
ZMOD = '/usr/bin/zmodopipe'                 # path to zmodopipe bin
CONF_FILE = '/etc/dvralarm/config.json'   # dvralarm config file

def usage():
  """ Print the usage text from the main() docstring."""
  print main.__doc__

class RingBuffer:
    '''
        ringbuffer to keep only last few bytes from h264 stream using array
    '''
    def __init__(self, size):
        self.data = [None for i in xrange(size)]

    def append(self, x):
        self.data.pop(0)
        self.data.append(x)

    def get(self):
        return self.data

class CircularBuffer(deque):
    '''
        Circular buffer to keep only last few bytes from h264 stream using deque
    '''
    def __init__(self, size):
         deque.__init__(self)

         assert size > 0
         self.capacity = size

    def append(self, x):
        while len(self) >= self.capacity:
            self.popleft()
        deque.append(self, x)

    def get(self):
        return self

def send_mail(send_from, send_to, subject, text, files, server):
    '''
        Function to send email with attachments
    '''
    logger.debug('sending mail\nFrom: %s\nTo: %s\nSubject: %s\nText: %s\nServer: %s' % (send_from, send_to, subject, text, server))
    
    msg = MIMEMultipart()
    msg['Subject'] = subject 
    msg['From'] = send_from
    msg['To'] = send_to
    msg.attach(MIMEText(text))

    for f in files:
        
        try:
            part = MIMEBase('application', "octet-stream")
            part.set_payload( open(f,"rb").read() )
            encoders.encode_base64(part)
            part.add_header('Content-Disposition', 'attachment; filename="{0}"'.format(os.path.basename(f)))
            msg.attach(part)
        except Exception:
            msg.attach(MIMEText('\nFailed to attach %s, skipping' % f))
            logger.warning('failed to attach %s, skipping' % f, exc_info=True)
        
        '''
        with open(f, "rb") as fil:
            msg.attach(MIMEApplication(
                fil.read(),
                Content_Disposition='attachment; filename="%s"' % basename(f)
            ))
        '''
        
    try:
        server = smtplib.SMTP(server, 25) #or port 465 doesn't seem to work!
        #server.ehlo()
        #server.starttls()
        #server.login(user, pwd)
        server.sendmail(send_from, send_to, msg.as_string())
        #server.quit()
        server.close()
        #print 'successfully sent the mail'
        logger.info('successfully sent the mail')
    except Exception:
        #print "failed to send mail"
        logger.warning('failed to send mail', exc_info=True)
        #print errtxt
        

def bubble_sort(items):
    """ Implementation of bubble sort """
    for i in range(len(items)):
        for j in range(len(items)-1-i):
            if items[j] > items[j+1]:
                items[j], items[j+1] = items[j+1], items[j]     # Swap!

def ensure_dir(d):
    '''
        Create any required directories if they do not exist
    '''
    if not os.path.exists(d):
        logger.debug('Creating dir %s' % d)
        os.makedirs(d) 

def is_locked(filepath):
    """Checks if a file is locked by opening it in append mode.
    If no exception thrown, then the file is not locked.
    """
    
    ''' Function Usage
    
        wait_time = 5
        while is_locked(filepath):
            print "%s is currently in use. Waiting %s seconds." % \
                  (filepath, wait_time)
            time.sleep(wait_time)
    
    '''
    
    locked = None
    file_object = None
    if os.path.exists(filepath):
        try:
            #print "Trying to open %s." % filepath
            logger.debug('Trying to open %s.' % filepath)
            buffer_size = 8
            # Opening file in append mode and read the first 8 characters.
            file_object = open(filepath, 'a', buffer_size)
            if file_object:
                #print "%s is not locked." % filepath
                logger.debug('%s is not locked.' % filepath)
                locked = False
        except IOError, message:
            #print "File is locked (unable to open in append mode). %s." % \
                #message
            logger.debug('File is locked (unable to open in append mode).', exc_info=True)
            locked = True
        finally:
            if file_object:
                file_object.close()
                #print "%s closed." % filepath
    else:
        #print "%s not found." % filepath
        logger.warning('%s not found.' % filepath)
    return locked

def backupVid(chl, ch_files, incident): # redundant because of ringbuffer implementation
    '''
        This function will find two latest captures files for each channel and
        copy to single joined video, ready to attached to email alert, 
    '''
    
    '''
    OS-independent way: if it is safe to assume that the legacy application keeps the file open for at most some known quantity of time, say T seconds (e.g. opens the file, performs one write, then closes the file), and re-opens it more or less every X seconds, where X is larger than 2*T.

    =stat the file
    =subtract file's modification time from now(), yielding D
    =if T <= D < X then open the file and do what you need with it
    =This may be safe enough for your application. Safety increases as T/X decreases.
    
    If 5 second segments, and keep 4 times segments
    T = 6
    X = ((4-1)*5) = 15
    6 <= D < 15
    '''
    
    ifiles = []
    waittime = 0
    
    for ch in chl:
        #print '-1 mtime: %s \tctime: %s'% (os.stat(ch_files[ch][-1]).st_mtime, os.stat(ch_files[ch][-1]).st_ctime)
        #print '-2 mtime: %s \tctime: %s'% (os.stat(ch_files[ch][-2]).st_mtime, os.stat(ch_files[ch][-2]).st_ctime)
        
        D = time.time() - os.stat(ch_files[ch][-1]).st_ctime            # time lapsed since Last modified time current file
        #T = SEG_TIME                                                    # time period writing a segment
        X = ((SEG_TIME*4)-SEG_TIME)                                   # time before earliest next file write
        ifiles.append('%s/%s_ch0%s.mp4' \
                % (TMP_PATH,time.strftime("%Y-%m-%d_%H-%M-%S"),ch))
        inputs = {ch_files[ch][-1], ch_files[ch][-2]}
        
        print "CH%s D: %.0f, X: %.0f" % (ch, D, X)
    
        # latest two videos should always contain a part of the incident on short periods
        # so simply wait for latest segment to complete and use two most recent files
        if waittime > SEG_TIME+2:
            joinvid(ifiles[-1],inputs,ch)
        else:        
            while not (D > 0 and D < X):
                print "CH%s ffmpeg still writing last file" % ch
                print "Waiting %s seconds." % 1
                print "CH%s D: %.0f, X: %.0f" % (ch, D, X)
                waittime += 1
                time.sleep(1)
                D = time.time() - os.stat(ch_files[ch][-1]).st_ctime
        
            while is_locked(ch_files[ch][-1]) and D < X:
                print 'CH%s File still locked, waiting...' % ch
                print "CH%s D: %.0f, X: %.0f" % (ch, D, X)
                time.sleep(1)
                waittime += 1
                D = time.time() - os.stat(ch_files[ch][-1]).st_ctime
            
            joinvid(ifiles[-1],inputs,ch)
                
            #waittime = time.time() - incident    
  
            #print 'CH%s WARNING: took too long to join files already overwritten' % ch
    
    
    ''' Previous function start
    for ch in chl:
        print '-1 mtime: %s \tctime: %s'% (os.stat(ch_files[ch][-1]).st_mtime, os.stat(ch_files[ch][-1]).st_ctime)
        print '-2 mtime: %s \tctime: %s'% (os.stat(ch_files[ch][-2]).st_mtime, os.stat(ch_files[ch][-2]).st_ctime)
        touchtime = os.stat(ch_files[ch][-2]).st_ctime  # Last modified time previous file = start time of current
                                                        # On OS X .getctime() gives the modified date
                                                        # On Linux .getctime() gives the modified date
                                                        # from what I read getctime() will not work on windows
        idiff = (incident - touchtime)                  # time difference between incident and starting current file
        cdiff = (time.time() - incident)                # time different between incident and now
        print "CH%s idiff: %.0f" % (ch, idiff)
        print "CH%s cdiff: %0.f" % (ch, cdiff)
    
        # latest two videos should always contain a part of the incident on short periods
        # so simply wait for latest segment to complete and use two most recent files
        if idiff <= (SEG_TIME) and cdiff <= (SEG_TIME)and waittime < (SEG_TIME - idiff):                   # latest video contains less than 4 seconds since incident
            print "CH%s ffmpeg still writing last file partially containing incident" % ch
            print "waiting %.0f second to complete" % (SEG_TIME + 1 - idiff)
            time.sleep(SEG_TIME + 1 - idiff)                        # sleep to complete last file
            waittime += SEG_TIME + 1 - idiff
            
            ifiles.append('%s/%s_ch0%s.mp4' % (TMP_PATH,time.strftime("%Y-%m-%d_%H-%M-%S"),ch))

            inputs = {ch_files[ch][-1], ch_files[ch][-2]}
            joinvid(ifiles[-1],inputs,ch)
            
        elif idiff <= (2*SEG_TIME):                                # latest video only contains 3 seconds before alarm trigger use 2nd oldest video
            print "CH%s grabbing last two files containing incident for channel" % ch
            
            ifiles.append('%s/%s_ch0%s.mp4' % (TMP_PATH,time.strftime("%Y-%m-%d_%H-%M-%S"),ch))
            
            inputs = {ch_files[ch][-1], ch_files[ch][-2]}
            joinvid(ifiles[-1],inputs,ch)
            
        else:
            print "CH%s WARNING: please check to ensure ffmpeg is streaming files!" % ch
      End'''   
    ## usage to combine 4 mp4 files into single mp4 using filter overleys
    ##./ffmpeg -i output020.mp4 -i output021.mp4 -i output022.mp4 -i output023.mp4 -filter_complex "[0:0]pad=iw*2:ih*2[a];[a][1:0]overlay=w[b];[b][2:0]overlay=0:h[c];[c][3:0]overlay=w:h[out]" -map "[out]" -shortest output.mp4

def become_daemon():
    '''
        Method to create a unix daemon with double fork
    '''
    pid = os.fork()
    if pid == 0:
        # This is the child of the fork

        # Become a process leader of a new process group
        os.setsid()

        # Fork again and exit this parent
        pid = os.fork()
        if pid == 0:
            # This is the child of the second fork -- the running process.
            pass
        else:
            # This is the parent of the second fork
            # Exit to prevent zombie process
            os._exit(0)
    else:
        # This is the parent of the fork
        os._exit(0)

def clean_processes(PROCS):
    '''
        method to clean up any running child processes
    '''
    for proc in PROCS:
        if proc.poll() == None:
            #print 'Killing: %s' % proc.pid
            logger.debug('Killing sub-process: %s' % proc.pid)
            os.killpg(proc.pid, signal.SIGTERM)
    logger.info('Completed cleaning up sub-processes')    

def transcodeVid(inputf):
    '''
        Function to encaptulate raw h264 streams with mp4 container
        File created from ringbuffer can be transcoded using the following ffmpeg command.
        ffmpeg -f h264 -i /tmp/dvralert/2015-05-03_14-10-52_ch01.h264 -reset_timestamps 1 -c copy -an /tmp/dvralert/2015-05-03_14-10-52_ch01.mp4
        ffmpeg -f h264 -i /tmp/test.h264 -reset_timestamps 1 -y -c copy -an /tmp/test.mp4
    
    '''
    dst = []
    
    #print inputf
    logger.debug('Transcoding captured h264 files to mp4')
    logger.debug(inputf)
    
    for fi in inputf:                        # Run ffmpeg for each channel
    
        file = os.path.splitext(fi)
        dst.append('%s.%s' % (file[0], 'mp4'))
        #print dst
    
        ffmpeg = '%s -f h264 -i %s -reset_timestamps 1 -y -c copy -an %s' % (FFMPEG_PATH, fi, dst[-1])
    
        command = shlex.split(ffmpeg)       # split str by spaces for Popen    
    
        try:
            #print 'Spawning: %s' % ffmpeg
            logger.debug('Spawning: %s' % ffmpeg)

            PIDS.append(subprocess.Popen(command, shell=False, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE))
            PIDS[-1].wait()
            stdout, stderr = PIDS[-1].communicate()
            if PIDS[-1].returncode != 0:
                #print '\tstderr: ', repr(stderr)
                #print 'failed to transcode alarm video %s' % dst[-1]
                logger.debug('ffmpeg output:\n', repr(stderr))
                logger.error('failed to transcode alarm video %s' % dst[-1])
            else:
                #print 'completed transcoding %s' % (dst[-1])
                logger.info('Completed transcoding %s' % (dst[-1]))
                #break
        
        except Exception:
            #print 'cannot spawn ffmpeg to transcode %s' % fi          
            logger.debug('%s' % ffmpeg)
            logger.error('cannot spawn ffmpeg to transcode %s' % fi, exc_info=True)
    
        if not is_locked(fi):
            logger.debug('deleting temp file %s' % fi)
            if LEVEL != logging.DEBUG: os.remove(fi)
    
    send_mail(CONFIG['MAIL_FROM'], CONFIG['MAIL_TO'], 'DVR Alarm %s' \
        % time.strftime("%Y-%m-%d_%H-%M-%S"), CONFIG['MAIL_BODY'], dst, CONFIG['MAIL_SERVER'])
    
def buildAlert(alarm_detected):
    '''
    Function to find latest video files and send for transcoding
    '''
    
    outf = []
    ch_files = {}
    
    # capture the time when the alarm sounds = eventtime
    eventtime = time.time()
    #print 'Alarm Time: %s' % eventtime
    logger.info('Alarm Time: %s' % time.strftime("%Y/%m/%d %H:%M:%S"))
    
    alarm_detected.set()
    
    while alarm_detected.is_set():
        time.sleep(0.1)                                           # wait for buffer to complete saving
    
    # get all files related to each channel and sort according to modified date.
    for ch in CH_LIST:
        ch_files[ch] = [file for file in glob.glob("%s/*ch0%s.h264" % (TMP_PATH,ch))]
        if len(ch_files[ch]) > 1:
            ch_files[ch].sort(key=os.path.getctime)             # be careful this must be modification time
        #else:
        #    print 'Warning: There is only 1 recording file available for channel %s' % ch
    
    
    #print ch_files                                             # print sorted file matrix
    for ch in CH_LIST:
        logger.debug('CH%s files found in tmp' % ch)
        for chf in ch_files[ch]:
            #print 'CH%s:\t%s\t%.0f' %(ch, chf, os.stat(chf).st_ctime)
            logger.debug('CH%s:\t%s\t%.0f' %(ch, chf, os.stat(chf).st_ctime))
        outf.append(chf)                                        # keep the latest files of each channel
    
    transcodeVid(outf)

def exit(work_completed):
    ''' function to notify all threads to finish processing '''
    work_completed.set()                                # Notify threads to finish processing
    pass

def readBuffer(ch, alarm_detected, work_completed):
    '''
    Function to continually read named pipe into ringbuffer and save to file  when alarm is triggered
    '''
    
    #buf = RingBuffer(12288)                            # ringbuffer less effective than circular buffer using dequeue
    buf = CircularBuffer(320*SEG_TIME)                    # 300 * 32 bytes ~ 1 sec video
    zpipe = '/tmp/zmodo%s' % (ch-1)
    blocksize = 32
    
    while not work_completed.is_set():                  # exit here when we close the program
        if os.path.exists(zpipe):
            try:
                with open(zpipe, 'rb') as fh:
                    #print threading.currentThread().getName(), 'started'
                    logger.debug('%s thread started' % threading.currentThread().getName())
                    
                    while not alarm_detected.is_set() and not work_completed.is_set():      # exit here when we raise an alarm
                        block = fh.read(blocksize)
                        '''
                        # perform some cleanup of the received h264 stream
                        for ch in block:
                            str += hex(ord(ch))+" "
                        print str
                        #buf.append(fh.read(32))            # 32 seems to be a happy medium value between read wait and CPU usage
                        '''
                        buf.append(block)

            except Exception:
                #print errtxt
                logger.error('Cannot read data from %s confirm zmodopipe is running and streaming video' % zpipe, exc_info=True)
            
            if work_completed.is_set(): break           # avoid writing buffer on normal exit
            
            ## write buffer to file when alarm_detected.is_set ##
            ofile = '%s/%s_ch0%s.h264' \
                    % (TMP_PATH,time.strftime("%Y%m%d_%H%M%S"),ch)
            try:
                with open(ofile, 'wb') as fo:
                    bytearray = buf.get()
                    #printBuf(bytearray)
            
                    for byte in bytearray:              # sequentially read ringbuffer and write to file
                        if byte != None:
                            fo.write(byte)
                
                time.sleep(1)                            # wait 1 second, should be enough time for all threads to complete action
                alarm_detected.clear()                  # reset alarm trigger since file is captured
                
            except Exception:
                    #print errtxt
                    logger.error('Cannot write ringbuffer data to file %s' % ofile, exc_info=True)
        
        else:
            time.sleep(0.5)                             # sleep if the zmodopipe is not yet available
        
    #print threading.currentThread().getName(), 'closed'
    logger.info('CH%s stopped readbuffer thread' % ch)
    logger.debug('%s thread closed' % threading.currentThread().getName())

def main(IS_DAEMON):
    '''DVRAlarm Alarm PGM CCTV Integration
    
    By default dvralarm is started in user interactive mode use CTRL-C to interrupt
    We spawn the zmodopipe process, read output into ringbuffer and wait for a GPIO or CLI trigger
    When we receive a trigger event we save video buffer and send an email alert
    
    Usage: dvralarm [OPTION]
    
    OPTIONS
    -d, --daemonize         Run dvralarm with no CLI output, CTRL-C to exit
    -v, --verbose           Enable debug level logging.
    -h, --help              Output this command usage message and exit.
    
    '''
    
    logger.info('### Starting dvralarm ###')
    logger.debug('DEBUG logging Enabled')
    if IS_DAEMON: logger.info('Daemonize mode enabled')
    
    
    # set session ID to this process so we can kill group in sigterm handler
    #become_daemon()
    #os.setpgrp()
    
    ## setup threading events
    alarm_detected = threading.Event()
    work_completed = threading.Event()
    
    # GPIO 23 set up as input. It is pulled up to stop false signals  
    GPIO.setup(23, GPIO.IN, pull_up_down=GPIO.PUD_UP)
    # GPIO 24 set up as input. It is pulled up to stop false signals  
    GPIO.setup(24, GPIO.IN, pull_up_down=GPIO.PUD_UP)
    
    # Add interrupt driven input detect
    GPIO.add_event_detect(23, GPIO.FALLING, callback=lambda x: buildAlert(alarm_detected), bouncetime=2000)
    # Add interrupt driven input detect
    GPIO.add_event_detect(24, GPIO.FALLING, callback=lambda x: exit(work_completed), bouncetime=2000)
    
    # Interrupt signal handler
    #signal.signal(signal.SIGTERM, sigterm_handler)
    
    # Setup working directories
    ensure_dir(TMP_PATH)

    
    '''
    ## Start by spawning zmodopipe
    # ./zmodopipe -s <dvr_ip> -u <user> -a <pass> -c <num> -c <num> -v -m <dvr_model>
    '''

    cstr = ''
    for ch in CH_LIST: cstr += "-c %s " % ch
    zmodopipe = '%s -s %s -u %s -a %s %s-m %s' % (ZMOD ,CONFIG['DVR_IP'], CONFIG['DVR_USER'], CONFIG['DVR_PASS'], cstr, CONFIG['DVR_MODEL'])
    #print 'Main Spawning: %s' % zmodopipe
    logger.info('Launching zmodopipe')
    logger.debug('Main Spawning: %s' % zmodopipe)
    
    command = shlex.split(zmodopipe)        # split str by spaces for Popen
    
    try:
        PIDS.append(subprocess.Popen(command, shell=False, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, close_fds=True, preexec_fn=os.setpgrp))
    except Exception:
        #print 'Cannot spawn zmodopipe!\nNo reason to continue exiting.'
        logger.error('Cannot spawn zmodopipe!\nNo reason to continue exiting.', exc_info=True)
        sys.exit()


    '''
    ## Spawn a ffmpeg instance for every channel to capture
    # ./ffmpeg -f h264 -i /tmp/zmodo0 -reset_timestamps 1 -vcodec h264 -r 4 -b:v 50k -f segment -segment_time 5 -segment_wrap 4 /tmp/dvralert/buffer/ch01_%d.mp4
    '''
    
    
    for ch in CH_LIST:
        '''
        # Spawn a thread for each channel to read zmodopipe h264 stream into ring buffer
        '''
        try:
            t = threading.Thread(name='CH%s_RingBuf' % ch, target=readBuffer, args = (ch, alarm_detected, work_completed))
            t.start()
            #print 'CH%s starting readbuffer thread' % ch
            logger.info('CH%s starting readbuffer thread' % ch)
        except Exception:
            #print errtxt
            logger.error('CH%s cannot spawn readbuffer thread' % ch, exc_info=True)

    '''
        Main loop
    '''
    if not IS_DAEMON: print 'Entering main loop, press [Ctrl-c] Menu'
    
    while True:
            
        try:

            # Structure for user benefit/logging only, GPIO events are handled through interrupts
            if GPIO.event_detected(23) == True:
                #print 'GPIO Alarm detected!'
                logger.info('GPIO Alarm detected!')
            if GPIO.event_detected(24) == True:
                #print 'GPIO Trigger exiting..'
                logger.info('GPIO Trigger exiting..')
                break

            time.sleep(1)
                
        except KeyboardInterrupt:
            if IS_DAEMON: break
            if not IS_DAEMON: print '[x] Exit, [a] Alarm, [c] Continue'
            fd = sys.stdin.fileno()
            old_settings = termios.tcgetattr(fd)
            try:
                tty.setraw(sys.stdin.fileno())
                cmd = sys.stdin.read(1)
            finally:
                termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
            
            if cmd == 'a':
                if not IS_DAEMON: print 'Alarm triggered from CLI!'
                buildAlert(alarm_detected)
            elif cmd == 'x':
                if not IS_DAEMON: print 'Exiting...'
                break
        except:
            e = sys.exc_info()[0]
            #print( 'Error: %s' % e )
            logger.error('Main loop exception', exc_info=True)
    
    ## Clean up any child processes
            
    #print 'cleaning up all child processes'
    logger.info('Cleaning up all child processes')
    work_completed.set()
    clean_processes(PIDS)
    GPIO.cleanup()          # clean up GPIO on normal exit
    
    logger.info('### dvralarm exited ###')

    # try to clean temporary buffer files
    #shutil.rmtree(TMP_PATH, ignore_errors=True)

def create_config():
    ''' 
        Function to create new configuration file 
    '''
    
    global CONF_FILE
    global CONFIG
    
    while True:
        print '\n## Initial configuration Wizard ##'
        print '# enter on blank line will use [default] value #\n'
        # Log Level, debug, info, warning, error and critical
        
        DVR_IP = raw_input("DVR IP Address [192.168.1.20]: ") or '192.168.1.20'
        DVR_USER = raw_input("DVR Username [admin]: ") or 'admin'
        DVR_PASS = raw_input("DVR Password [admin]: ") or 'admin'
        DVR_MODEL = int(raw_input("DVR Zmodopipe Model [9]: ") or '9')
        CH_LIST = raw_input("Comma Separated list of DVR channels [1,2,3,4]: ") or '1,2,3,4'
        MAIL_SERVER = raw_input("SMTP Relay [127.0.0.1]: ") or '127.0.0.1'
        MAIL_FROM = raw_input("SMTP Mail From [alarm@raspberry.pi]: ") or 'alarm@raspberry.pi'
        MAIL_TO = raw_input("SMTP Mail To [admin@example.com]: ") or 'admin@example.com'
        LEVEL = 'logging.' + (raw_input("Logging level (DEBUG|[INFO]|WARNING|ERROR|CRITICAL): ") or 'INFO')
        
        print '\n## Verify your configuration variables ##'
        print 'DVR_IP: %s' % DVR_IP, '\nDVR_USER: %s' % DVR_USER, '\nDVR_PASS: %s' % DVR_PASS, \
            '\nDVR_MODEL: %s' % DVR_MODEL, '\nCH_LIST: %s' % CH_LIST, '\nMAIL_SERVER: %s' % MAIL_SERVER, \
            '\nMAIL_FROM: %s' % MAIL_FROM, '\nMAIL_TO: %s' % MAIL_TO, '\nLogging level: %s' % LEVEL
        response = raw_input("\nSave configuration to file? (y/n): ")
        
        while response != 'y' and response != 'n':
            print '   Invalid input, type "y" or "n"'
            response = raw_input("Save configuration to file? (y/n): ")
        
        if response == 'y': break
        
    MAIL_BODY = 'DVR Alarm generated footage prior to Alarm trigger.'
    
    CONFIG = {'DVR_IP': DVR_IP, 'DVR_USER': DVR_USER, 'DVR_PASS': DVR_PASS, \
            'DVR_MODEL': DVR_MODEL, 'CH_LIST': CH_LIST, 'MAIL_SERVER': MAIL_SERVER, \
            'MAIL_FROM': MAIL_FROM, 'MAIL_TO': MAIL_TO, 'MAIL_BODY': MAIL_BODY, 'LEVEL': LEVEL} 

    ensure_dir(os.path.dirname(CONF_FILE))
    
    try:
        with open(CONF_FILE, 'w') as f:
            json.dump(CONFIG, f, indent=0, sort_keys=True)
    except Exception:
        logger.error('Failed to create config file ', exc_info=True)
        print 'Failed to create config file, view log for more detail.'
        sys.exit()
    
    print 'Successfully created config file %s.' % CONF_FILE
    sys.exit()


if __name__ == '__main__':      ## main function of the application
    
    if not os.geteuid() == 0:
        sys.exit('Script must be run as root')
    
    # Trap the input arguments.
    try:
        opts, args = getopt.getopt(sys.argv[1:], 'hvdi', # Basic I/O arguments, arguments followed by : expect argument.
                ['help', 'verbose','daemonize'])
        #print opts, args
    except getopt.GetoptError as e:
        print '%s\n' % e
        usage()
        sys.exit(2)
        
    # Walk through the argument list...
    for opt, arg in opts:
        # First, look for help and version requests, exit if found.
        if opt in ('-h', '--help'):
            usage()
            sys.exit()
        elif opt in ('-v', '--verbose'):
            LEVEL = logging.DEBUG
        elif opt in ('-d', '--daemonize'):
            DAEMONIZE = True
            #os.setpgrp()
        elif opt in ('-i'):
            INIT_C = True
    
    
    # setup logger 
    logger = logging.getLogger(__name__)
    logger.setLevel(LEVEL)

    # create a file handler
    handler = logging.FileHandler(LOGFILE)
    handler.setLevel(LEVEL)

    # create a logging format
    formatter = logging.Formatter('%(asctime)s - %(name)s - %(levelname)s - %(message)s')
    handler.setFormatter(formatter)

    # add the handlers to the logger
    logger.addHandler(handler)
    
    
    # read or create configuration file
    if not os.path.isfile(CONF_FILE):
        if DAEMONIZE: 
            logger.error('Missing configuration file: %s' % CONF_FILE)
            logger.info('run without -d argument to create configuration file using wizard, exiting...')
            sys.exit()
        print 'Config file not found, running initial configuration wizard'
        try:
            create_config()
        except KeyboardInterrupt:
            sys.exit()
    elif INIT_C and not DAEMONIZE:
        print 'Init flag set, config file already exist so exiting...'
        sys.exit()
    else:
        try:
            logger.debug('Loading configuration from file %s' % CONF_FILE)
            with open(CONF_FILE) as cfg:            # load configuration file
                CONFIG = json.load(cfg)
            logger.debug('Successfully read config file')
        except:
            logger.error('Unable to open %s, creating default config file. \
            Please check configuration file permissions and re-run the script.' % CONF_FILE)
            #if not DAEMONIZE: create_config()
            sys.exit()

    logger.setLevel(eval(CONFIG['LEVEL']))
    handler.setLevel(eval(CONFIG['LEVEL']))
    CH_LIST = [ int(e) for e in CONFIG['CH_LIST'].split(',') ]
    
    #sys.exit()                      # Temporary system exit to test config file unit
    
    # call main program
    main(DAEMONIZE)
