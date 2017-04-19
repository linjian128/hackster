import urllib.request

response=urllib.request.urlopen('http://api.yeelink.net/v1.0/device/355777/sensor/402694/datapoints')
html=response.read()
print(html)

