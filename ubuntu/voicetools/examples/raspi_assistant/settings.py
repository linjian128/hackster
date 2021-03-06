# coding: utf-8
import logging

from voicetools import (
    APIError, RespError, RecognitionError, VerifyError, QuotaError)

# from . import handler
# from .utils import Keyword


class RedisConfig(object):
    """docstring for RedisConfig"""
    HOST_ADDR = 'localhost'
    PORT = 6379
    DB = 0
    SOCKET_TIMEOUT = 1


class BasicConfig(object):
    """Basic config for the raspi_assistant."""
    # Your location, only used in weather queries
    LOCATION = '成都'
    TURING_KEY = '4da1dc534ee8480d9372936269ba428d'
    VOICE_API_KEY = 'C2anyVZMoq61e2vLKyvPTXyP'
    VOICE_SECRET = 'bb1789eb1453d9f908523dd60c84885b'
    HAPPINESS_THRESHOLD = 0.6
    KEYWORDS = {'提醒', '备忘录', '播放', '今天', '明天', '天气', '删除', '最后', '第一条'}
    INPUT_NAME = 'record.wav'
    OUTPUT_NAME = 'output.wav'
    POSITIVE_ANSWER = {
     #   1: 'hello',
         1: '你回来了',
    #    3: '你需要什么',
    #    4: '今天工作辛苦吗',
    #    5: '你今天又变帅了',
    #    6: '需要我帮你做点什么？'
    }
    # The negative_answer won't negative your command, just the emotion expression
    NEGATIVE_ANSWER = {
    #    1: '别吵了，真烦人',
    #    2: '好了好了，我听见了',
    #    3: '我今天心情不太好',
    #    4: '我想静静，也别问我静静是谁',
         1: '每天都来烦我，你什么时候给我找个男朋友'
    }


class GPIOConfig(object):
    """GPIO config"""
    VOICE_SENSOR = 4


class LogConfig(object):
    LOGGING_FORMAT = '%(asctime)s %(funcName)s:%(lineno)d [%(levelname)s] %(message)s'
    LOGGING_LOCATION = './log/raspi_assistant.log'
    LOGGING_LEVEL = logging.DEBUG


class BaiduAPIConfig(object):
    """Baidu API Store is used for weather."""
    API_KEY = 'YOUR_BAIDU_API_KEY'
    WEATHER_URL = 'http://apis.baidu.com/heweather/weather/free'
    TODAY_WEATHER_TEXT = \
    u'当前天气{cond}，体感温度{fl}摄氏度，空气湿度百分之{hum}。今日温度为{min}到{max}摄氏度，{txt_d}转{txt_n}，降水概率百分之{pop}，空气质量{qlty}。'

    TOMO_WEATHER_TEXT = \
        u'明天的气温是{min}到{max}摄氏度，{txt_d}转{txt_n}，降水概率百分之{pop}。'


class ErrNo(object):
    ExceptionMap = {
        3001: QuotaError,
        3002: VerifyError,
        3003: APIError,
    }
