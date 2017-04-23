import snowboydecoder
import sys
import signal
import os
import threading
import subprocess 


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

model=sys.argv[1]

def detect_1():
	os.popen('python erji.py snowboy.umdl saved_model.pmdl')

detector = snowboydecoder.HotwordDetector(model, sensitivity=0.5)
print('Listening... Press Ctrl+C to exit')

# main loop
detector.start(detected_callback=detect_1,
               interrupt_check=interrupt_callback,
               sleep_time=0.03)

