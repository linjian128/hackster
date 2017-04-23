# coding: utf-8
import os
import sys
import time

#import RPi.GPIO as GPIO

HOME = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(HOME)

#from raspi_assistant.settings import GPIOConfig
from raspi_assistant.utils import init_logging_handler
from raspi_assistant.handler import BaseHandler



def test():
    handler = BaseHandler()
    try:
        while True:
		handler.worker()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    #set_GPIO()
    #loop()
    test()
