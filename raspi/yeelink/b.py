import requests
import lxml
import json

url = 'http://api.yeelink.net/v1.0/device/355777/sensor/402694/datapoints'
page = requests.get(url)

print(page.text)


files = {"timestamp":"2017-03-20T09:59:11","value":0}
headers = {"U-ApiKey" :"8f04b76a4f6670ac19bb944d78235708"}

r = requests.post(url,data=json.dumps(files),headers=headers)
