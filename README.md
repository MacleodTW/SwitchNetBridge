# SwitchNetBridge
ESP32 S3 for Nintendo Switch is made from VS Code with PlatformIO.<BR>
It's ported from jackspence7's esp32-bridge.ino and use NAT to reach internet<BR>
<BR>
Switch <---> SwitchNetBridge (Filter DNS, Connection Test) <---> Normal Wireless AP <---> Internet
<BR><BR>

Requirement:<BR>
。ESP32 S3 4MB flash or more
<BR><BR>

Usage:
01. Download release SwitchNetBridge_xxx.zip
02. Flash ESP32-S3 dongle with the tool inside zip
03. Plug ESP32-S3 dongle to recycle power
04. Phone or PC WiFi connect to "SwitchNetBridge" SSID, default password is "password"
05. Use browser to http://192.168.4.1/dns
06. Set Wi-Fi SSID, password, and Relay AP password then click "Save & Reboot"
07. Phone or PC WiFi connect to "SwitchNetBridge" SSID, use new password set on Step 06 to make sure connection is correct
08. Switch wireless connect to SwitchNetBridge
<BR><BR>

Warning:
01. You try this at your own risk.
02. DO NOT REMOVE "nintendo" from Blocked Domains. Use another lines to block other domains
<BR>

Credits:<BR>
。[jackspence7](https://github.com/jackspence7) - [CivSwitch-SerialBridge](https://github.com/jackspence7/CivSwitch-SerialBridge)<BR>
