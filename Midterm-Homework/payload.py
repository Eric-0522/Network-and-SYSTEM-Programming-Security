import socket,struct
s=socket.create_connection(('127.0.0.1',9090))
hdr=struct.pack('!IHHI',0xDEADBEEF,1,0,8) # magic=0xDEADBEEF, type=1, flags=0, len=8
s.sendall(hdr+b'ABCDEFGH')
print('sent bad frame')
