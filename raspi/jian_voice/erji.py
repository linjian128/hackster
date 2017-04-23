import snowboydecoder
import sys
import signal
import os
import threading



TOP_DIR = os.path.dirname(os.path.abspath(__file__))

RESOURCE_FILE = os.path.join(TOP_DIR, "resources/common.res")
DETECT_DING = os.path.join(TOP_DIR, "resources/ding.wav")
DETECT_DONG = os.path.join(TOP_DIR, "resources/dong.wav")



# Demo code for listening to two hotwords at the same time

interrupted = False
interrupted_1 = False


def signal_handler(signal, frame):
    global interrupted
    interrupted = True


def interrupt_callback():
    global interrupted
    return interrupted

def interrupt_callback_1():
    global interrupted_1
    return interrupted_1

# capture SIGINT signal, e.g., Ctrl+C
signal.signal(signal.SIGINT, signal_handler)

################################################################

models=sys.argv[1:]
sensitivity = [0.5]*(len(models) - 1)

def my_call_bak():
    global interrupted_1
    return interrupted_1

def stop():
    global interrupted_1
    interrupted_1 = True

print 'In Erji ------------'
	#lock.acquire()
detector = snowboydecoder.HotwordDetector(models, sensitivity=sensitivity)
call_backs =[lambda: snowboydecoder.play_audio_file(snowboydecoder.DETECT_DING),
	    # lambda: snowboydecoder.play_audio_file(snowboydecoder.DETECT_DONG)]
	     lambda: stop()]
detector.start(detected_callback=call_backs,
       interrupt_check=my_call_bak,
       sleep_time=0.03)
#lock.release()

detector.terminate()
