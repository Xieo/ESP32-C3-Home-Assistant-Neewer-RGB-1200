Hi All,

I've created this to control the Neewer RGB1200 Light from Home Assistant via ESPHome.

All you need to do is place your Light MAC Address in the main .yaml file. you can add your encryption 
Add your local wifi credentials if you don't already use the below code with your ESPHome.
  ssid: !secret wifi_ssid
  password: !secret wifi_password

  Feel free to add you comments or updates but let me know :)
  I may add the Neewer CCT660 later.
