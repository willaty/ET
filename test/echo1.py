__author__ = 'yaocoder'
from socket import *

#TOKEN_LENGTH = 5
#TOKEN_STR = "12345"

sfd = socket(AF_INET, SOCK_STREAM)

ip = '127.0.0.1'
port = 12006
sfd.connect((ip, port))

#ch = ['yaocoder', 'wht', 'xty', 'zfd']
#for i in ch:
#message = TOKEN_STR + 'hello world!' + i + '\r\n'
login_message = '||0000011234567800000000'
sfd.send(login_message)

input("input: ")
send_msg = '||0005031234567887654321'
sfd.send(send_msg)

print 'done'

